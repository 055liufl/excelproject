#include "InboxLedger.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace dbridge::sync {

bool InboxLedger::init(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_inbox_ledger WHERE 0"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool InboxLedger::markSeen(QSqlDatabase& db, const QString& artifactName, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("INSERT OR IGNORE INTO __sync_inbox_ledger "
                       "(artifact_name, status, first_seen_ms) VALUES (?, 'seen', ?)"));
    q.addBindValue(artifactName);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool InboxLedger::markConsumed(QSqlDatabase& db, const QString& artifactName, QString* err) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("UPDATE __sync_inbox_ledger SET status = 'consumed', consumed_ms = ? "
                       "WHERE artifact_name = ?"));
    q.addBindValue(nowMs);
    q.addBindValue(artifactName);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool InboxLedger::markCorrupt(QSqlDatabase& db, const QString& artifactName, QString* err) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "UPDATE __sync_inbox_ledger SET status = 'corrupt' WHERE artifact_name = ?"));
    q.addBindValue(artifactName);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

LedgerStatus InboxLedger::status(QSqlDatabase& db, const QString& artifactName) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT status FROM __sync_inbox_ledger WHERE artifact_name = ?"));
    q.addBindValue(artifactName);
    if (!q.exec() || !q.next())
        return LedgerStatus::Unknown;
    const QString s = q.value(0).toString();
    if (s == QLatin1String("seen"))
        return LedgerStatus::Seen;
    if (s == QLatin1String("consumed"))
        return LedgerStatus::Consumed;
    if (s == QLatin1String("corrupt"))
        return LedgerStatus::Corrupt;
    return LedgerStatus::Unknown;
}

QStringList InboxLedger::pendingSeen(QSqlDatabase& db) {
    QStringList result;
    QSqlQuery q(db);
    if (!q.exec(
            QStringLiteral("SELECT artifact_name FROM __sync_inbox_ledger WHERE status = 'seen'")))
        return result;
    while (q.next())
        result.append(q.value(0).toString());
    return result;
}

}  // namespace dbridge::sync
