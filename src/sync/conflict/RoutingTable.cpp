#include "sync/conflict/RoutingTable.h"

namespace dbridge::sync {

void RoutingTable::configure(const QString& localNodeId, const QStringList& peers) {
    localNodeId_ = localNodeId;
    peers_ = peers;
}

bool RoutingTable::shouldRoute(const QString& peer, const QString& origin, qint64 originSeq,
                               qint64 peerAckedSeq) const {
    // Echo suppression: never forward a change back to its origin
    if (origin == peer) {
        return false;
    }

    // Only forward if the peer has not yet acked this sequence number
    return originSeq > peerAckedSeq;
}

}  // namespace dbridge::sync
