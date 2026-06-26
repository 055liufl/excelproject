#include "OutboundAckStore.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace dbridge::sync {

bool OutboundAckStore::init(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_outbound_ack WHERE 0"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool OutboundAckStore::updateAcked(QSqlDatabase& db, const QString& peer, const QString& origin,
                                   qint64 epoch, qint64 ackedSeq_, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("INSERT INTO __sync_outbound_ack "
                       "(peer, origin, stream_epoch, acked_seq, last_ack_ms) "
                       "VALUES (?, ?, ?, ?, ?) "
                       "ON CONFLICT(peer, origin, stream_epoch) DO UPDATE SET "
                       "  acked_seq  = MAX(excluded.acked_seq, __sync_outbound_ack.acked_seq), "
                       "  last_ack_ms = excluded.last_ack_ms"));
    q.addBindValue(peer);
    q.addBindValue(origin);
    q.addBindValue(epoch);
    q.addBindValue(ackedSeq_);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

qint64 OutboundAckStore::ackedSeq(QSqlDatabase& db, const QString& peer, const QString& origin,
                                  qint64 epoch) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT acked_seq FROM __sync_outbound_ack "
                       "WHERE peer = ? AND origin = ? AND stream_epoch = ?"));
    q.addBindValue(peer);
    q.addBindValue(origin);
    q.addBindValue(epoch);
    if (!q.exec() || !q.next())
        return -1;
    return q.value(0).toLongLong();
}

qint64 OutboundAckStore::minAckedSeq(QSqlDatabase& db, const QString& origin, qint64 epoch) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT MIN(acked_seq) FROM __sync_outbound_ack "
                       "WHERE origin = ? AND stream_epoch = ? AND pending_baseline = 0"));
    q.addBindValue(origin);
    q.addBindValue(epoch);
    if (!q.exec() || !q.next())
        return -1;
    if (q.value(0).isNull())
        return -1;
    return q.value(0).toLongLong();
}

qint64 OutboundAckStore::lastSentLocalSeq(QSqlDatabase& db, const QString& peer, qint64 epoch) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT COALESCE(MAX(last_sent_seq), -1) FROM __sync_outbound_ack "
                       "WHERE peer = ? AND stream_epoch = ? AND origin = '__broadcast__'"));
    q.addBindValue(peer);
    q.addBindValue(epoch);
    if (!q.exec() || !q.next())
        return -1;
    if (q.value(0).isNull())
        return -1;
    return q.value(0).toLongLong();
}

bool OutboundAckStore::updateLastSent(QSqlDatabase& db, const QString& peer, qint64 epoch,
                                      qint64 lastSentLocalSeq_, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    // UPSERT a sentinel row keyed on (peer, '__broadcast__', epoch).
    // last_sent_seq advances monotonically (MAX semantics).
    q.prepare(
        QStringLiteral("INSERT INTO __sync_outbound_ack "
                       "(peer, origin, stream_epoch, acked_seq, last_sent_seq, last_ack_ms) "
                       "VALUES (?, '__broadcast__', ?, -1, ?, ?) "
                       "ON CONFLICT(peer, origin, stream_epoch) DO UPDATE SET "
                       "  last_sent_seq = MAX(excluded.last_sent_seq, "
                       "                      __sync_outbound_ack.last_sent_seq), "
                       "  last_ack_ms   = excluded.last_ack_ms"));
    q.addBindValue(peer);
    q.addBindValue(epoch);
    q.addBindValue(lastSentLocalSeq_);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

qint64 OutboundAckStore::minUnackedLocalSeq(QSqlDatabase& db, const QString& peer, qint64 epoch) {
    // C-02 fix: use LEFT JOIN so origins without an ACK row are treated as acked_seq = -1
    // (i.e. nothing has been confirmed yet) rather than being silently excluded.
    // With an inner JOIN, an origin that never appeared in outbound_ack would be missed
    // entirely, causing broadcastToPeer to skip its changelog entries.
    // M-02 fix: do NOT filter by cl.stream_epoch so that cross-epoch un-ACKed entries are
    // included in the lower-bound calculation.  The LEFT JOIN already matches on
    // (peer, origin, stream_epoch) as a three-part key, so each changelog row is correctly
    // paired with its own epoch's ACK row (or treated as unacked when absent).
    // epoch parameter is kept for API compatibility but no longer used in the WHERE clause.
    Q_UNUSED(epoch)
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT COALESCE(MIN(cl.local_seq), -1) - 1 "
                       "FROM __sync_changelog cl "
                       "LEFT JOIN __sync_outbound_ack oa "
                       "  ON oa.origin = cl.origin "
                       "  AND oa.stream_epoch = cl.stream_epoch "
                       "  AND oa.peer = ? "
                       "  AND oa.origin != '__broadcast__' "
                       "WHERE cl.origin_seq > COALESCE(oa.acked_seq, -1)"));
    q.addBindValue(peer);
    if (!q.exec() || !q.next())
        return -1;
    if (q.value(0).isNull())
        return -1;
    return q.value(0).toLongLong();
}

bool OutboundAckStore::setPendingBaseline(QSqlDatabase& db, const QString& peer, bool pending,
                                          QString* err) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE __sync_outbound_ack SET pending_baseline = ? WHERE peer = ?"));
    q.addBindValue(pending ? 1 : 0);
    q.addBindValue(peer);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

}  // namespace dbridge::sync
