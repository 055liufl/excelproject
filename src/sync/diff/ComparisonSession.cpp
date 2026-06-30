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

// tableDiffs —— 返回 initialize 时已算好并缓存的表级差异，不重算（O(1) 取值）。
QList<TableDiff> ComparisonSession::tableDiffs() const {
    return diffs_;
}

// rowDiffs —— 取某表 [offset, offset+limit) 区间的行级差异（分页）。
//   委托 DiffEngine 拿「本地（rconn_ 上 SELECT）vs 对端缓存行」逐行比对的结果。
//   表不在对端缓存里（未参与比对）→ 返回空。limit<=0 通常表示「到底」（见 DiffEngine）。
QList<RowDiff> ComparisonSession::rowDiffs(const QString& table, int offset, int limit) const {
    if (!remoteData_.contains(table))
        return {};
    return diff_.rowDiffs(rconn_, table, remoteData_[table].rows, offset, limit);
}

// ---------------------------------------------------------------------------
// stageRow / stageTable / unstage
// ---------------------------------------------------------------------------

// stageRow —— 把「对端的某一行」整行暂存为决策（即：这一行采用对端版本）。
//   从对端缓存找出该行内容存入 staging_；对端没有这行（pk 无对应）→ 返回 false。
//   暂存只进内存，真正写库要等 save()。
bool ComparisonSession::stageRow(const QString& table, const QString& pk) {
    QVariantMap row = findRemoteRow(table, pk);
    if (row.isEmpty())
        return false;
    staging_.stage(table, pk, row);
    return true;
}

// stageTable —— 「整表采用对端」：把该表所有「与本地不同」的行一次性暂存。
//   做法：先用 DiffEngine 算出全表行级差异（limit=-1 表示不分页、全取），跳过 Same 行
//   （相同无需写），其余逐行取对端内容入暂存。表未参与比对 → 返回 false。
//   为什么只 stage Different 行：减少无谓写入，也让 save 时的捕获/广播只覆盖真正变化的行。
bool ComparisonSession::stageTable(const QString& table) {
    if (!remoteData_.contains(table))
        return false;

    // Find the table diff to limit work to Different rows.
    // 取主键列名（下方虽未直接用 pkCol，但 getPkColumn 同时会暖好 pkColCache_，供后续复用）。
    QString pkCol = getPkColumn(table);

    // For each remote row that differs from local, stage it.
    // 算全表行级差异，对每条「非 Same」的差异行取对端内容入暂存。
    const QList<QVariantMap>& remoteRows = remoteData_[table].rows;
    QList<RowDiff> diffs = diff_.rowDiffs(rconn_, table, remoteRows, 0, -1);

    for (const RowDiff& rd : diffs) {
        if (rd.kind == RowDiffKind::Same)
            continue;  // 与本地相同 → 无需采用对端，跳过
        QVariantMap row = findRemoteRow(table, rd.primaryKey);
        if (!row.isEmpty())
            staging_.stage(table, rd.primaryKey, row);
    }
    return true;
}

// unstage —— 撤销某行的暂存决策（恢复到「沿用本地、不采用对端」）。恒返回 true。
bool ComparisonSession::unstage(const QString& table, const QString& pk) {
    staging_.unstage(table, pk);
    return true;
}

// ---------------------------------------------------------------------------
// acceptLocal / acceptRemote
// ---------------------------------------------------------------------------

// acceptLocal —— 「这行采用本地」：等价于撤销该行的暂存（本地是默认值，不写即沿用本地）。
bool ComparisonSession::acceptLocal(const QString& table, const QString& pk) {
    // Accepting local means discard any staged remote change for this row.
    // 采用本地 = 丢弃该行已暂存的「采用对端」决策（沿用本地无需任何写入）。
    staging_.unstage(table, pk);
    return true;
}

// acceptRemote —— 「这行采用对端」：语义上就是 stageRow（把对端整行暂存为决策）。
bool ComparisonSession::acceptRemote(const QString& table, const QString& pk) {
    return stageRow(table, pk);
}

// ---------------------------------------------------------------------------
// stageCell
// ---------------------------------------------------------------------------

// stageCell —— 单元格级合并：把某行某列改成自定义值（既非纯本地也非纯对端，而是手动取值）。
//   用于「这行大部分用本地，但某几个字段要改成对端/某个值」的精细合并场景。
bool ComparisonSession::stageCell(const QString& table, const QString& pk, const QString& column,
                                  const QVariant& value) {
    // C-4 fix: start from the already-staged version of the row so that multiple stageCell()
    // calls accumulate correctly. Without this, each call re-reads from local/remote and
    // overwrites any cells that were staged by previous stageCell() calls on the same row.
    // C-4 修复：从「已暂存的该行版本」起手，使对同一行的多次 stageCell() 能逐格累积。
    //   否则每次调用都从 local/remote 重新读整行，会把前几次 stageCell 改过的格子覆盖掉。
    QVariantMap row = staging_.getRow(table, pk);
    if (row.isEmpty()) {
        // Not yet staged — seed from local row, fall back to remote.
        // 该行尚未暂存过 → 以本地行为基底；本地无此行则退而用对端行；都没有则失败。
        row = findLocalRow(table, pk);
        if (row.isEmpty())
            row = findRemoteRow(table, pk);
        if (row.isEmpty())
            return false;
    }

    row.insert(column, value);       // 覆盖目标列为自定义值（其余列保持基底）
    staging_.stage(table, pk, row);  // 回写整行进暂存，供下次 stageCell 继续累积
    return true;
}

// ---------------------------------------------------------------------------
// fetchRemoteRows
// ---------------------------------------------------------------------------

// fetchRemoteRows —— 分页拉取「对端行」本身（不与本地比对），把每行包成「全列皆 Added」的
//   RowDiff 返回。用途：UI 需要逐页浏览对端快照内容（如惰性表的远端数据）时取数。
//   分页机制：keysetPageToken 这里被当作「起始下标提示」（简化实现，非真正的 keyset 游标）；
//     pageSize<=0 表示取到末尾，否则取 [startIdx, startIdx+pageSize)。
//   主键：有主键列则取该列值作 primaryKey；无则退化用下标字符串占位。
//   snapshotId 形参在本实现未用（对端数据已固化在 remoteData_ 内，无需按快照 id 重取）。
QList<RowDiff> ComparisonSession::fetchRemoteRows(const QString& table,
                                                  const QString& keysetPageToken, int pageSize,
                                                  const QString& /*snapshotId*/) const {
    if (!remoteData_.contains(table))
        return {};

    const QList<QVariantMap>& rows = remoteData_[table].rows;
    int startIdx = 0;

    // keysetPageToken encodes the last PK seen as a simple offset hint.
    // If not empty, treat it as the index into the list.
    // 这里把 token 简化为「列表下标提示」：非空且可解析为合法下标时，作为本页起点。
    if (!keysetPageToken.isEmpty()) {
        bool ok = false;
        int hint = keysetPageToken.toInt(&ok);
        if (ok && hint >= 0 && hint < rows.size())
            startIdx = hint;
    }

    // 本页终点：pageSize<=0 取到末尾；否则起点 + 页大小，并用 min 夹住不越界。
    int endIdx = (pageSize <= 0) ? rows.size() : std::min(startIdx + pageSize, (int)rows.size());

    QList<RowDiff> result;
    result.reserve(endIdx - startIdx);  // 预留本页容量

    QString pkCol = getPkColumn(table);
    for (int i = startIdx; i < endIdx; ++i) {
        RowDiff rd;
        rd.kind = RowDiffKind::Added;  // remote-side view: all rows are "added"
                                       // 纯对端视角：本接口不做本地比对，每行一律标 Added。
        // 主键取值：有主键列用其值，否则用下标占位（保证 primaryKey 非空可定位）。
        rd.primaryKey = pkCol.isEmpty() ? QString::number(i) : rows[i].value(pkCol).toString();
        // 把该行每一列都包成 CellDiff（remoteValue 为该列值、changed=true），供 UI 直接展示。
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

// save —— 会话收尾的「提交」路径：stale 检测通过后，把暂存决策经 worker 落库，再释放
//   读事务与门控并触发 rescan。是全文件最综合的一段，分多个早退分支：
//     · 无任何暂存（用户没采用任何对端行）→ 直接清理收尾、视为成功（无写）。
//     · stale（本地库被外部改过）→ 拒绝写回（E_SYNC_STAGE_STALE），清理并返回 false。
//     · 无 worker 写队列 → 无法落库，报 E_SYNC_INIT。
//     · 正常：把暂存转 RowMutation，优先走「捕获式写」（进 changelog 并广播），回退走普通写。
//   不变量：无论成功失败，读事务与门控都要被释放（避免泄漏/长期挂起 inbox），故每个分支末尾
//     都重复了「rollback 读事务 + releaseAll 门控 + rescanFn」这套收尾动作。
bool ComparisonSession::save(QString* err) {
    if (staging_.isEmpty()) {
        // 没有任何「采用对端」的决策 → 无需写库，直接释放资源并成功返回。
        if (readTxnActive_) {
            rconn_.rollback();  // 释放钉住的读快照（rconn_ 上无写，rollback 等同收尾）
            readTxnActive_ = false;
        }
        gate_.releaseAll();  // 解除对被比对表的 inbox 门控
        if (context_ && context_->rescanFn)
            context_->rescanFn();  // 触发重扫，让门控期间被推迟的入站工件得以补上
        return true;
    }

    if (!checkStale(err)) {
        // 本地库在会话期间被外部改动（data_version 变了）→ 暂存基于的画面已过期，拒绝写回，
        //   否则可能基于陈旧数据覆盖掉新写入。丢弃暂存并完整收尾。
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
    // 汇集所有参与表的主键列名到 allPkCols（去重）。StagingBuffer::save 接收单一 pkCols 列表，
    //   故取「跨表主键列名的并集」一次性传入——安全的原因见上方英文注释：UpsertExecutor 会按
    //   实际列名匹配 pkCols，多余的主键列名对某张不含它的表不会误伤。
    QStringList allPkCols;
    const QStringList tables = [&] {  // 立即调用的 lambda：把 remoteData_ 的键收集成表名列表
        QStringList ts;
        for (const auto& kv : remoteData_.keys())
            ts << kv;
        return ts;
    }();

    for (const QString& table : tables) {
        QString pkCol = getPkColumn(table);
        if (!pkCol.isEmpty() && !allPkCols.contains(pkCol))
            allPkCols.append(pkCol);  // 去重累积各表主键列名
    }

    // 没有任何 worker 写函数 → 本会话无法把决策落库（构造时未接好写队列，如错误初始化）。
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
        // 走「捕获式写」路径：为每张表建「表→主键列」映射，使暂存转成的 RowMutation 主键正确。
        QHash<QString, QStringList> pkColsPerTable;
        for (const QString& table : tables) {
            const QString pkCol = getPkColumn(table);
            if (!pkCol.isEmpty())
                pkColsPerTable[table] = QStringList{pkCol};
        }
        // 把暂存决策转成行变更集合，交给 worker 的「捕获式写」——它会经 CapturedWriteTemplate
        //   落库、写入 __sync_changelog 并广播给对端（合并结果会自然传播出去）。
        const QList<RowMutation> mutations = staging_.toMutations(pkColsPerTable, allPkCols);
        const QStringList syncTables = context_->canonicalSyncTables;  // 规范同步表集（白名单）
        ok = context_->workerCaptureWriteFn(mutations, syncTables, &saveErr);
    } else {
        // 回退路径（无捕获函数，如测试桩）：把暂存按值拷进 lambda，提交一个普通 worker 写任务，
        //   在 worker 线程上用 UpsertExecutor 直接落库。注意此路径不进 changelog、不广播。
        StagingBuffer staged = staging_;  // 值拷贝，安全跨线程传入 lambda
        ok = context_->workerWriteFn(
            [staged, allPkCols](QSqlDatabase& wconn, QString* taskErr) mutable {
                UpsertExecutor upsert;
                return staged.save(wconn, upsert, allPkCols, taskErr);
            },
            &saveErr);
    }
    if (!ok) {
        if (err)
            *err = saveErr;  // 落库失败：透传 worker 报的错
        return false;
    }

    staging_.discard();  // 写成功 → 清空暂存（决策已落地）
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

// discard —— 放弃整场比对：丢弃所有暂存决策、释放读事务与门控、触发 rescan。
//   与 save 的「不写库版收尾」对称——用户取消比对时调用，库保持原状不受任何影响。
void ComparisonSession::discard() {
    staging_.discard();  // 丢弃全部暂存决策（不落库）
    // H-7 fix: release pinned read transaction on discard as well.
    // H-7 修复：discard 同样要释放钉住的读事务（rconn_ 上无写，rollback 即收尾）。
    if (readTxnActive_) {
        rconn_.rollback();
        readTxnActive_ = false;
    }
    gate_.releaseAll();  // 解除 inbox 门控
    // H-09 fix: trigger rescan so pending 'seen' artifacts deferred during session are processed.
    // H-09 修复：触发重扫，使会话期间被推迟的 'seen' 入站工件得以补上处理。
    if (context_ && context_->rescanFn)
        context_->rescanFn();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

// checkStale —— stale 检测：当前 data_version 与 initialize 钉住的是否一致。
//   读当前值；读失败(<=0)即判失败；钉住值非 0 且与当前不等 → 库被外部改过，判 stale 并写
//   E_SYNC_STAGE_STALE（含两个版本号便于诊断）；一致则通过返回 true。
bool ComparisonSession::checkStale(QString* err) {
    const qint64 current = readDataVersion(err);
    if (current <= 0)
        return false;  // 读 data_version 失败：保守判失败
    if (pinnedDataVersion_ != 0 && current != pinnedDataVersion_) {
        if (err)
            *err = QStringLiteral(
                       "%1: staged comparison is stale (pinned data_version=%2, "
                       "current data_version=%3)")
                       .arg(QLatin1String(err::E_SYNC_STAGE_STALE))
                       .arg(pinnedDataVersion_)
                       .arg(current);
        return false;  // 版本漂移 → 暂存过期
    }
    return true;
}

// readDataVersion —— 读 PRAGMA data_version（SQLite 维护的「本连接之外的写入计数」）。
//   库每被其它连接改一次该值就变，是 stale 检测的依据。失败返回 -1 并写 err。
qint64 ComparisonSession::readDataVersion(QString* err) const {
    QSqlQuery q(rconn_);
    if (!q.exec(QStringLiteral("PRAGMA data_version")) || !q.next()) {
        if (err)
            *err = q.lastError().text();
        return -1;
    }
    return q.value(0).toLongLong();
}

// findRemoteRow —— 在对端缓存行集合里按主键线性查找一行（找不到返回空 map）。
//   对端数据在内存里、量不大，故直接遍历比对主键列的字符串值即可。
QVariantMap ComparisonSession::findRemoteRow(const QString& table, const QString& pk) const {
    if (!remoteData_.contains(table))
        return {};

    QString pkCol = getPkColumn(table);
    const QList<QVariantMap>& rows = remoteData_[table].rows;
    for (const QVariantMap& row : rows) {
        if (!pkCol.isEmpty() && row.value(pkCol).toString() == pk)
            return row;  // 主键匹配 → 返回该行
    }
    return {};
}

// findLocalRow —— 在本地库（rconn_，已钉读快照）按主键 SELECT 一行，组装成列名→值的 map。
//   无主键列或查不到行 → 返回空 map。
QVariantMap ComparisonSession::findLocalRow(const QString& table, const QString& pk) const {
    QString pkCol = getPkColumn(table);
    if (pkCol.isEmpty())
        return {};

    QSqlQuery q(rconn_);
    // H-4 fix: use quoteIdent to handle table/column names with embedded double-quotes.
    // H-4 修复：用 quoteIdent 安全转义表名/列名（处理名字里含双引号等情形，防注入/语法错）。
    q.prepare(
        QStringLiteral("SELECT * FROM %1 WHERE %2 = :pk")
            .arg(detail::SqlBuilder::quoteIdent(table), detail::SqlBuilder::quoteIdent(pkCol)));
    q.bindValue(":pk", pk);  // 主键值用绑定参数，不拼进 SQL（防注入）
    if (!q.exec() || !q.next())
        return {};

    // 把结果记录的每一列搬进 QVariantMap（列名 → 值）。
    QSqlRecord rec = q.record();
    QVariantMap row;
    for (int i = 0; i < rec.count(); ++i)
        row.insert(rec.fieldName(i), q.value(i));
    return row;
}

// getPkColumn —— 取某表的主键列名，结果进 pkColCache_ 缓存（mutable，const 方法内可填）。
//   实现：PRAGMA table_info 返回各列的 pk 序号（0=非主键，>=1 为主键内的位次）。本函数取
//     pk>0 中位次最小者作为主键列——这意味着对复合主键只取「第一列」（同步以单列主键为前提）。
//   缓存命中直接返回；查询失败返回空串（不缓存失败，下次可重试）。
QString ComparisonSession::getPkColumn(const QString& table) const {
    if (pkColCache_.contains(table))
        return pkColCache_.value(table);  // 缓存命中

    QSqlQuery q(rconn_);
    // H-4 fix: use quoteIdent.
    // H-4 修复：表名经 quoteIdent 转义后拼入 PRAGMA。
    q.prepare(QStringLiteral("PRAGMA table_info(") + detail::SqlBuilder::quoteIdent(table) +
              QLatin1Char(')'));
    if (!q.exec())
        return {};

    QString pkCol;
    int bestPk = INT_MAX;  // 记录目前见到的最小 pk 位次
    while (q.next()) {
        int pk = q.value("pk").toInt();  // 该列在主键中的位次（0 表示非主键）
        if (pk > 0 && pk < bestPk) {     // 取位次最小（=第一列）的主键列
            bestPk = pk;
            pkCol = q.value("name").toString();
        }
    }

    pkColCache_.insert(table, pkCol);  // 缓存结果（含空串，但上面失败路径已提前返回不缓存）
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
// I-18：createComparisonSession 工厂。
//   ComparisonSession 的依赖（rconn 连接、TableStateStore、DiffEngine、UpsertExecutor、
//   SyncContext）都靠引用持有、不拥有生命周期；工厂把它们打包成一个 OwnedDeps 一并拥有，
//   再用一个薄 RAII 壳（OwningComparisonSession）转发所有接口调用，并在析构时按正确顺序
//   关闭/移除那条只读连接——从而对外只暴露一个自洽、自管理生命周期的 IComparisonSession。
// ---------------------------------------------------------------------------

namespace {  // 匿名命名空间：以下类型仅本 TU 可见，不污染外部符号。

// OwnedDeps —— 一束「会话存活期内必须保活」的依赖对象（工厂拥有、随会话一起销毁）。
//   注意成员声明顺序也是构造顺序、其逆序是析构顺序；rconn 须在 ts/diff/upsert 之后销毁的
//   约束由 OwningComparisonSession 析构里的显式收尾保证（见下）。
struct OwnedDeps {
    QString connName;                  // 只读连接的唯一名（用于最终 removeDatabase）
    QSqlDatabase rconn;                // 专用只读连接（diff 查询 + 钉读快照）
    TableStateStore ts;                // 表状态存储
    DiffEngine diff;                   // 差异引擎
    UpsertExecutor upsert;             // UPSERT 执行器（回退路径用）
    std::shared_ptr<SyncContext> ctx;  // 同步上下文（提供门控/worker 写函数/规范表集）
};

// OwningComparisonSession —— 持有 OwnedDeps + 内层 ComparisonSession 的 RAII 包装；
//   所有 IComparisonSession 方法都「透传」给内层 inner_，自身只负责生命周期管理。
class OwningComparisonSession : public IComparisonSession {
   public:
    explicit OwningComparisonSession(std::unique_ptr<OwnedDeps> deps,
                                     std::unique_ptr<ComparisonSession> inner)
        : deps_(std::move(deps)), inner_(std::move(inner)) {
    }

    // 析构按序拆解：先毁内层会话（它可能仍持有 rconn 上的读事务）→ 关连接 → 置空句柄 → 移除。
    ~OwningComparisonSession() override {
        inner_.reset();        // 先销毁会话（触发其 discard/收尾，释放读事务）
        deps_->rconn.close();  // 关闭连接
        // M-6 fix: invalidate the QSqlDatabase handle before removeDatabase.
        // Qt requires all copies of a QSqlDatabase to be destroyed or set to default
        // before removeDatabase() is called to avoid "connection still in use" warnings.
        // M-6 修复：removeDatabase 前必须先把所有 QSqlDatabase 句柄副本销毁或置默认，
        //   否则 Qt 会报 "connection still in use"。故这里先把 rconn 重置为默认对象。
        deps_->rconn = QSqlDatabase();
        QSqlDatabase::removeDatabase(deps_->connName);  // 注销连接，释放底层资源
    }

    // ↓↓ 以下所有方法均为「透传」给内层 ComparisonSession，本壳不含业务逻辑。↓↓
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
    std::unique_ptr<OwnedDeps> deps_;  // 被拥有的依赖束（保活直至本壳析构）
    std::unique_ptr<ComparisonSession> inner_;  // 内层真正实现（先于 deps_ 析构）
};

}  // anonymous namespace

// createComparisonSession —— 工厂入口：装配一个自管理生命周期的比对会话。
//   步骤：① 取本库已初始化的 SyncContext（未初始化/无门控 → 失败，比对依赖同步上下文）；
//   ② 开一条只读 QSQLITE 连接（QSQLITE_OPEN_READONLY=1，并设 5s busy timeout 容忍并发写）；
//   ③ 用 worker 发布的真实 streamEpoch（H-13）；④ 构造 ComparisonSession（注意写连接形参
//   复用 rconn 占位——本实现不在它上面写，写经 worker 函数）；⑤ 用 OwningComparisonSession
//   把会话与依赖打包返回。任一前置失败都返回 nullptr 并写 err（连接已开则回滚注销）。
std::unique_ptr<IComparisonSession> createComparisonSession(const SyncConfig& config,
                                                            QString* err) {
    auto deps = std::make_unique<OwnedDeps>();
    // 取本物理库的共享上下文；比对会话强依赖它（门控、worker 写函数、规范表集都在其中）。
    deps->ctx = SyncContextRegistry::instance().getExisting(config.sqlitePath());
    if (!deps->ctx || !deps->ctx->inboundTableGate) {
        if (err)
            *err = QStringLiteral("SyncContext not initialized for comparison session");
        return nullptr;  // 同步未初始化 → 无法比对
    }

    // Open a read-only connection for diff operations.
    // 为 diff 操作开一条专用只读连接（唯一名，避免与 worker/其它连接冲突）。
    deps->connName =
        QStringLiteral("dbridge_cs_ro_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    deps->rconn = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), deps->connName);
    deps->rconn.setDatabaseName(config.sqlitePath());
    // 只读打开 + 5 秒忙等：只读保证本连接绝不改库；busy_timeout 让它在 worker
    // 写时短暂等待而非立刻失败。
    deps->rconn.setConnectOptions(
        QStringLiteral("QSQLITE_OPEN_READONLY=1;QSQLITE_BUSY_TIMEOUT=5000"));
    if (!deps->rconn.open()) {
        if (err)
            *err = deps->rconn.lastError().text();
        QSqlDatabase::removeDatabase(deps->connName);  // 开失败：注销刚加的连接，避免泄漏
        return nullptr;
    }

    // H-13 fix: use the worker's published stream epoch (not a 0 placeholder) so
    // DiffEngine::tableDiffs() reads __sync_table_state at the correct epoch.
    // H-13 修复：用 worker 发布的真实 streamEpoch（而非 0 占位），否则 DiffEngine 会按错误
    //   纪元读 __sync_table_state，导致表级差异算错。
    const qint64 epoch = deps->ctx->streamEpoch;

    auto session = std::make_unique<ComparisonSession>(
        deps->rconn,  // rconn: read-only diff queries（只读 diff 查询 + 钉读快照）
        deps->rconn,  // unused write reference; writes are marshalled to SyncWorker
                      // 写连接形参未用，复用 rconn 占位；真正的写经 SyncWorker 串行化。
        deps->ts, deps->diff, *deps->ctx->inboundTableGate, deps->upsert, epoch, deps->ctx);

    // 把会话与其依赖束一起移交给 RAII 壳；返回后调用方只见 IComparisonSession 接口。
    return std::make_unique<OwningComparisonSession>(std::move(deps), std::move(session));
}

}  // namespace dbridge::sync
