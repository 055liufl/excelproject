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

QList<QPair<qint64, QByteArray>> QuarantineStore::drainReady(QSqlDatabase& db,
                                                             qint64 currentSchemaVer) {
    QList<QPair<qint64, QByteArray>> result;

    // H-01 fix: order by id ASC (arrival order) so cross-origin replay preserves insertion
    // sequence.
    QSqlQuery sel(db);
    sel.prepare(
        QStringLiteral("SELECT id, payload FROM __sync_quarantine "
                       "WHERE payload_schema_ver <= ? "
                       "ORDER BY id ASC"));
    sel.addBindValue(currentSchemaVer);
    if (!sel.exec())
        return result;

    while (sel.next())
        result.append(qMakePair(sel.value(0).toLongLong(), sel.value(1).toByteArray()));

    return result;
}

void QuarantineStore::markReplayed(QSqlDatabase& db, qint64 id) {
    QSqlQuery del(db);
    del.prepare(QStringLiteral("DELETE FROM __sync_quarantine WHERE id = ?"));
    del.addBindValue(id);
    del.exec();
}

}  // namespace dbridge::sync
