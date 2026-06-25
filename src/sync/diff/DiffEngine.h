#pragma once
#include "dbridge/sync/IComparisonSession.h"

#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

#include "../schema/TableStateStore.h"

namespace dbridge::sync {

class DiffEngine {
   public:
    struct RemoteMeta {
        QString schemaFp;
        QString checksum;  // hex quint64 modular sum
        qint64 rowCount = 0;
    };

    // Table-level diff: compare local TableStateStore state vs remote metadata.
    // streamEpoch used to look up local state.
    QList<TableDiff> tableDiffs(QSqlDatabase& rconn, const QStringList& tables, qint64 streamEpoch,
                                TableStateStore& localTs, const QHash<QString, RemoteMeta>& remote);

    // Row-level diff: compare rows in local table vs remoteRows.
    // offset/limit for pagination.
    QList<RowDiff> rowDiffs(QSqlDatabase& rconn, const QString& table,
                            const QList<QVariantMap>& remoteRows, int offset, int limit);

   private:
    // Fetch rows from local table as QList<QVariantMap>.
    QList<QVariantMap> fetchLocalRows(QSqlDatabase& rconn, const QString& table, int offset,
                                      int limit);
    // Get PK column for table via PRAGMA table_info.
    QString getPkColumn(QSqlDatabase& rconn, const QString& table);
    // Compare two rows cell by cell.
    QList<CellDiff> compareRows(const QVariantMap& local, const QVariantMap& remote);
    QHash<QString, QString> pkColCache_;
};

}  // namespace dbridge::sync
