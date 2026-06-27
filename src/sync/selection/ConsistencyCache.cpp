#include "ConsistencyCache.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace dbridge::sync {

static constexpr const char* kCreateTable =
    "CREATE TABLE IF NOT EXISTS __sync_consistency_cache ("
    "  table_name      TEXT NOT NULL,"
    "  primary_key     TEXT NOT NULL,"
    "  center_fingerprint BLOB,"
    "  PRIMARY KEY (table_name, primary_key)"
    ")";

bool ConsistencyCache::init(QSqlDatabase& db, bool durable, QString* err) {
    durable_ = durable;
    memCache_.clear();

    if (!durable_)
        return true;

    QSqlQuery q(db);
    if (!q.exec(QLatin1String(kCreateTable))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }

    return loadFromDb(db, err);
}

bool ConsistencyCache::loadFromDb(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT table_name, primary_key, center_fingerprint "
                               "FROM __sync_consistency_cache"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }

    while (q.next()) {
        const QString tbl = q.value(0).toString();
        const QString pk = q.value(1).toString();
        const QByteArray fp = q.value(2).toByteArray();
        memCache_[tbl][pk] = fp;
    }
    return true;
}

bool ConsistencyCache::isConsistent(const QString& table, const QString& pk,
                                    const QByteArray& localFp) const {
    auto tblIt = memCache_.constFind(table);
    if (tblIt == memCache_.constEnd())
        return false;
    auto pkIt = tblIt->constFind(pk);
    if (pkIt == tblIt->constEnd())
        return false;
    return *pkIt == localFp;
}

void ConsistencyCache::stampFromAuthoritative(QSqlDatabase& db, const QString& table,
                                              const QString& pk, const QByteArray& centerFp) {
    memCache_[table][pk] = centerFp;
    if (durable_)
        persistStamp(db, table, pk, centerFp);
}

bool ConsistencyCache::persistStamp(QSqlDatabase& db, const QString& table, const QString& pk,
                                    const QByteArray& fp) {
    // M-01 fix: include updated_ms (NOT NULL in DDL) to prevent INSERT failure.
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("INSERT OR REPLACE INTO __sync_consistency_cache"
                       " (table_name, primary_key, center_fingerprint, updated_ms)"
                       " VALUES (?, ?, ?, ?)"));
    q.addBindValue(table);
    q.addBindValue(pk);
    q.addBindValue(fp);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    return q.exec();
}

void ConsistencyCache::invalidateTable(QSqlDatabase& db, const QString& table) {
    memCache_.remove(table);
    if (durable_)
        deleteTable(db, table);
}

bool ConsistencyCache::deleteTable(QSqlDatabase& db, const QString& table) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM __sync_consistency_cache WHERE table_name = ?"));
    q.addBindValue(table);
    return q.exec();
}

}  // namespace dbridge::sync
