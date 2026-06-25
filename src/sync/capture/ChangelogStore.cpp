#include "ChangelogStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace dbridge::sync {

bool ChangelogStore::init(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_changelog WHERE 0"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool ChangelogStore::append(QSqlDatabase& db, const QString& kind, const QString& origin,
                            const QString& sourcePeer, qint64 originSeq, qint64 parentSeq,
                            qint64 epoch, qint64 schemaVer, const QString& schemaFp,
                            const QByteArray& changeset, bool authoritative, qint64* localSeqOut,
                            QString* err) {
    return insertRow(db, kind, origin, sourcePeer, originSeq, parentSeq, epoch, schemaVer, schemaFp,
                     changeset, authoritative, localSeqOut, err);
}

bool ChangelogStore::appendForward(QSqlDatabase& db, const QString& origin,
                                   const QString& sourcePeer, qint64 originSeq, qint64 epoch,
                                   qint64 schemaVer, const QString& schemaFp,
                                   const QByteArray& changesetBlob, qint64* localSeqOut,
                                   QString* err) {
    // forwarded changesets: kind="forward", parentSeq=0, authoritative=false
    return insertRow(db, QStringLiteral("forward"), origin, sourcePeer, originSeq, /*parentSeq=*/0,
                     epoch, schemaVer, schemaFp, changesetBlob, /*authoritative=*/false,
                     localSeqOut, err);
}

QList<ChangelogStore::Entry> ChangelogStore::readRange(QSqlDatabase& db, const QString& peer,
                                                       qint64 afterOriginSeq, int limit) {
    // peer-based range: return entries for the given origin after afterOriginSeq.
    // Caller sets peer == origin for same-origin reads.
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT local_seq, origin, origin_seq, changeset, byte_size "
                       "FROM __sync_changelog "
                       "WHERE origin = ? AND origin_seq > ? "
                       "ORDER BY origin_seq ASC "
                       "LIMIT ?"));
    q.addBindValue(peer);
    q.addBindValue(afterOriginSeq);
    q.addBindValue(limit);

    QList<Entry> result;
    if (!q.exec())
        return result;
    while (q.next()) {
        Entry e;
        e.localSeq = q.value(0).toLongLong();
        e.origin = q.value(1).toString();
        e.originSeq = q.value(2).toLongLong();
        e.changeset = q.value(3).toByteArray();
        e.byteSize = q.value(4).toLongLong();
        result.append(e);
    }
    return result;
}

QList<ChangelogStore::EntryFull> ChangelogStore::readRangeAll(QSqlDatabase& db,
                                                              const QString& excludeOrigin,
                                                              qint64 afterLocalSeq, int limit) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT local_seq, origin, origin_seq, changeset, byte_size "
                       "FROM __sync_changelog "
                       "WHERE origin != ? AND local_seq > ? "
                       "ORDER BY local_seq ASC "
                       "LIMIT ?"));
    q.addBindValue(excludeOrigin);
    q.addBindValue(afterLocalSeq);
    q.addBindValue(limit);

    QList<EntryFull> result;
    if (!q.exec())
        return result;
    while (q.next()) {
        EntryFull e;
        e.localSeq = q.value(0).toLongLong();
        e.origin = q.value(1).toString();
        e.originSeq = q.value(2).toLongLong();
        e.changeset = q.value(3).toByteArray();
        e.byteSize = q.value(4).toLongLong();
        result.append(e);
    }
    return result;
}

qint64 ChangelogStore::maxLocalSeq(QSqlDatabase& db) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COALESCE(MAX(local_seq), -1) FROM __sync_changelog")))
        return -1;
    if (!q.next())
        return -1;
    return q.value(0).toLongLong();
}

bool ChangelogStore::truncate(QSqlDatabase& db, qint64 beforeLocalSeq, QString* err) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM __sync_changelog WHERE local_seq < ?"));
    q.addBindValue(beforeLocalSeq);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// private
// ---------------------------------------------------------------------------

bool ChangelogStore::insertRow(QSqlDatabase& db, const QString& kind, const QString& origin,
                               const QString& sourcePeer, qint64 originSeq, qint64 parentSeq,
                               qint64 epoch, qint64 schemaVer, const QString& schemaFp,
                               const QByteArray& changeset, bool authoritative, qint64* localSeqOut,
                               QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QString checksum = blobChecksum(changeset);
    const qint64 byteSize = changeset.size();

    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("INSERT OR IGNORE INTO __sync_changelog "
                       "(kind, origin, source_peer, origin_seq, parent_seq, stream_epoch, "
                       " schema_ver, schema_fingerprint, changeset, payload_checksum, "
                       " byte_size, authoritative, created_ms) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(kind);
    q.addBindValue(origin);
    q.addBindValue(sourcePeer.isEmpty() ? QVariant(QVariant::String) : QVariant(sourcePeer));
    q.addBindValue(originSeq);
    q.addBindValue(parentSeq == 0 ? QVariant(QVariant::LongLong) : QVariant(parentSeq));
    q.addBindValue(epoch);
    q.addBindValue(schemaVer);
    q.addBindValue(schemaFp);
    q.addBindValue(changeset);
    q.addBindValue(checksum);
    q.addBindValue(byteSize);
    q.addBindValue(authoritative ? 1 : 0);
    q.addBindValue(nowMs);

    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }

    if (localSeqOut) {
        *localSeqOut = q.lastInsertId().toLongLong();
    }
    return true;
}

QString ChangelogStore::blobChecksum(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

}  // namespace dbridge::sync
