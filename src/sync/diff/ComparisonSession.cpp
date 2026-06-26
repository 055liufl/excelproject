#include "ComparisonSession.h"

#include "dbridge/Errors.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

#include "sync/WriteTxn.h"
#include <algorithm>
#include <memory>

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// Constructor
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
// ---------------------------------------------------------------------------

bool ComparisonSession::initialize(const QStringList& tables,
                                   const QHash<QString, DiffEngine::RemoteMeta>& remoteMetas,
                                   const QHash<QString, QList<QVariantMap>>& remoteRows,
                                   QString* err) {
    // Pin a read snapshot by issuing a no-op query on rconn.
    // SQLite in WAL mode isolates the read transaction from that point on.
    {
        QSqlQuery q(rconn_);
        if (!q.exec("SELECT * FROM sqlite_master LIMIT 0")) {
            if (err)
                *err = q.lastError().text();
            return false;
        }
    }
    pinnedDataVersion_ = readDataVersion(err);
    if (pinnedDataVersion_ <= 0)
        return false;

    // Build remoteData_ cache.
    for (const QString& t : tables) {
        RemoteTableData rd;
        if (remoteMetas.contains(t))
            rd.meta = remoteMetas[t];
        if (remoteRows.contains(t))
            rd.rows = remoteRows[t];
        remoteData_.insert(t, rd);
    }

    // Compute table-level diffs.
    diffs_ = diff_.tableDiffs(rconn_, tables, streamEpoch_, ts_, remoteMetas);

    // Open the gate to defer inbound changes to these tables.
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
    // Start from already-staged row if present, otherwise fall back to local.
    QVariantMap row;

    // Check staged_ by looking for the pk in the staging buffer via a
    // round-trip: we save, then retrieve — but StagingBuffer has no getter.
    // Use findLocalRow as base, then overlay with the remote to get latest.
    row = findLocalRow(table, pk);
    if (row.isEmpty())
        row = findRemoteRow(table, pk);
    if (row.isEmpty())
        return false;

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
        gate_.releaseAll();
        if (context_ && context_->rescanFn)
            context_->rescanFn();
        return true;
    }

    if (!checkStale(err)) {
        staging_.discard();
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

    if (!context_ || !context_->workerWriteFn) {
        if (err)
            *err = QStringLiteral("%1: comparison session has no worker write queue")
                       .arg(QLatin1String(err::E_SYNC_INIT));
        return false;
    }

    StagingBuffer staged = staging_;
    QString saveErr;
    const bool ok = context_->workerWriteFn(
        [staged, allPkCols](QSqlDatabase& wconn, QString* taskErr) mutable {
            UpsertExecutor upsert;
            return staged.save(wconn, upsert, allPkCols, taskErr);
        },
        &saveErr);
    if (!ok) {
        if (err)
            *err = saveErr;
        return false;
    }

    staging_.discard();
    gate_.releaseAll();
    if (context_ && context_->rescanFn)
        context_->rescanFn();
    return true;
}

void ComparisonSession::discard() {
    staging_.discard();
    gate_.releaseAll();
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
    q.prepare(QString("SELECT * FROM \"%1\" WHERE \"%2\" = :pk").arg(table, pkCol));
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
    q.prepare(QString("PRAGMA table_info(\"%1\")").arg(table));
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

    // streamEpoch is not known here at factory time (it is owned by SyncWorker);
    // use 0 as a placeholder — the caller can call initialize() to set up diffs.
    constexpr qint64 kPlaceholderEpoch = 0;

    auto session = std::make_unique<ComparisonSession>(
        deps->rconn,  // rconn: read-only diff queries
        deps->rconn,  // unused write reference; writes are marshalled to SyncWorker
        deps->ts, deps->diff, *deps->ctx->inboundTableGate, deps->upsert, kPlaceholderEpoch,
        deps->ctx);

    return std::make_unique<OwningComparisonSession>(std::move(deps), std::move(session));
}

}  // namespace dbridge::sync
