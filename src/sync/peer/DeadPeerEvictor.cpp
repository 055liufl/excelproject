#include "sync/peer/DeadPeerEvictor.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace dbridge::sync {

void DeadPeerEvictor::configure(qint64 softSeq, qint64 hardSeq, qint64 softBytes, qint64 hardBytes,
                                qint64 softMs, qint64 hardMs) {
    softSeq_ = softSeq;
    hardSeq_ = hardSeq;
    softBytes_ = softBytes;
    hardBytes_ = hardBytes;
    softMs_ = softMs;
    hardMs_ = hardMs;
}

DeadPeerEvictor::AlertLevel DeadPeerEvictor::evaluate(const PeerState& peer, qint64 nowMs) const {
    if (peer.evicted) {
        return AlertLevel::Dead;
    }

    const bool hasTimeData = peer.lastAckMs > 0;
    const qint64 msLag = hasTimeData ? (nowMs - peer.lastAckMs) : 0;

    // Hard thresholds → Dead
    if (peer.lagSeq >= hardSeq_)
        return AlertLevel::Dead;
    if (peer.lagBytes >= hardBytes_)
        return AlertLevel::Dead;
    if (hasTimeData && msLag >= hardMs_)
        return AlertLevel::Dead;

    // Soft thresholds → Lagging
    if (peer.lagSeq >= softSeq_)
        return AlertLevel::Lagging;
    if (peer.lagBytes >= softBytes_)
        return AlertLevel::Lagging;
    if (hasTimeData && msLag >= softMs_)
        return AlertLevel::Lagging;

    return AlertLevel::Healthy;
}

bool DeadPeerEvictor::evict(QSqlDatabase& db, const QString& peer, OutboundAckStore& ack,
                            QString* err) {
    if (!ack.setPendingBaseline(db, peer, true, err))
        return false;

    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE __sync_outbound_ack SET acked_seq = -1 WHERE peer = ?"));
    q.addBindValue(QVariant(peer));
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

}  // namespace dbridge::sync
