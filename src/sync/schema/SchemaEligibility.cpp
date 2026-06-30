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

// ── introspect —— 体检单张表，把「与准入相关」的结构事实填进 TableInfo ──────────
// 做什么：分四步自省一张表的结构——
//   1) 查 sqlite_master 确认表是否存在、是 view 还是 table；若是 table 再看其建表 SQL
//      是否为 "CREATE VIRTUAL TABLE"（虚表在 sqlite_master 里 type 仍记作 'table'，
//      只能靠建表语句区分）。
//   2) 影子表判定（委托 isShadowTable）。
//   3) PRAGMA table_info 收集 PK 列及其 NOT NULL 性。
//   （凡命中 view/virtual/shadow 等「明确不合格」情形即尽早 return，不再继续后续步骤。）
// 为什么「尽早 return」：一旦确定是视图/虚表/影子表，PK 等信息已无意义，省去无用查询。
// 参数：db 连接；table 表名；err（可空）查询出错时写入 SQL 错误文本。
// 返回：填好的 TableInfo（出错时相关字段保持默认值，由 verify 据 err 非空判定为体检失败）。
// 复杂度：每表常数条 PRAGMA / sqlite_master 查询。
SchemaEligibility::TableInfo SchemaEligibility::introspect(QSqlDatabase& db, const QString& table,
                                                           QString* err) {
    TableInfo info;

    // 1. Check existence + type via sqlite_master
    // 【译】第 1 步：经 sqlite_master 查「是否存在 + 对象类型」。
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
            return info;  // not found  【译】无此对象 → exists 保持 false
        }
        info.exists = true;
        const QString type = q.value(0).toString();
        if (type == QLatin1String("view")) {
            info.isView = true;
            return info;  // 视图不可写 → 直接判不合格
        }
        // Virtual tables show as "table" in sqlite_master but have a CREATE VIRTUAL TABLE stmt
        // 【译】虚表在 sqlite_master 里的 type 同样是 "table"，只能靠建表语句里是否含
        //   "CREATE VIRTUAL TABLE" 来辨认。下面把建表 SQL 取出转大写后做包含判断。
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
    // 【译】第 2 步：影子表检查（虚表的内部存储表，名形如 <vtab>_<suffix>，不可直接动）。
    {
        QString shadowErr;
        info.isShadow = isShadowTable(db, table, &shadowErr);
        if (!shadowErr.isEmpty()) {
            if (err)
                *err = shadowErr;
            return info;
        }
        if (info.isShadow)
            return info;  // 是影子表 → 不合格
    }

    // 3. PRAGMA table_info — collect PK columns
    // 【译】第 3 步：PRAGMA table_info 列出每列信息，收集主键列并判定其 NOT NULL 性。
    {
        QSqlQuery q(db);
        // M-03 fix: double-quote identifier to handle names with spaces/reserved words.
        // 【译】M-03 修复：用双引号包裹表名，以正确处理含空格/保留字的表名；表名内的双引号
        //   按 SQL 规则转义为两个双引号（"" ）。下同 hasUpsertTarget。
        const QString quotedTbl = QStringLiteral("\"") +
                                  QString(table).replace(QLatin1Char('"'), QLatin1String("\"\"")) +
                                  QStringLiteral("\"");
        q.exec(QStringLiteral("PRAGMA table_info(") + quotedTbl + QLatin1Char(')'));
        struct ColInfo {
            int pkSeq;
            bool notNull;
            QString type;
        };
        QMap<int, ColInfo> pkMap;  // pk_seq -> ColInfo（按 PK 内序号排序，用 QMap 自动有序）
        while (q.next()) {
            // PRAGMA table_info 返回列：0=cid 1=name 2=type 3=notnull 4=dflt 5=pk。
            int pkSeq =
                q.value(5).toInt();  // pk column: >0 means part of PK
                                     // 【译】第 5 列 pk：>0 表示该列属于主键（值即 PK 内序号）
            bool nn = q.value(3).toBool();  // notnull  【译】第 3 列 notnull
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
        // 【译】I-17 修复：正确判定每个 PK 列的 NOT NULL 性。SQLite 的「单列 INTEGER PRIMARY KEY」
        //   是 rowid 别名，即便 PRAGMA 报 notnull=0 也【隐式非空】，必须特判放行；其余所有 PK 列
        //   则必须显式 notnull=1 才合格。任一非 rowid 的 PK 列可空 → pkNotNull 置 false 并跳出。
        info.pkNotNull = true;
        for (auto it = pkMap.begin(); it != pkMap.end(); ++it) {
            const ColInfo& ci = it.value();
            // 单列 + 类型为 INTEGER + 是首个 PK 列 → 判定为 rowid 别名（隐式非空）。
            bool isIntegerRowid = (pkMap.size() == 1 &&
                                   ci.type.toUpper() == QLatin1String("INTEGER") && ci.pkSeq == 1);
            if (!isIntegerRowid && !ci.notNull) {
                info.pkNotNull = false;  // 非 rowid 的 PK 列竟可空 → 不满足非空要求
                break;
            }
        }
    }

    return info;
}

// ── hasUpsertTarget —— 确认该表存在「可用作 ON CONFLICT 目标」的约束 ───────────────
// 做什么：再次用 PRAGMA table_info 收集 PK 列，确认表确有主键、且所有 PK 列都满足
//   NOT NULL（单列 INTEGER PK 按 rowid 别名特判为隐式非空）。只要 PK 本身合法，
//   它就是一个有效的 ON CONFLICT 目标，函数即返回 true。
// 为什么 PK 即足够：UPSERT 的 ON CONFLICT 子句需要引用一个非 partial 的 UNIQUE 约束或
//   PRIMARY KEY；主键天然就是这样的目标，故有合法 PK 就够，无需再去找别的 UNIQUE 索引。
// 与 verify 中其它检查的关系：verify 已先确保「有显式 PK、非复合、PK 非空」；本函数是对
//   「PK 能否充当 UPSERT 冲突目标」的最后确认（含对可空 PK 的兜底拒绝）。
// 参数：db；table；err（可空）查询失败或 PK 不合格时写入说明。
// 返回：true=存在可用冲突目标；false=查询失败/无 PK/PK 列可空（err 已填）。
bool SchemaEligibility::hasUpsertTarget(QSqlDatabase& db, const QString& table, QString* err) {
    // PK itself is always a valid ON CONFLICT target — if we got here the table has a PK.
    // Additionally verify no partial unique index is the *only* unique constraint
    // (PK alone is sufficient, so just return true if PK exists).
    // M-02 fix: verify all PK columns are NOT NULL so ON CONFLICT(pk) is a valid target.
    // M-03 fix: double-quote identifier.
    // 【译】主键本身永远是合法的 ON CONFLICT 目标——能走到这里说明表已有 PK。无需再纠结
    //   「是否存在唯一索引」：PK 单独就够（有 PK 即可返回 true）。M-02 修复：须确认所有 PK 列
    //   NOT NULL，ON CONFLICT(pk) 才有效。M-03 修复：表名加双引号转义（同 introspect）。
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
    // 【译】H-03 修复：SQLite 的 INTEGER PRIMARY KEY（rowid 别名）在 PRAGMA table_info 里
    //   notnull 恒为 0，但实际隐式非空。故把「单列 INTEGER PK」一律当作非空，避免误拒最常见的
    //   建表模式（id INTEGER PRIMARY KEY）。
    struct PkCol {
        int seq;
        bool notNull;
        QString type;
    };
    QList<PkCol> pkCols;
    while (ti.next()) {
        const int pkPos = ti.value(5).toInt();  // 第 5 列 pk：>0 即属主键
        if (pkPos > 0)
            pkCols.append({pkPos, ti.value(3).toInt() == 1, ti.value(2).toString().toUpper()});
    }
    if (pkCols.isEmpty()) {
        if (err)
            *err = QStringLiteral("table '%1' has no primary key columns").arg(table);
        return false;  // 无主键 → 无冲突目标
    }
    for (const PkCol& c : pkCols) {
        // Single-column INTEGER PRIMARY KEY is a rowid alias → implicitly NOT NULL.
        // 【译】单列 INTEGER PRIMARY KEY = rowid 别名 → 隐式非空，特判放行。
        const bool isIntegerRowid = (pkCols.size() == 1 && c.type == QLatin1String("INTEGER"));
        if (!isIntegerRowid && !c.notNull) {
            // 非 rowid 的 PK 列可空 → ON CONFLICT(pk) 不可靠，判不合格。
            if (err)
                *err =
                    QStringLiteral("table '%1': PK column is nullable; ON CONFLICT target invalid")
                        .arg(table);
            return false;
        }
    }
    return true;
}

// ── isShadowTable —— 判断某表是否为虚表的「影子表」（虚表内部存储，不可直接同步）─────
// 做什么：影子表名形如 <vtab>_<suffix>（如 FTS 表 docs 会衍生 docs_content/docs_data 等）。
//   本函数取 table 第一个下划线之前的前缀，再查 sqlite_master 里是否存在「名字恰为该前缀、
//   且建表语句以 CREATE VIRTUAL TABLE 打头」的虚表；存在则判定 table 是它的影子表。
// 为什么这样近似判断：SQLite 未直接暴露「影子表归属」元数据，用「虚表名是该表名前缀」作
//   启发式识别，足以挡住 FTS/R*Tree 等常见虚表的内部表。
// 边界：表名不含下划线、或下划线在首位（us<=0）→ 不可能是 <vtab>_<suffix> 形式，直接 false。
// 参数：db；table；err（可空）查询失败时写入。返回：true=是影子表；false=不是/查询失败。
bool SchemaEligibility::isShadowTable(QSqlDatabase& db, const QString& table, QString* err) {
    // Shadow tables have names of the form <vtab>_<suffix>.
    // We check: does any virtual table in sqlite_master have a name that is a prefix of `table`?
    // 【译】影子表名形如 <vtab>_<suffix>；检查：sqlite_master 里是否有某个虚表的名字恰好是
    //   `table` 第一个下划线前的前缀。
    const int us = table.indexOf(QLatin1Char('_'));
    if (us <= 0)
        return false;  // 无下划线或下划线在首位 → 不符合影子表命名形式

    const QString prefix = table.left(us);  // 取下划线前的前缀作为「候选虚表名」
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
    // COUNT>0：确有同名虚表 → table 是它的影子表。
    if (q.next() && q.value(0).toInt() > 0)
        return true;
    return false;
}

}  // namespace dbridge::sync
