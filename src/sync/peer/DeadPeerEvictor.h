#pragma once
#include <QSqlDatabase>
#include <QString>

#include "../anchor/OutboundAckStore.h"

namespace dbridge::sync {

// Three-dimensional threshold-based peer health evaluator and evictor.
class DeadPeerEvictor {
   public:
    struct PeerState {
        QString peer;
        qint64 lastAckMs = 0;  // epoch ms of last ACK received
        qint64 lagSeq = 0;     // sequences behind local head
        qint64 lagBytes = 0;   // bytes behind local head
        bool evicted = false;
    };

    enum class AlertLevel { Healthy, Lagging, Dead };

    // Configure thresholds.
    void configure(qint64 softSeq, qint64 hardSeq, qint64 softBytes, qint64 hardBytes,
                   qint64 softMs, qint64 hardMs);

    // Evaluate peer health. Returns Healthy/Lagging/Dead.
    AlertLevel evaluate(const PeerState& peer, qint64 nowMs) const;

    // Evict a peer: mark pending_baseline=true, zero its acked_seq.
    bool evict(QSqlDatabase& db, const QString& peer, OutboundAckStore& ack, QString* err);

   private:
    qint64 softSeq_ = 10000;
    qint64 hardSeq_ = 100000;
    qint64 softBytes_ = 50LL * 1024 * 1024;
    qint64 hardBytes_ = 500LL * 1024 * 1024;
    qint64 softMs_ = 300000;
    qint64 hardMs_ = 3600000;
};

}  // namespace dbridge::sync
