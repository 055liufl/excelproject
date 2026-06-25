#include "ComparisonSession.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

#include <algorithm>

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ComparisonSession::ComparisonSession(QSqlDatabase& rconn, QSqlDatabase& wconn, TableStateStore& ts,
                                     DiffEngine& diff, InboundTableGate& gate,
                                     UpsertExecutor& upsert, qint64 streamEpoch)
    : rconn_(rconn),
      wconn_(wconn),
      ts_(ts),
      diff_(diff),
      gate_(gate),
      upsert_(upsert),
      streamEpoch_(streamEpoch) {
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
    pinnedDataVersion_ = 1;  // non-zero means initialized

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
        return true;
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

    QString saveErr;
    if (!staging_.save(wconn_, upsert_, allPkCols, &saveErr)) {
        if (err)
            *err = saveErr;
        return false;
    }

    gate_.releaseAll();
    return true;
}

void ComparisonSession::discard() {
    staging_.discard();
    gate_.releaseAll();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

bool ComparisonSession::checkStale(QString* /*err*/) {
    // Simplified: assume not stale. Full implementation would compare
    // the pinned data_version against current sqlite3_total_changes.
    return true;
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

}  // namespace dbridge::sync
