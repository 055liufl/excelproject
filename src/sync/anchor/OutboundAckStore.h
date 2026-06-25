#pragma once
#include <QSqlDatabase>
#include <QString>

namespace dbridge::sync {

// Per-peer per-origin ACK water-mark stored in __sync_outbound_ack.
class OutboundAckStore {
   public:
    bool init(QSqlDatabase& db, QString* err);

    // Upsert acked_seq if the incoming value is higher.
    bool updateAcked(QSqlDatabase& db, const QString& peer, const QString& origin, qint64 epoch,
                     qint64 ackedSeq, QString* err);

    // Return current acked_seq for a specific (peer, origin, epoch). -1 if not found.
    qint64 ackedSeq(QSqlDatabase& db, const QString& peer, const QString& origin, qint64 epoch);

    // Return the minimum acked_seq across all peers for a given (origin, epoch).
    // Used as the changelog truncation watermark.  Returns -1 if no rows exist.
    qint64 minAckedSeq(QSqlDatabase& db, const QString& origin, qint64 epoch);

    // Toggle the pending_baseline flag for a peer.
    bool setPendingBaseline(QSqlDatabase& db, const QString& peer, bool pending, QString* err);
};

}  // namespace dbridge::sync
