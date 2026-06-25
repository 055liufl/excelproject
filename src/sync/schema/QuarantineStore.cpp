#include "QuarantineStore.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace dbridge::sync {

bool QuarantineStore::init(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_quarantine WHERE 0"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool QuarantineStore::quarantine(QSqlDatabase& db, const QString& origin, qint64 seq, qint64 epoch,
                                 qint64 schemaVer, const QByteArray& payload, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT INTO __sync_quarantine "
        "(origin, origin_seq, stream_epoch, payload_schema_ver, payload, created_ms) "
        "VALUES (?, ?, ?, ?, ?, ?)"));
    q.addBindValue(origin);
    q.addBindValue(seq);
    q.addBindValue(epoch);
    q.addBindValue(schemaVer);
    q.addBindValue(payload);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

QList<QByteArray> QuarantineStore::drainReady(QSqlDatabase& db, qint64 currentSchemaVer) {
    QList<QByteArray> result;

    // Collect in ascending order so callers can replay in-order.
    QSqlQuery sel(db);
    sel.prepare(
        QStringLiteral("SELECT id, payload FROM __sync_quarantine "
                       "WHERE payload_schema_ver <= ? "
                       "ORDER BY origin_seq ASC"));
    sel.addBindValue(currentSchemaVer);
    if (!sel.exec())
        return result;

    QList<qint64> ids;
    while (sel.next()) {
        ids.append(sel.value(0).toLongLong());
        result.append(sel.value(1).toByteArray());
    }

    // Delete the drained rows.
    for (qint64 id : qAsConst(ids)) {
        QSqlQuery del(db);
        del.prepare(QStringLiteral("DELETE FROM __sync_quarantine WHERE id = ?"));
        del.addBindValue(id);
        del.exec();
    }
    return result;
}

}  // namespace dbridge::sync
