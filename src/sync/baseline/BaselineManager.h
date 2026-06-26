#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

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
        // C-03 fix: per-origin applied-vector snapshot at export time (includes stream_epoch).
        // Replaces the previous QHash<QString,qint64> which lacked stream_epoch, causing
        // applyBaseline() to call av.resetTo() with the local epoch instead of each origin's own.
        QVector<BaselineOriginCut> originCuts;
    };

    // Export all rows from syncTables into a BaselineArtifact.
    // M-01 fix: localOrigin/localEpoch/localOriginSeq are the caller's own identity so that
    // the self origin cut (absent from __sync_applied_vector on the exporting node) is merged
    // into the artifact's originCuts, preventing the receiver from resetting it to 0.
    bool exportBaseline(QSqlDatabase& rconn, const QStringList& tables, BaselineArtifact* out,
                        QString* err, const QString& localOrigin = QString(), qint64 localEpoch = 0,
                        qint64 localOriginSeq = 0);

    // Apply a baseline: import rows into wconn, then reset tracking stores.
    // H-05 fix: schemaFp is written into table_state so DiffEngine can compare fingerprints
    // correctly after baseline apply and avoid false "Different" diffs.
    // M-02 fix: baselineRank is the rank of the baseline origin; used to seed RowWinner entries
    // for every imported row so subsequent low-rank challengers cannot overwrite baseline truth.
    bool applyBaseline(QSqlDatabase& wconn, sqlite3* h, const BaselineArtifact& art,
                       AppliedVectorStore& av, TableStateStore& ts, RowWinnerStore& rw,
                       ConsistencyCache& cache, qint64 epoch, const QString& origin,
                       const QString& schemaFp, qint64* newAnchorSeq, QString* err,
                       int baselineRank = 0);

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
