#include "AckChannel.h"

#include <QDateTime>
#include <QStringList>

#include "../SyncDDL.h"

namespace dbridge::sync {

AckChannel::AckChannel(OutboxWriter& writer, const QString& nodeId, qint64 ackMaxDelayMs)
    : writer_(writer), nodeId_(nodeId), ackMaxDelayMs_(ackMaxDelayMs) {
    lastFlushMs_ = QDateTime::currentMSecsSinceEpoch();
}

bool AckChannel::scheduleChangesetAck(const ChangesetAck& ack, PayloadCodec& codec, QString* err) {
    pendingChangeset_.append(ack);
    return maybeFlush(codec, err);
}

bool AckChannel::schedulePushChunkAck(const PushChunkAck& ack, PayloadCodec& codec, QString* err) {
    pendingChunk_.append(ack);
    return maybeFlush(codec, err);
}

bool AckChannel::flush(PayloadCodec& codec, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    bool ok = true;
    QStringList failures;
    QList<ChangesetAck> failedChangeset;
    for (const ChangesetAck& ack : qAsConst(pendingChangeset_)) {
        QByteArray data = codec.encodeChangesetAck(ack);
        // fromPeer = nodeId_ (this node), toPeer = ack.toPeer (J-01 fix: was hardcoded "self")
        const QString name = ddl::ackArtifactName(nodeId_, ack.toPeer, nowMs);
        QString writeErr;
        if (!writer_.writeAck(name, data, &writeErr)) {
            ok = false;
            failedChangeset.append(ack);
            failures.append(QStringLiteral("%1: %2").arg(name, writeErr));
        }
    }

    QList<PushChunkAck> failedChunk;
    for (const PushChunkAck& ack : qAsConst(pendingChunk_)) {
        QByteArray data = codec.encodeChunkAck(ack);
        // H-04 fix: use toPeer (the push origin) so the ACK file is routed correctly.
        const QString toPeer = ack.toPeer.isEmpty() ? ack.pushId : ack.toPeer;
        const QString name = ddl::ackArtifactName(nodeId_, toPeer, nowMs);
        QString writeErr;
        if (!writer_.writeAck(name, data, &writeErr)) {
            ok = false;
            failedChunk.append(ack);
            failures.append(QStringLiteral("%1: %2").arg(name, writeErr));
        }
    }

    pendingChangeset_ = std::move(failedChangeset);
    pendingChunk_ = std::move(failedChunk);
    lastFlushMs_ = nowMs;
    if (!ok && err)
        *err = failures.join(QStringLiteral("; "));
    return ok;
}

bool AckChannel::maybeFlush(PayloadCodec& codec, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - lastFlushMs_ >= ackMaxDelayMs_)
        return flush(codec, err);
    return true;
}

}  // namespace dbridge::sync
