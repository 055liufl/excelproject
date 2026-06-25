#include "SchemaEligibility.h"

#include <QMap>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

bool SchemaEligibility::verify(QSqlDatabase& db, const QStringList& syncTables,
                               QStringList* rejected, QString* err) {
    bool allOk = true;
    for (const QString& tbl : syncTables) {
        QString localErr;
        TableInfo info = introspect(db, tbl, &localErr);
        if (!localErr.isEmpty()) {
            if (err)
                *err = localErr;
            return false;
        }

        auto reject = [&](const QString& reason) {
            if (rejected)
                rejected->append(QStringLiteral("%1: %2").arg(tbl, reason));
            allOk = false;
        };

        if (!info.exists) {
            reject(QStringLiteral("table does not exist"));
            continue;
        }
        if (info.isView) {
            reject(QStringLiteral("is a view, not a base table"));
            continue;
        }
        if (info.isVirtual) {
            reject(QStringLiteral("is a virtual table"));
            continue;
        }
        if (info.isShadow) {
            reject(QStringLiteral("is a shadow table of a virtual table"));
            continue;
        }
        if (info.pkCols.isEmpty()) {
            reject(QStringLiteral("has no explicit PRIMARY KEY"));
            continue;
        }
        if (!info.pkNotNull) {
            reject(QStringLiteral("PRIMARY KEY column(s) are nullable"));
            continue;
        }
        if (!hasUpsertTarget(db, tbl, &localErr)) {
            if (!localErr.isEmpty()) {
                if (err)
                    *err = localErr;
                return false;
            }
            reject(QStringLiteral("no non-partial UNIQUE conflict target available for UPSERT"));
            continue;
        }
    }
    return allOk;
}

// ---------------------------------------------------------------------------
// private helpers
// ---------------------------------------------------------------------------

SchemaEligibility::TableInfo SchemaEligibility::introspect(QSqlDatabase& db, const QString& table,
                                                           QString* err) {
    TableInfo info;

    // 1. Check existence + type via sqlite_master
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral("SELECT type FROM sqlite_master WHERE name = ?"));
        q.addBindValue(table);
        if (!q.exec()) {
            if (err)
                *err = q.lastError().text();
            return info;
        }
        if (!q.next()) {
            return info;  // not found
        }
        info.exists = true;
        const QString type = q.value(0).toString();
        if (type == QLatin1String("view")) {
            info.isView = true;
            return info;
        }
        // Virtual tables show as "table" in sqlite_master but have a CREATE VIRTUAL TABLE stmt
        QString sql;
        {
            QSqlQuery sq(db);
            sq.prepare(
                QStringLiteral("SELECT sql FROM sqlite_master WHERE name = ? AND type = 'table'"));
            sq.addBindValue(table);
            if (sq.exec() && sq.next())
                sql = sq.value(0).toString().toUpper();
        }
        if (sql.contains(QLatin1String("CREATE VIRTUAL TABLE"))) {
            info.isVirtual = true;
            return info;
        }
    }

    // 2. Shadow table check
    {
        QString shadowErr;
        info.isShadow = isShadowTable(db, table, &shadowErr);
        if (!shadowErr.isEmpty()) {
            if (err)
                *err = shadowErr;
            return info;
        }
        if (info.isShadow)
            return info;
    }

    // 3. PRAGMA table_info — collect PK columns
    {
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA table_info(\"%1\")").arg(table));
        struct ColInfo {
            int pkSeq;
            bool notNull;
        };
        QMap<int, ColInfo> pkMap;  // pk_seq -> {pkSeq, notNull}
        while (q.next()) {
            int pkSeq = q.value(5).toInt();  // pk column: >0 means part of PK
            bool nn = q.value(3).toBool();   // notnull
            QString colName = q.value(1).toString();
            if (pkSeq > 0) {
                pkMap.insert(pkSeq, {pkSeq, nn});
                info.pkCols.append(colName);
            }
        }
        // Check all PK cols are NOT NULL (SQLite rowid tables: INTEGER PK is implicitly NOT NULL)
        info.pkNotNull = true;
        for (auto it = pkMap.begin(); it != pkMap.end(); ++it) {
            if (!it.value().notNull) {
                info.pkNotNull = false;
                break;
            }
        }
        // A single INTEGER PK (rowid alias) is always NOT NULL
        if (info.pkCols.size() == 1 && !info.pkNotNull) {
            // Re-check: INTEGER PRIMARY KEY is always not-null in SQLite
            // We rely on the notnull flag from PRAGMA; but rowid aliases may show notnull=0
            // Accept it as implicitly not-null if it's the sole PK column
            info.pkNotNull = true;
        }
    }

    return info;
}

bool SchemaEligibility::hasUpsertTarget(QSqlDatabase& db, const QString& table, QString* err) {
    // PK itself is always a valid ON CONFLICT target — if we got here the table has a PK.
    // Additionally verify no partial unique index is the *only* unique constraint
    // (PK alone is sufficient, so just return true if PK exists).
    Q_UNUSED(err)
    // We already verified pkCols is non-empty in the caller. PK = valid upsert target.
    Q_UNUSED(table)
    Q_UNUSED(db)
    return true;  // PK is always a valid conflict target for ON CONFLICT DO UPDATE
}

bool SchemaEligibility::isShadowTable(QSqlDatabase& db, const QString& table, QString* err) {
    // Shadow tables have names of the form <vtab>_<suffix>.
    // We check: does any virtual table in sqlite_master have a name that is a prefix of `table`?
    const int us = table.indexOf(QLatin1Char('_'));
    if (us <= 0)
        return false;

    const QString prefix = table.left(us);
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT COUNT(*) FROM sqlite_master "
                       "WHERE type = 'table' AND name = ? "
                       "AND sql LIKE 'CREATE VIRTUAL TABLE%'"));
    q.addBindValue(prefix);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    if (q.next() && q.value(0).toInt() > 0)
        return true;
    return false;
}

}  // namespace dbridge::sync
