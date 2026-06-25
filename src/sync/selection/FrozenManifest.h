#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>

namespace dbridge::sync {

// Persistent frozen manifest: the topology-ordered list of (table, pk, fingerprint)
// entries for a push-id. Serves as the source for ChunkStreamer resumption (C13).
class FrozenManifest {
   public:
    bool init(QSqlDatabase& db, QString* err);

    // Persist entries for a push (called before releasing the read snapshot, C16)
    bool save(QSqlDatabase& db, const QString& pushId, int chunkSeq,
              const QList<FrozenEntry>& entries, QString* err);

    // Load entries for an existing push (for resumption)
    QList<FrozenEntry> load(QSqlDatabase& db, const QString& pushId, int chunkSeq);

    // Cleanup finished push
    bool remove(QSqlDatabase& db, const QString& pushId, QString* err);
};

}  // namespace dbridge::sync
