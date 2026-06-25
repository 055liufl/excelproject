#include "sync/peer/DeadPeerEvictor.h"

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
    return ack.setPendingBaseline(db, peer, true, err);
}

}  // namespace dbridge::sync
