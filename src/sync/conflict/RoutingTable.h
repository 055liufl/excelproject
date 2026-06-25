#pragma once
#include <QString>
#include <QStringList>

namespace dbridge::sync {

// Prevents echo routing: a change is only sent to a peer if:
//   1. origin != peer (no echo)
//   2. originSeq > peerAckedSeq (peer hasn't seen it yet)
class RoutingTable {
   public:
    void configure(const QString& localNodeId, const QStringList& peers);

    // Returns true if the change from `origin` with `originSeq` should be
    // forwarded to `peer`. peerAckedSeq is that peer's current acked watermark.
    bool shouldRoute(const QString& peer, const QString& origin, qint64 originSeq,
                     qint64 peerAckedSeq) const;

   private:
    QString localNodeId_;
    QStringList peers_;
};

}  // namespace dbridge::sync
