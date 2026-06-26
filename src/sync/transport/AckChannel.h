#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QList>

#include "../payload/PayloadCodec.h"
#include "OutboxWriter.h"

namespace dbridge::sync {

// Batches outgoing ACK artifacts and flushes them once ackMaxDelayMs elapses
// or flush() is called explicitly.
class AckChannel {
   public:
    // nodeId: this node's own identifier, used as the "fromPeer" in ACK artifact
    // names so that receivers can parse the sender correctly (J-01 fix).
    explicit AckChannel(OutboxWriter& writer, const QString& nodeId, qint64 ackMaxDelayMs = 5000);

    // Queue a changeset ACK.  May trigger an automatic flush.
    bool scheduleChangesetAck(const ChangesetAck& ack, PayloadCodec& codec, QString* err = nullptr);

    // Queue a push-chunk ACK.  May trigger an automatic flush.
    bool schedulePushChunkAck(const PushChunkAck& ack, PayloadCodec& codec, QString* err = nullptr);

    // Immediately write all queued ACKs to the outbox.
    bool flush(PayloadCodec& codec, QString* err = nullptr);

   private:
    bool maybeFlush(PayloadCodec& codec, QString* err);

    OutboxWriter& writer_;
    QString nodeId_;
    qint64 ackMaxDelayMs_;
    QList<ChangesetAck> pendingChangeset_;
    QList<PushChunkAck> pendingChunk_;
    qint64 lastFlushMs_ = 0;
};

}  // namespace dbridge::sync
