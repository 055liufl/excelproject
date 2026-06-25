#include "AckChannel.h"

#include <QDateTime>

#include "../SyncDDL.h"

namespace dbridge::sync {

AckChannel::AckChannel(OutboxWriter& writer, qint64 ackMaxDelayMs)
    : writer_(writer), ackMaxDelayMs_(ackMaxDelayMs) {
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
        const QString name = ddl::ackArtifactName(QStringLiteral("self"), ack.origin, nowMs);
        writer_.writeAck(name, data, nullptr);
    }
    pendingChangeset_.clear();

    for (const PushChunkAck& ack : qAsConst(pendingChunk_)) {
        QByteArray data = codec.encodeChunkAck(ack);
        const QString name = ddl::ackArtifactName(QStringLiteral("self"), ack.pushId, nowMs);
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
