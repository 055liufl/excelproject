#pragma once
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "../apply/AppliedVectorStore.h"
#include "../apply/RowWinnerStore.h"
#include "../schema/TableStateStore.h"
#include "../selection/ConsistencyCache.h"
#include <sqlite3.h>

namespace dbridge::sync {

// Handles full-table baseline export and apply.
// Export: serialize all rows as a flat binary format.
// Apply: import rows, reset all vector/state stores.
class BaselineManager {
   public:
    struct BaselineArtifact {
        QByteArray data;          // serialized baseline data (compressed)
        qint64 sourceMaxSeq = 0;  // max local_seq at export time (diagnostic only)
        // C-03 fix: per-origin max origin_seq at export time, used to reset applied_vector
        // to the correct authoritative truncation point (MAX(origin_seq) per origin).
        QHash<QString, qint64> originMaxSeq;
    };

    // Export all rows from syncTables into a BaselineArtifact.
    bool exportBaseline(QSqlDatabase& rconn, const QStringList& tables, BaselineArtifact* out,
                        QString* err);

    // Apply a baseline: import rows into wconn, then reset tracking stores.
    // H-05 fix: schemaFp is written into table_state so DiffEngine can compare fingerprints
    // correctly after baseline apply and avoid false "Different" diffs.
    bool applyBaseline(QSqlDatabase& wconn, sqlite3* h, const BaselineArtifact& art,
                       AppliedVectorStore& av, TableStateStore& ts, RowWinnerStore& rw,
                       ConsistencyCache& cache, qint64 epoch, const QString& origin,
                       const QString& schemaFp, qint64* newAnchorSeq, QString* err);

    // True if we should fall back to baseline (source has already compacted
    // the changesets we need).
    bool shouldFallbackToBaseline(qint64 appliedSeq, qint64 sourceMinSeq) const;

   private:
    // Serialize tables to QByteArray (QDataStream format, then qCompress).
    bool serializeTables(QSqlDatabase& rconn, const QStringList& tables, QByteArray* out,
                         qint64* maxSeq, QString* err);

    // Deserialize and UPSERT all rows.
    bool deserializeAndApply(QSqlDatabase& wconn, const QByteArray& data, QStringList* tables,
                             QString* err);
};

}  // namespace dbridge::sync
