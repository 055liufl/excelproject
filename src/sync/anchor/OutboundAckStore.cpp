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
    // For each origin that this peer has started ACKing, find the local_seq of the first
    // changelog entry whose origin_seq > acked_seq (i.e. not yet confirmed).
    // Return the minimum of those, minus 1, so readRangeAll(afterLocalSeq=result) will
    // include all un-ACKed entries.  If no ACK rows exist, return -1 (read from beginning).
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT COALESCE(MIN(cl.local_seq), -1) - 1 "
                       "FROM __sync_changelog cl "
                       "JOIN __sync_outbound_ack oa "
                       "  ON oa.origin = cl.origin "
                       "  AND oa.stream_epoch = cl.stream_epoch "
                       "  AND oa.peer = ? "
                       "  AND oa.origin != '__broadcast__' "
                       "WHERE cl.stream_epoch = ? "
                       "  AND cl.origin_seq > oa.acked_seq"));
    q.addBindValue(peer);
    q.addBindValue(epoch);
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
