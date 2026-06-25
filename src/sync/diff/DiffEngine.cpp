#include "DiffEngine.h"

#include <QSet>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QVariant>

#include <algorithm>

namespace dbridge::sync {

QList<TableDiff> DiffEngine::tableDiffs(QSqlDatabase& rconn, const QStringList& tables,
                                        qint64 streamEpoch, TableStateStore& localTs,
                                        const QHash<QString, RemoteMeta>& remote) {
    QList<TableDiff> result;
    result.reserve(tables.size());

    for (const QString& table : tables) {
        TableDiff td;
        td.table = table;

        QString localFp, localChecksum;
        qint64 localRowCount = 0;
        QString err;
        bool localFound = localTs.readState(rconn, table, streamEpoch, &localFp, &localChecksum,
                                            &localRowCount, &err);

        bool remoteFound = remote.contains(table);

        if (!remoteFound && localFound) {
            td.status = TableDiffStatus::OnlyLocal;
        } else if (remoteFound && !localFound) {
            td.status = TableDiffStatus::OnlyRemote;
            td.addedRows = static_cast<int>(remote[table].rowCount);
        } else if (!remoteFound && !localFound) {
            td.status = TableDiffStatus::Identical;
        } else {
            const RemoteMeta& rm = remote[table];
            if (localChecksum == rm.checksum) {
                td.status = TableDiffStatus::Identical;
            } else {
                td.status = TableDiffStatus::Different;
                qint64 rowDiff = rm.rowCount - localRowCount;
                if (rowDiff > 0) {
                    td.addedRows = static_cast<int>(rowDiff);
                    td.modifiedRows = static_cast<int>(localRowCount / 4);
                } else if (rowDiff < 0) {
                    td.deletedRows = static_cast<int>(-rowDiff);
                    td.modifiedRows = static_cast<int>(rm.rowCount / 4);
                } else {
                    // Same count but different checksum — all changes are modifications.
                    td.modifiedRows = static_cast<int>(localRowCount);
                }
            }
        }

        result.append(td);
    }

    return result;
}

QList<RowDiff> DiffEngine::rowDiffs(QSqlDatabase& rconn, const QString& table,
                                    const QList<QVariantMap>& remoteRows, int offset, int limit) {
    QList<QVariantMap> localRows = fetchLocalRows(rconn, table, offset, limit);
    QString pkCol = getPkColumn(rconn, table);

    // Slice remoteRows to same window.
    int remoteStart = offset;
    int remoteEnd =
        (limit < 0) ? remoteRows.size() : std::min(remoteStart + limit, (int)remoteRows.size());
    QList<QVariantMap> remoteSlice = remoteRows.mid(remoteStart, remoteEnd - remoteStart);

    // Build lookup maps keyed by PK value (as string).
    QHash<QString, QVariantMap> localMap;
    for (const QVariantMap& row : localRows) {
        QString key = pkCol.isEmpty() ? QString() : row.value(pkCol).toString();
        if (!key.isEmpty())
            localMap.insert(key, row);
    }

    QHash<QString, QVariantMap> remoteMap;
    for (const QVariantMap& row : remoteSlice) {
        QString key = pkCol.isEmpty() ? QString() : row.value(pkCol).toString();
        if (!key.isEmpty())
            remoteMap.insert(key, row);
    }

    QList<RowDiff> result;

    // Added (in remote, not in local).
    for (auto it = remoteMap.constBegin(); it != remoteMap.constEnd(); ++it) {
        if (!localMap.contains(it.key())) {
            RowDiff rd;
            rd.kind = RowDiffKind::Added;
            rd.primaryKey = it.key();
            rd.cells = compareRows(QVariantMap{}, it.value());
            result.append(rd);
        }
    }

    // Deleted (in local, not in remote).
    for (auto it = localMap.constBegin(); it != localMap.constEnd(); ++it) {
        if (!remoteMap.contains(it.key())) {
            RowDiff rd;
            rd.kind = RowDiffKind::Deleted;
            rd.primaryKey = it.key();
            rd.cells = compareRows(it.value(), QVariantMap{});
            result.append(rd);
        }
    }

    // Modified (in both but different).
    for (auto it = localMap.constBegin(); it != localMap.constEnd(); ++it) {
        if (remoteMap.contains(it.key())) {
            QList<CellDiff> cells = compareRows(it.value(), remoteMap[it.key()]);
            bool anyChanged = std::any_of(cells.constBegin(), cells.constEnd(),
                                          [](const CellDiff& c) { return c.changed; });
            RowDiff rd;
            rd.kind = anyChanged ? RowDiffKind::Modified : RowDiffKind::Same;
            rd.primaryKey = it.key();
            rd.cells = cells;
            result.append(rd);
        }
    }

    return result;
}

QList<QVariantMap> DiffEngine::fetchLocalRows(QSqlDatabase& rconn, const QString& table, int offset,
                                              int limit) {
    QList<QVariantMap> rows;
    QString sql = QString("SELECT * FROM \"%1\"").arg(table);
    if (limit >= 0)
        sql += " LIMIT :lim OFFSET :off";

    QSqlQuery q(rconn);
    q.prepare(sql);
    if (limit >= 0) {
        q.bindValue(":lim", limit);
        q.bindValue(":off", offset);
    }

    if (!q.exec())
        return rows;

    QSqlRecord rec = q.record();
    int colCount = rec.count();
    while (q.next()) {
        QVariantMap row;
        for (int i = 0; i < colCount; ++i)
            row.insert(rec.fieldName(i), q.value(i));
        rows.append(row);
    }
    return rows;
}

QString DiffEngine::getPkColumn(QSqlDatabase& rconn, const QString& table) {
    if (pkColCache_.contains(table))
        return pkColCache_.value(table);

    QSqlQuery q(rconn);
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

QList<CellDiff> DiffEngine::compareRows(const QVariantMap& local, const QVariantMap& remote) {
    QList<CellDiff> cells;

    // Union of all column names.
    QSet<QString> allCols;
    for (auto it = local.constBegin(); it != local.constEnd(); ++it)
        allCols.insert(it.key());
    for (auto it = remote.constBegin(); it != remote.constEnd(); ++it)
        allCols.insert(it.key());

    for (const QString& col : allCols) {
        CellDiff cd;
        cd.column = col;
        cd.localValue = local.value(col);
        cd.remoteValue = remote.value(col);
        cd.changed = (cd.localValue != cd.remoteValue);
        cells.append(cd);
    }

    return cells;
}

}  // namespace dbridge::sync
