#include "AckChannel.h"

#include <QDateTime>

#include "../SyncDDL.h"

namespace dbridge::sync {

AckChannel::AckChannel(OutboxWriter& writer, const QString& nodeId, qint64 ackMaxDelayMs)
    : writer_(writer), nodeId_(nodeId), ackMaxDelayMs_(ackMaxDelayMs) {
    lastFlushMs_ = QDateTime::currentMSecsSinceEpoch();
}

void AckChannel::scheduleChangesetAck(const ChangesetAck& ack, PayloadCodec& codec) {
    pendingChangeset_.append(ack);
    maybeFlush(codec);
}

void AckChannel::schedulePushChunkAck(const PushChunkAck& ack, PayloadCodec& codec) {
    pendingChunk_.append(ack);
    maybeFlush(codec);
}

void AckChannel::flush(PayloadCodec& codec) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    for (const ChangesetAck& ack : qAsConst(pendingChangeset_)) {
        QByteArray data = codec.encodeChangesetAck(ack);
        // fromPeer = nodeId_ (this node), toPeer = ack.toPeer (J-01 fix: was hardcoded "self")
        const QString name = ddl::ackArtifactName(nodeId_, ack.toPeer, nowMs);
        writer_.writeAck(name, data, nullptr);
    }
    pendingChangeset_.clear();

    for (const PushChunkAck& ack : qAsConst(pendingChunk_)) {
        QByteArray data = codec.encodeChunkAck(ack);
        // PushChunkAck does not carry a toPeer yet; keep existing routing tag.
        // TODO(J-01): extend PushChunkAck with toPeer when push fan-out is implemented.
        const QString name = ddl::ackArtifactName(nodeId_, ack.pushId, nowMs);
        writer_.writeAck(name, data, nullptr);
    }
    pendingChunk_.clear();

    lastFlushMs_ = nowMs;
}

void AckChannel::maybeFlush(PayloadCodec& codec) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - lastFlushMs_ >= ackMaxDelayMs_)
        flush(codec);
}

}  // namespace dbridge::sync
