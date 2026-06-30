// ============================================================================
// ComparisonSession.cpp — 交互式比对会话实现（diff 链路：会话 + 暂存 + 门控）
// ============================================================================
//
// 【本文件实现了什么】IComparisonSession 的完整落地（ComparisonSession 类），外加文件
//   末尾的工厂 createComparisonSession()。一次会话把「算差异 → 交互合并 → 写回本地库」
//   串成一条流水线，四个协作者各司其职（DiffEngine 算差异、StagingBuffer 暂存决策、
//   InboundTableGate 门控 inbox、SyncContext 的 worker 写函数落库）。
//
// 【阅读顺序建议】先看 initialize（建立读快照/缓存/门控的所有前提），再看 save（收尾时
//   stale 检测 + 落库 + 释放资源，是最综合的一段），最后看各 stage*/accept* 小方法。
//
// 【贯穿全文件的三个不直观机制（务必先建立直觉）】
//   ① 读事务钉快照（H-7）：WAL 模式下「裸 SELECT」只在单条查询期间持有快照，跨多条查询
//      会各看各的版本。本会话用 BEGIN DEFERRED 把一个一致快照钉在 rconn_ 上，保证整场
//      比对（多次 rowDiffs/findLocalRow）看到的本地数据是同一时刻的，不被后台写入搅动。
//   ② stale 检测：PRAGMA data_version 是 SQLite 维护的「本连接之外的写入计数」，库每被
//      改一次它就变。initialize 记下钉住值，save 前再读一次，不等就判暂存过期、拒绝写回。
//   ③ 门控开闭：initialize 开门控冻结被比对表的 inbox 应用，save/discard 释放并 rescan。
// ============================================================================

#include "ComparisonSession.h"

#include "dbridge/Errors.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

#include "sql/SqlBuilder.h"
#include "sync/WriteTxn.h"
#include <algorithm>
#include <memory>

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// Constructor
// 构造函数：仅做依赖注入（成员初始化列表绑定各引用与值），不做任何 I/O。
//   真正建立读快照/算差异/开门控全部推迟到 initialize()，使「构造」廉价且不可失败。
//   注意 context 以 std::move 接管 shared_ptr 所有权（引用计数 +1，会话期内保活上下文）。
// ---------------------------------------------------------------------------

ComparisonSession::ComparisonSession(QSqlDatabase& rconn, QSqlDatabase& wconn, TableStateStore& ts,
                                     DiffEngine& diff, InboundTableGate& gate,
                                     UpsertExecutor& upsert, qint64 streamEpoch,
                                     std::shared_ptr<SyncContext> context)
    : rconn_(rconn),
      wconn_(wconn),
      ts_(ts),
      diff_(diff),
      gate_(gate),
      upsert_(upsert),
      streamEpoch_(streamEpoch),
      context_(std::move(context)) {
}

// ---------------------------------------------------------------------------
// initialize
// 初始化是整场会话的「立基」步骤：钉读快照 → 缓存对端数据 → 算表级差异 → 开门控。
// 这里有两个重载：公共重载（吃对外快照类型，做类型转换）转调内部重载（干实事）。
// ---------------------------------------------------------------------------

// C-10 fix: public API overload — convert RemoteTableSnapshot to internal types.
// C-10 修复：公共 API 重载——把对外的 RemoteTableSnapshot 拆成内部三件套后转调内部重载。
//   为什么要这层转换：对外接口用 IComparisonSession.h 里的快照结构（schemaFingerprint/
//   contentChecksum 等长名字段、rows 一体），而内部 DiffEngine 用更紧凑的 RemoteMeta +
//   「表→行集」哈希。这层只做字段搬运，不含逻辑。
bool ComparisonSession::initialize(const QList<RemoteTableSnapshot>& remoteSnapshots,
                                   QString* err) {
    QStringList tables;                                  // 参与比对的表名（保序）
    QHash<QString, DiffEngine::RemoteMeta> remoteMetas;  // 表→对端表级三元组
    QHash<QString, QList<QVariantMap>> remoteRows;  // 表→对端行集合（惰性表则缺席）

    for (const RemoteTableSnapshot& snap : remoteSnapshots) {
        tables.append(snap.table);
        DiffEngine::RemoteMeta meta;
        meta.schemaFp = snap.meta.schemaFingerprint;  // 表结构指纹（字段改名搬运）
        meta.checksum = snap.meta.contentChecksum;    // 内容校验和
        meta.rowCount = snap.meta.rowCount;           // 行数
        remoteMetas.insert(snap.table, meta);
        // 仅当快照真带了行才登记行集；空 rows 表示「惰性」——稍后由 fetchRemoteRows 拉取，
        // 不在此处塞一个空列表（这样下游可用 contains() 区分「无此表」与「有表但行惰性」）。
        if (!snap.rows.isEmpty())
            remoteRows.insert(snap.table, snap.rows);
    }

    return initialize(tables, remoteMetas, remoteRows, err);
}

// 内部重载：真正建立会话状态。注意各步骤顺序有意为之（先钉快照再算差异，保证差异基于
// 被钉住的那一刻），任一前置步骤失败即提前返回 false（此时门控尚未开、无需清理）。
bool ComparisonSession::initialize(const QStringList& tables,
                                   const QHash<QString, DiffEngine::RemoteMeta>& remoteMetas,
                                   const QHash<QString, QList<QVariantMap>>& remoteRows,
                                   QString* err) {
    // H-7 fix: pin a real read snapshot by opening an explicit read transaction.
    // A bare SELECT in autocommit mode does NOT hold a snapshot across multiple queries —
    // SQLite in WAL mode only pins the snapshot for the duration of the query itself.
    // BEGIN DEFERRED pins the read snapshot for the life of the transaction on rconn_.
    // Note: rconn_ is a dedicated read-only connection, so BEGIN DEFERRED is safe here
    // and does not conflict with the write connection held by SyncWorker.
    // ───────────────────────────────────────────────────────────────────────
    // H-7 修复：用一个显式读事务「钉住一份真正的读快照」。
    //   关键背景：自动提交（autocommit）模式下的裸 SELECT 不会跨多条查询持有快照——WAL
    //   模式的 SQLite 只在「单条查询执行期间」锁定快照。于是若不开事务，本会话两次
    //   rowDiffs/findLocalRow 之间，后台一旦写入，第二次就会看到新版本，画面前后不一致。
    //   QSqlDatabase::transaction() 在此连接上发出 BEGIN（DEFERRED 语义：首条读语句执行时
    //   才真正取得读快照，并把它钉到整个事务的生命周期），于是整场比对看到同一时刻的本地库。
    //   安全性：rconn_ 是本会话专用的只读连接，在它上面 BEGIN 不会与 SyncWorker 持有的写
    //   连接相互阻塞（读写各自独立的连接 + WAL 允许读写并发）。
    if (!rconn_.transaction()) {
        if (err)
            *err = QStringLiteral("ComparisonSession: cannot start read transaction: %1")
                       .arg(rconn_.lastError().text());
        return false;  // 连读事务都开不起来 → 初始化失败（门控未开、无需回滚）
    }
    readTxnActive_ = true;  // 标记读事务已打开，save/discard 收尾时据此回滚释放

    // 记下「此刻」的 data_version 作为 stale 检测基准。<=0 表示读取失败（err 已被写入）。
    // 失败时直接返回：注意此处虽已开了读事务，但留给上层 discard 时统一回滚（readTxnActive_
    // 已置 true），本函数不在此中途回滚以保持单一收尾路径。
    pinnedDataVersion_ = readDataVersion(err);
    if (pinnedDataVersion_ <= 0)
        return false;

    // Build remoteData_ cache.
    // 构建对端数据缓存 remoteData_：把每张表的 meta 与（可选的）行集合搬进会话私有缓存，
    // 之后 rowDiffs/findRemoteRow/stageTable/fetchRemoteRows 都从这里读对端侧，比对期内只读。
    for (const QString& t : tables) {
        RemoteTableData rd;
        if (remoteMetas.contains(t))
            rd.meta = remoteMetas[t];
        if (remoteRows.contains(t))
            rd.rows = remoteRows[t];  // 惰性表此处 rows 留空，待 fetchRemoteRows 时已具备
        remoteData_.insert(t, rd);
    }

    // Compute table-level diffs.
    // 算表级差异：把本地 TableStateStore（按 streamEpoch_ 定位的状态）与对端 remoteMetas
    // 逐表比对三元组，得出每表 Identical/Different/OnlyLocal/OnlyRemote + 增删改计数。
    // 结果缓存进 diffs_，tableDiffs() 之后只是直接返回它（不重算）。
    diffs_ = diff_.tableDiffs(rconn_, tables, streamEpoch_, ts_, remoteMetas);

    // Open the gate to defer inbound changes to these tables.
    // 开门控：从此刻起冻结针对这些被比对表的 inbox 入站应用，防止比对期间底层数据被搅动
    // （进而避免 save 时 stale、或用户基于过期画面决策）。释放在 save/discard 时进行。
    gate_.open(tables);

    return true;
}

// ---------------------------------------------------------------------------
// tableDiffs / rowDiffs
// ---------------------------------------------------------------------------

QList<TableDiff> ComparisonSession::tableDiffs() const {
    return diffs_;
}

QList<RowDiff> ComparisonSession::rowDiffs(const QString& table, int offset, int limit) const {
    if (!remoteData_.contains(table))
        return {};
    return diff_.rowDiffs(rconn_, table, remoteData_[table].rows, offset, limit);
}

// ---------------------------------------------------------------------------
// stageRow / stageTable / unstage
// ---------------------------------------------------------------------------

bool ComparisonSession::stageRow(const QString& table, const QString& pk) {
    QVariantMap row = findRemoteRow(table, pk);
    if (row.isEmpty())
        return false;
    staging_.stage(table, pk, row);
    return true;
}

bool ComparisonSession::stageTable(const QString& table) {
    if (!remoteData_.contains(table))
        return false;

    // Find the table diff to limit work to Different rows.
    QString pkCol = getPkColumn(table);

    // For each remote row that differs from local, stage it.
    const QList<QVariantMap>& remoteRows = remoteData_[table].rows;
    QList<RowDiff> diffs = diff_.rowDiffs(rconn_, table, remoteRows, 0, -1);

    for (const RowDiff& rd : diffs) {
        if (rd.kind == RowDiffKind::Same)
            continue;
        QVariantMap row = findRemoteRow(table, rd.primaryKey);
        if (!row.isEmpty())
            staging_.stage(table, rd.primaryKey, row);
    }
    return true;
}

bool ComparisonSession::unstage(const QString& table, const QString& pk) {
    staging_.unstage(table, pk);
    return true;
}

// ---------------------------------------------------------------------------
// acceptLocal / acceptRemote
// ---------------------------------------------------------------------------

bool ComparisonSession::acceptLocal(const QString& table, const QString& pk) {
    // Accepting local means discard any staged remote change for this row.
    staging_.unstage(table, pk);
    return true;
}

bool ComparisonSession::acceptRemote(const QString& table, const QString& pk) {
    return stageRow(table, pk);
}

// ---------------------------------------------------------------------------
// stageCell
// ---------------------------------------------------------------------------

bool ComparisonSession::stageCell(const QString& table, const QString& pk, const QString& column,
                                  const QVariant& value) {
    // C-4 fix: start from the already-staged version of the row so that multiple stageCell()
    // calls accumulate correctly. Without this, each call re-reads from local/remote and
    // overwrites any cells that were staged by previous stageCell() calls on the same row.
    QVariantMap row = staging_.getRow(table, pk);
    if (row.isEmpty()) {
        // Not yet staged — seed from local row, fall back to remote.
        row = findLocalRow(table, pk);
        if (row.isEmpty())
            row = findRemoteRow(table, pk);
        if (row.isEmpty())
            return false;
    }

    row.insert(column, value);
    staging_.stage(table, pk, row);
    return true;
}

// ---------------------------------------------------------------------------
// fetchRemoteRows
// ---------------------------------------------------------------------------

QList<RowDiff> ComparisonSession::fetchRemoteRows(const QString& table,
                                                  const QString& keysetPageToken, int pageSize,
                                                  const QString& /*snapshotId*/) const {
    if (!remoteData_.contains(table))
        return {};

    const QList<QVariantMap>& rows = remoteData_[table].rows;
    int startIdx = 0;

    // keysetPageToken encodes the last PK seen as a simple offset hint.
    // If not empty, treat it as the index into the list.
    if (!keysetPageToken.isEmpty()) {
        bool ok = false;
        int hint = keysetPageToken.toInt(&ok);
        if (ok && hint >= 0 && hint < rows.size())
            startIdx = hint;
    }

    int endIdx = (pageSize <= 0) ? rows.size() : std::min(startIdx + pageSize, (int)rows.size());

    QList<RowDiff> result;
    result.reserve(endIdx - startIdx);

    QString pkCol = getPkColumn(table);
    for (int i = startIdx; i < endIdx; ++i) {
        RowDiff rd;
        rd.kind = RowDiffKind::Added;  // remote-side view: all rows are "added"
        rd.primaryKey = pkCol.isEmpty() ? QString::number(i) : rows[i].value(pkCol).toString();
        for (auto it = rows[i].constBegin(); it != rows[i].constEnd(); ++it) {
            CellDiff cd;
            cd.column = it.key();
            cd.remoteValue = it.value();
            cd.changed = true;
            rd.cells.append(cd);
        }
        result.append(rd);
    }

    return result;
}

// ---------------------------------------------------------------------------
// save / discard
// ---------------------------------------------------------------------------

bool ComparisonSession::save(QString* err) {
    if (staging_.isEmpty()) {
        if (readTxnActive_) {
            rconn_.rollback();
            readTxnActive_ = false;
        }
        gate_.releaseAll();
        if (context_ && context_->rescanFn)
            context_->rescanFn();
        return true;
    }

    if (!checkStale(err)) {
        staging_.discard();
        if (readTxnActive_) {
            rconn_.rollback();
            readTxnActive_ = false;
        }
        gate_.releaseAll();
        if (context_ && context_->rescanFn)
            context_->rescanFn();
        return false;
    }

    // Gather pk columns for all staged tables (use first table's PK as simplification;
    // a production implementation would group by table).
    // Collect distinct tables in staging and save per-table with correct pkCols.
    // Because StagingBuffer::save takes a single pkCols list, we call it once
    // with the union of pk columns across all tables (safe: UpsertExecutor
    // matches pkCols against actual column names).
    QStringList allPkCols;
    const QStringList tables = [&] {
        QStringList ts;
        for (const auto& kv : remoteData_.keys())
            ts << kv;
        return ts;
    }();

    for (const QString& table : tables) {
        QString pkCol = getPkColumn(table);
        if (!pkCol.isEmpty() && !allPkCols.contains(pkCol))
            allPkCols.append(pkCol);
    }

    if (!context_ || (!context_->workerCaptureWriteFn && !context_->workerWriteFn)) {
        if (err)
            *err = QStringLiteral("%1: comparison session has no worker write queue")
                       .arg(QLatin1String(err::E_SYNC_INIT));
        return false;
    }

    // C-05 fix: use workerCaptureWriteFn when available so mutations flow through
    // CapturedWriteTemplate — they are session-captured, written to __sync_changelog, and
    // broadcast to peers. Fall back to workerWriteFn for backwards-compat with contexts that
    // don't have a capture function wired (e.g. test stubs).
    QString saveErr;
    bool ok = false;
    if (context_->workerCaptureWriteFn) {
        // Build per-table PK column map for correct RowMutation construction.
        QHash<QString, QStringList> pkColsPerTable;
        for (const QString& table : tables) {
            const QString pkCol = getPkColumn(table);
            if (!pkCol.isEmpty())
                pkColsPerTable[table] = QStringList{pkCol};
        }
        const QList<RowMutation> mutations = staging_.toMutations(pkColsPerTable, allPkCols);
        const QStringList syncTables = context_->canonicalSyncTables;
        ok = context_->workerCaptureWriteFn(mutations, syncTables, &saveErr);
    } else {
        StagingBuffer staged = staging_;
        ok = context_->workerWriteFn(
            [staged, allPkCols](QSqlDatabase& wconn, QString* taskErr) mutable {
                UpsertExecutor upsert;
                return staged.save(wconn, upsert, allPkCols, taskErr);
            },
            &saveErr);
    }
    if (!ok) {
        if (err)
            *err = saveErr;
        return false;
    }

    staging_.discard();
    // H-7 fix: commit (ROLLBACK is fine too — no writes on rconn_) the pinned read transaction.
    if (readTxnActive_) {
        rconn_.rollback();
        readTxnActive_ = false;
    }
    gate_.releaseAll();
    if (context_ && context_->rescanFn)
        context_->rescanFn();
    return true;
}

void ComparisonSession::discard() {
    staging_.discard();
    // H-7 fix: release pinned read transaction on discard as well.
    if (readTxnActive_) {
        rconn_.rollback();
        readTxnActive_ = false;
    }
    gate_.releaseAll();
    // H-09 fix: trigger rescan so pending 'seen' artifacts deferred during session are processed.
    if (context_ && context_->rescanFn)
        context_->rescanFn();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool ComparisonSession::checkStale(QString* err) {
    const qint64 current = readDataVersion(err);
    if (current <= 0)
        return false;
    if (pinnedDataVersion_ != 0 && current != pinnedDataVersion_) {
        if (err)
            *err = QStringLiteral(
                       "%1: staged comparison is stale (pinned data_version=%2, "
                       "current data_version=%3)")
                       .arg(QLatin1String(err::E_SYNC_STAGE_STALE))
                       .arg(pinnedDataVersion_)
                       .arg(current);
        return false;
    }
    return true;
}

qint64 ComparisonSession::readDataVersion(QString* err) const {
    QSqlQuery q(rconn_);
    if (!q.exec(QStringLiteral("PRAGMA data_version")) || !q.next()) {
        if (err)
            *err = q.lastError().text();
        return -1;
    }
    return q.value(0).toLongLong();
}

QVariantMap ComparisonSession::findRemoteRow(const QString& table, const QString& pk) const {
    if (!remoteData_.contains(table))
        return {};

    QString pkCol = getPkColumn(table);
    const QList<QVariantMap>& rows = remoteData_[table].rows;
    for (const QVariantMap& row : rows) {
        if (!pkCol.isEmpty() && row.value(pkCol).toString() == pk)
            return row;
    }
    return {};
}

QVariantMap ComparisonSession::findLocalRow(const QString& table, const QString& pk) const {
    QString pkCol = getPkColumn(table);
    if (pkCol.isEmpty())
        return {};

    QSqlQuery q(rconn_);
    // H-4 fix: use quoteIdent to handle table/column names with embedded double-quotes.
    q.prepare(
        QStringLiteral("SELECT * FROM %1 WHERE %2 = :pk")
            .arg(detail::SqlBuilder::quoteIdent(table), detail::SqlBuilder::quoteIdent(pkCol)));
    q.bindValue(":pk", pk);
    if (!q.exec() || !q.next())
        return {};

    QSqlRecord rec = q.record();
    QVariantMap row;
    for (int i = 0; i < rec.count(); ++i)
        row.insert(rec.fieldName(i), q.value(i));
    return row;
}

QString ComparisonSession::getPkColumn(const QString& table) const {
    if (pkColCache_.contains(table))
        return pkColCache_.value(table);

    QSqlQuery q(rconn_);
    // H-4 fix: use quoteIdent.
    q.prepare(QStringLiteral("PRAGMA table_info(") + detail::SqlBuilder::quoteIdent(table) +
              QLatin1Char(')'));
    if (!q.exec())
        return {};

    QString pkCol;
    int bestPk = INT_MAX;
    while (q.next()) {
        int pk = q.value("pk").toInt();
        if (pk > 0 && pk < bestPk) {
            bestPk = pk;
            pkCol = q.value("name").toString();
        }
    }

    pkColCache_.insert(table, pkCol);
    return pkCol;
}

// ---------------------------------------------------------------------------
// I-18: createComparisonSession factory
//
// Owns all helper objects (TableStateStore, DiffEngine, InboundTableGate,
// UpsertExecutor) as well as the QSqlDatabase connection used for read-only
// diff operations.  The owned session is wrapped in a thin RAII holder so
// that the connection is cleaned up when the IComparisonSession is destroyed.
// ---------------------------------------------------------------------------

namespace {

// OwningComparisonSession wraps a ComparisonSession and keeps its dependency
// objects alive for the full lifetime of the session.
struct OwnedDeps {
    QString connName;
    QSqlDatabase rconn;
    TableStateStore ts;
    DiffEngine diff;
    UpsertExecutor upsert;
    std::shared_ptr<SyncContext> ctx;
};

class OwningComparisonSession : public IComparisonSession {
   public:
    explicit OwningComparisonSession(std::unique_ptr<OwnedDeps> deps,
                                     std::unique_ptr<ComparisonSession> inner)
        : deps_(std::move(deps)), inner_(std::move(inner)) {
    }

    ~OwningComparisonSession() override {
        inner_.reset();
        deps_->rconn.close();
        // M-6 fix: invalidate the QSqlDatabase handle before removeDatabase.
        // Qt requires all copies of a QSqlDatabase to be destroyed or set to default
        // before removeDatabase() is called to avoid "connection still in use" warnings.
        deps_->rconn = QSqlDatabase();
        QSqlDatabase::removeDatabase(deps_->connName);
    }

    QList<TableDiff> tableDiffs() const override {
        return inner_->tableDiffs();
    }
    QList<RowDiff> rowDiffs(const QString& t, int off, int lim) const override {
        return inner_->rowDiffs(t, off, lim);
    }
    bool stageRow(const QString& t, const QString& pk) override {
        return inner_->stageRow(t, pk);
    }
    bool stageTable(const QString& t) override {
        return inner_->stageTable(t);
    }
    bool unstage(const QString& t, const QString& pk) override {
        return inner_->unstage(t, pk);
    }
    bool acceptLocal(const QString& t, const QString& pk) override {
        return inner_->acceptLocal(t, pk);
    }
    bool acceptRemote(const QString& t, const QString& pk) override {
        return inner_->acceptRemote(t, pk);
    }
    bool stageCell(const QString& t, const QString& pk, const QString& col,
                   const QVariant& v) override {
        return inner_->stageCell(t, pk, col, v);
    }
    QList<RowDiff> fetchRemoteRows(const QString& t, const QString& tok, int ps,
                                   const QString& snap) const override {
        return inner_->fetchRemoteRows(t, tok, ps, snap);
    }
    bool initialize(const QList<RemoteTableSnapshot>& remoteSnapshots,
                    QString* err = nullptr) override {
        return inner_->initialize(remoteSnapshots, err);
    }
    bool save(QString* err) override {
        return inner_->save(err);
    }
    void discard() override {
        inner_->discard();
    }

   private:
    std::unique_ptr<OwnedDeps> deps_;
    std::unique_ptr<ComparisonSession> inner_;
};

}  // anonymous namespace

std::unique_ptr<IComparisonSession> createComparisonSession(const SyncConfig& config,
                                                            QString* err) {
    auto deps = std::make_unique<OwnedDeps>();
    deps->ctx = SyncContextRegistry::instance().getExisting(config.sqlitePath());
    if (!deps->ctx || !deps->ctx->inboundTableGate) {
        if (err)
            *err = QStringLiteral("SyncContext not initialized for comparison session");
        return nullptr;
    }

    // Open a read-only connection for diff operations.
    deps->connName =
        QStringLiteral("dbridge_cs_ro_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    deps->rconn = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), deps->connName);
    deps->rconn.setDatabaseName(config.sqlitePath());
    deps->rconn.setConnectOptions(
        QStringLiteral("QSQLITE_OPEN_READONLY=1;QSQLITE_BUSY_TIMEOUT=5000"));
    if (!deps->rconn.open()) {
        if (err)
            *err = deps->rconn.lastError().text();
        QSqlDatabase::removeDatabase(deps->connName);
        return nullptr;
    }

    // H-13 fix: use the worker's published stream epoch (not a 0 placeholder) so
    // DiffEngine::tableDiffs() reads __sync_table_state at the correct epoch.
    const qint64 epoch = deps->ctx->streamEpoch;

    auto session = std::make_unique<ComparisonSession>(
        deps->rconn,  // rconn: read-only diff queries
        deps->rconn,  // unused write reference; writes are marshalled to SyncWorker
        deps->ts, deps->diff, *deps->ctx->inboundTableGate, deps->upsert, epoch, deps->ctx);

    return std::make_unique<OwningComparisonSession>(std::move(deps), std::move(session));
}

}  // namespace dbridge::sync
