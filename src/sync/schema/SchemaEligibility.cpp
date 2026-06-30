#include "SchemaEligibility.h"

#include "dbridge/Errors.h"  // err::E_SYNC_COMPOSITE_PK_NOT_SUPPORTED 等错误码

#include <QMap>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

// ── expandSyncTables —— 把「空列表」解释为「全部业务表」───────────────────────
// 做什么：若调用方显式给了表名列表，原样返回；若给的是空列表，则查 sqlite_master
//         取出所有用户表（排除 sqlite_ 内建表与 __sync_ 同步元数据表）。
// 为什么：让上层有「不指定 = 同步全部业务表」的便捷语义，同时绝不把内建/元数据表
//         卷入同步（那会造成无限自指与污染）。
// 返回：规范化后的表名集（按名排序）；查询失败时返回空列表并写 *err。
QStringList SchemaEligibility::expandSyncTables(QSqlDatabase& db, const QStringList& syncTables,
                                                QString* err) {
    if (!syncTables.isEmpty())
        return syncTables;  // 调用方已显式指定，直接采用，不做展开

    // C-08 fix: empty = all user tables (exclude SQLite internals and sync meta tables).
    // C-08 修复（译）：空 = 全部用户表。两个 NOT LIKE 过滤掉：
    //   · sqlite_%   —— SQLite 内建对象（如 sqlite_sequence、自动索引）；
    //   · __sync_%   —— 本同步子系统自己的元数据表（changelog/ledger/quarantine 等）。
    // ORDER BY name 让结果稳定有序，便于日志比对与可复现。
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' "
                               "AND name NOT LIKE 'sqlite_%' AND name NOT LIKE '__sync_%' "
                               "ORDER BY name"))) {
        if (err)
            *err = q.lastError().text();
        return {};
    }
    QStringList tables;
    while (q.next())
        tables.append(q.value(0).toString());
    return tables;
}

// ── verify —— 逐表准入体检的主流程 ───────────────────────────────────────────
// 做什么：对每张表先 introspect() 取结构事实，再按「拒绝清单」逐条判定，把不合格表
//         连同原因追加进 rejected。
// 控制流要点：
//   · introspect 报错（localErr 非空）→ 视为「体检过程本身失败」，立即 return false 并写 err；
//   · 表不合格 → 调用本地 lambda reject() 记一条原因、置 allOk=false，然后 continue
//     （继续体检后续表，以便一次性把所有问题都列出来，而不是发现第一个就停）。
// 返回：全合格 true；有不合格项 false；体检出错 false+err。
bool SchemaEligibility::verify(QSqlDatabase& db, const QStringList& syncTables,
                               QStringList* rejected, QString* err) {
    bool allOk = true;
    for (const QString& tbl : syncTables) {
        QString localErr;
        TableInfo info = introspect(db, tbl, &localErr);
        if (!localErr.isEmpty()) {
            // 自省过程出错（如 PRAGMA 查询失败）——这是基础设施错误，非「表不合格」，
            // 直接中止整个体检并把错误透传给调用方。
            if (err)
                *err = localErr;
            return false;
        }

        // 本地辅助：记录一条「不合格原因」并把整体结果标记为有失败项。
        // 捕获 rejected/allOk/tbl 引用，调用方便。
        auto reject = [&](const QString& reason) {
            if (rejected)
                rejected->append(QStringLiteral("%1: %2").arg(tbl, reason));
            allOk = false;
        };

        // —— 按「拒绝清单」逐条判定；命中即记因 + continue（跳到下一张表）——
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
        // M-02 fix: composite PK rejection must clearly state the MVP limitation so developers
        // understand this is a known constraint, not a general sync bug.
        if (info.pkCols.size() > 1) {
            reject(
                QLatin1String(err::E_SYNC_COMPOSITE_PK_NOT_SUPPORTED) +
                QStringLiteral(": composite PRIMARY KEY (%1 columns) is not supported — "
                               "MVP phase supports only single-column INTEGER or TEXT PRIMARY KEY; "
                               "add a surrogate single-column PK or use a UNIQUE index instead")
                    .arg(info.pkCols.size()));
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
        // M-03 fix: double-quote identifier to handle names with spaces/reserved words.
        const QString quotedTbl = QStringLiteral("\"") +
                                  QString(table).replace(QLatin1Char('"'), QLatin1String("\"\"")) +
                                  QStringLiteral("\"");
        q.exec(QStringLiteral("PRAGMA table_info(") + quotedTbl + QLatin1Char(')'));
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
    // M-03 fix: double-quote identifier.
    QSqlQuery ti(db);
    const QString quotedTbl2 = QStringLiteral("\"") +
                               QString(table).replace(QLatin1Char('"'), QLatin1String("\"\"")) +
                               QStringLiteral("\"");
    ti.prepare(QStringLiteral("PRAGMA table_info(") + quotedTbl2 + QLatin1Char(')'));
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
