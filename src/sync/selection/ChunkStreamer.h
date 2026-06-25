#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QList>
#include <QString>
#include <QUuid>

#include "../payload/PayloadCodec.h"
#include "FkClosureBuilder.h"

namespace dbridge::sync {

// Splits a topologically sorted manifest into chunks within budget.
class ChunkStreamer {
   public:
    struct Chunk {
        QString pushId;
        int chunkSeq = 0;
        int totalChunks = 0;
        QList<FrozenEntry> entries;
        QList<QVariantMap> rows;
    };

    // Stream manifest into chunks. pushId is generated (UUID) if empty.
    // chunkBudgetBytes: max serialized bytes per chunk.
    bool stream(const QList<FkClosureBuilder::Entry>& manifest, const QString& origin,
                const QString& targetPeer, qint64 chunkBudgetBytes, PayloadCodec& codec,
                QList<Chunk>* chunks, QString* err);
};

}  // namespace dbridge::sync
