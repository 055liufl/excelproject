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
            QString type;
        };
        QMap<int, ColInfo> pkMap;  // pk_seq -> ColInfo
        while (q.next()) {
            int pkSeq = q.value(5).toInt();  // pk column: >0 means part of PK
            bool nn = q.value(3).toBool();   // notnull
            QString colName = q.value(1).toString();
            QString colType = q.value(2).toString();
            if (pkSeq > 0) {
                pkMap.insert(pkSeq, {pkSeq, nn, colType});
                info.pkCols.append(colName);
            }
        }

        // I-17 fix: check NOT NULL for each PK column correctly.
        // Only INTEGER PRIMARY KEY (single-column, type == "INTEGER") is a rowid alias
        // and is implicitly NOT NULL even when PRAGMA notnull reports 0.
        // All other PK columns must have notnull=1 to be eligible.
        info.pkNotNull = true;
        for (auto it = pkMap.begin(); it != pkMap.end(); ++it) {
            const ColInfo& ci = it.value();
            bool isIntegerRowid = (pkMap.size() == 1 &&
                                   ci.type.toUpper() == QLatin1String("INTEGER") && ci.pkSeq == 1);
            if (!isIntegerRowid && !ci.notNull) {
                info.pkNotNull = false;
                break;
            }
        }
    }

    return info;
}

bool SchemaEligibility::hasUpsertTarget(QSqlDatabase& db, const QString& table, QString* err) {
    // PK itself is always a valid ON CONFLICT target — if we got here the table has a PK.
    // Additionally verify no partial unique index is the *only* unique constraint
    // (PK alone is sufficient, so just return true if PK exists).
    // M-02 fix: verify all PK columns are NOT NULL so ON CONFLICT(pk) is a valid target.
    QSqlQuery ti(db);
    ti.prepare(QStringLiteral("PRAGMA table_info(\"%1\")").arg(table));
    if (!ti.exec()) {
        if (err)
            *err = ti.lastError().text();
        return false;
    }
    // H-03 fix: SQLite's INTEGER PRIMARY KEY (rowid alias) always has notnull=0 in PRAGMA
    // table_info, but it is implicitly NOT NULL in practice. Treat single-column INTEGER PK
    // as always NOT NULL to avoid falsely rejecting the most common table pattern.
    struct PkCol {
        int seq;
        bool notNull;
        QString type;
    };
    QList<PkCol> pkCols;
    while (ti.next()) {
        const int pkPos = ti.value(5).toInt();
        if (pkPos > 0)
            pkCols.append({pkPos, ti.value(3).toInt() == 1, ti.value(2).toString().toUpper()});
    }
    if (pkCols.isEmpty()) {
        if (err)
            *err = QStringLiteral("table '%1' has no primary key columns").arg(table);
        return false;
    }
    for (const PkCol& c : pkCols) {
        // Single-column INTEGER PRIMARY KEY is a rowid alias → implicitly NOT NULL.
        const bool isIntegerRowid = (pkCols.size() == 1 && c.type == QLatin1String("INTEGER"));
        if (!isIntegerRowid && !c.notNull) {
            if (err)
                *err =
                    QStringLiteral("table '%1': PK column is nullable; ON CONFLICT target invalid")
                        .arg(table);
            return false;
        }
    }
    return true;
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
