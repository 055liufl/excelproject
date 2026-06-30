#include "SchemaIntrospector.h"

#include <QSqlError>  // QSqlError：执行失败时取 .text() 错误文本
#include <QSqlQuery>  // QSqlQuery：执行 SQL / PRAGMA 并按列下标取结果
#include <QVariant>  // QVariant：QSqlQuery::value() 的返回类型，再 .toXxx() 转具体类型

// ============================================================================
// SchemaIntrospector.cpp — schema 自省读取器的实现
// ============================================================================
//
// 【本文件做什么】
//   实现 SchemaIntrospector.h 声明的各方法：用 SQLite 的自省机制
//   （sqlite_master 系统表 + 各种 PRAGMA）把库结构读出来，填进 SchemaCatalog。
//
// 【贯穿全文件的两个通用约定，先在此交代，后文不再重复】
//   1) QSqlQuery 取列：q.value(i) 按「结果集的列下标（0 基）」取值，返回 QVariant，
//      再 .toString()/.toInt()/.toBool() 转目标类型。各 PRAGMA 的列顺序是 SQLite
//      固定约定的，下文每处都用注释标出「下标对应哪一列」，这是读懂解析的关键。
//   2) 标识符引用（M-01 fix 反复出现的套路）：把表名/索引名拼进 PRAGMA 语句时，
//      用「双引号包裹 + 内部双引号转义为两个双引号」的方式，即 SQL 标准的标识符
//      引用。这样含空格、保留字、甚至双引号的表名也能安全拼接，且这是「标识符」
//      语义（指代对象名），区别于用单引号的「字符串字面量」语义。
//
// 【为什么不用参数绑定（prepare + bind）来传表名】
//   PRAGMA 的参数位置不接受占位符绑定（它要的是「标识符」而非「值」），故只能
//   字符串拼接；正因为是拼接，才必须如上做标识符转义来防注入/防语法错误。
// ============================================================================

namespace dbridge::detail {

// ── readTables —— 列出所有「用户表」的表名 ──────────────────────────────────
//
//   原理：sqlite_master 是 SQLite 的系统目录表，每个数据库对象（表/索引/视图/
//   触发器）在其中占一行，type 列区分种类、name 列是对象名。这里只取 type='table'。
//
//   过滤掉两类内部表：
//     · name LIKE 'sqlite_%' —— SQLite 自身的内部表（如 sqlite_sequence 等）；
//     · name LIKE '__sync_%' —— 本项目同步子系统的元数据表（见下方 L-04 fix）。
//
//   出参 tables：把命中的表名逐行 append 进去。返回 false 表示查询失败（err 已填）。
bool SchemaIntrospector::readTables(QSqlDatabase& db, QStringList* tables, QString* err) {
    QSqlQuery q(db);
    // L-04 fix：排除 __sync_* 元数据表，使 schema 目录只含用户表。
    //   Profile/导出逻辑绝不应绑定到同步子系统的内部状态表上。
    // L-04 fix: exclude __sync_* meta-tables so the schema catalog only contains user tables.
    // Profile/export logic should never bind to internal sync state tables.
    if (!q.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' "
                               "AND name NOT LIKE 'sqlite_%' AND name NOT LIKE '__sync_%'"))) {
        // exec 返回 false 表示 SQL 执行失败：把底层错误文本透出给上层，再返回失败。
        if (err)
            *err = q.lastError().text();
        return false;
    }
    // q.next() 逐行前移游标；value(0) 即 SELECT 的第 1 列（name）。
    while (q.next()) {
        tables->append(q.value(0).toString());
    }
    return true;
}

// ── readColumns —— 读取一张表的所有列（含生成列），并判定自增主键 ───────────
//
//   核心 PRAGMA：table_xinfo(T)。它是 table_info 的「扩展版」，相比后者多了一列
//   hidden，能告诉我们哪些是隐藏列/生成列。其结果集每行描述一列，列序固定为：
//     下标0 cid        —— 列号（0 基，即建表时的列序）
//     下标1 name       —— 列名
//     下标2 type       —— 声明类型文本（如 "INTEGER"、"VARCHAR(32)"，可能为空）
//     下标3 notnull    —— 是否 NOT NULL（0/1）
//     下标4 dflt_value —— 默认值表达式文本（无则 NULL）
//     下标5 pk         —— 主键序号：0=非主键；>0=复合主键中的第几列（1 基）
//     下标6 hidden     —— 0=普通列；1=隐藏列；2=VIRTUAL 生成列；3=STORED 生成列
bool SchemaIntrospector::readColumns(QSqlDatabase& db, TableInfo* info, QString* err) {
    QSqlQuery q(db);
    // table_xinfo 会给出隐藏列信息（生成列的 hidden > 0）。
    // table_xinfo gives hidden column info (generated columns have hidden > 0).
    // M-01 fix：用「双引号标识符引用」而非「单引号字符串字面量」，这样名字中含
    //   双引号、空格或 SQL 保留字的表也能被正确处理。
    // M-01 fix: use double-quote identifier quoting instead of single-quote string literal so
    // table names with embedded double-quotes, spaces, or reserved words are handled correctly.
    //   下面这段拼接 = 在表名两侧各加一个双引号，并把表名内部出现的每个 " 替换成 ""
    //   （SQL 标准的标识符内双引号转义），最终得到形如  "weird""name"  的安全标识符。
    const QString quotedName =
        QStringLiteral("\"") +
        QString(info->name).replace(QLatin1Char('"'), QLatin1String("\"\"")) + QStringLiteral("\"");
    QString sql = QStringLiteral("PRAGMA table_xinfo(") + quotedName + QLatin1Char(')');
    if (!q.exec(sql)) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    while (q.next()) {
        // 结果列序（table_xinfo）：cid, name, type, notnull, dflt_value, pk, hidden
        // cid, name, type, notnull, dflt_value, pk, hidden
        ColumnInfo col;
        col.name = q.value(1).toString();  // 列名 ← name
        col.declaredType = q.value(2).toString();  // 声明类型原文 ← type（亲和性由消费方按需推导）
        col.notNull = q.value(3).toBool();         // NOT NULL ← notnull（0/1）
        col.defaultValue = q.value(4).toString();  // 默认值表达式 ← dflt_value
        int pk = q.value(5).toInt();               // 主键序号 ← pk
        if (pk > 0) {
            // pk>0 说明此列属于主键；pk 的具体数值就是它在（可能的）复合主键里的位次。
            col.primaryKey = true;
            col.pkOrder = pk;  // 例如复合主键 (a,b)：a 的 pk=1，b 的 pk=2；单列主键则 pk=1。
        }
        int hidden = q.value(6).toInt();  // 隐藏/生成标志 ← hidden
        // hidden==2(VIRTUAL) 或 ==3(STORED) 即「生成列」——其值由表达式算出、不可写入，
        //   ETL 写库时须跳过。（hidden==1 是其它隐藏列，这里不视作 generated。）
        col.generated = (hidden == 2 || hidden == 3);  // VIRTUAL or STORED generated

        info->columns.append(col);
    }

    // 逐列判定 AUTOINCREMENT（自增）。
    // Check autoIncrement per column
    //   为什么要这样判：PRAGMA 并不直接给出 AUTOINCREMENT 标志。而 SQLite 的
    //   AUTOINCREMENT 只对「单列 INTEGER PRIMARY KEY」生效，所以这里只在满足
    //   该前提的列上，才去扫描建表 SQL 原文确认是否真的写了 AUTOINCREMENT 关键字
    //   （见 isAutoIncrement）。其余列一律保持默认 false，省去无谓查询。
    for (auto& col : info->columns) {
        if (col.primaryKey && col.pkOrder == 1) {  // 主键、且是主键第 1 列（排除复合主键的后续列）
            if (col.declaredType.toUpper() ==
                QStringLiteral("INTEGER")) {  // 声明类型严格为 INTEGER
                col.autoIncrement = isAutoIncrement(db, info->name, col.name);
            }
        }
    }
    return true;
}

// ── isAutoIncrement —— 判断该表是否使用了 AUTOINCREMENT ─────────────────────
//
//   做什么：取出该表的建表 SQL 原文（sqlite_master.sql 列存的就是 CREATE TABLE 语句），
//           大写化后看是否包含 "AUTOINCREMENT" 关键字。
//   为什么这么做：SQLite 没有任何 PRAGMA 直接暴露「这列是不是 AUTOINCREMENT」，
//           而该关键字只会出现在建表语句文本里，故只能回查原文做关键字匹配。
//   参数 colName：当前实现并不需要（AUTOINCREMENT 在 SQLite 中至多作用于整张表的
//           那唯一一个 INTEGER PRIMARY KEY），故用 Q_UNUSED 标注「有意未用」，
//           保留形参是为接口对称、便于将来若需精确到列再扩展。
//   返回：包含关键字为 true；查询失败/无此表/无结果一律返回 false（不写 err，保守降级）。
//   注意（已知近似）：这是「整表级」的粗判——只要建表语句任意处含该关键字即判 true，
//           理论上若关键字以非约束形式出现在文本别处会误判，但实务中 CREATE TABLE
//           里出现该词即意味着自增，足够可靠。
bool SchemaIntrospector::isAutoIncrement(QSqlDatabase& db, const QString& tableName,
                                         const QString& colName) {
    Q_UNUSED(colName)
    // SQLite 的 AUTOINCREMENT 体现为 CREATE TABLE 语句里出现 AUTOINCREMENT 关键字。
    // SQLite AUTOINCREMENT is indicated by presence of AUTOINCREMENT keyword in CREATE TABLE
    QSqlQuery q(db);
    // 这里查的是「值」（表名），可安全用占位符 ? 绑定——与前面拼标识符的场景不同。
    q.prepare(QStringLiteral("SELECT sql FROM sqlite_master WHERE type='table' AND name=?"));
    q.addBindValue(tableName);
    if (!q.exec() || !q.next())  // 执行失败或查无此表：保守地认为「非自增」。
        return false;
    QString sql = q.value(0).toString().toUpper();  // 取建表 SQL 原文并转大写，便于大小写无关匹配。
    return sql.contains(QStringLiteral("AUTOINCREMENT"));
}

// ── readIndexes —— 读取一张表的全部索引（两级 PRAGMA）──────────────────────
//
//   分两步：
//     第 1 步 PRAGMA index_list(T) —— 列出该表的所有索引「清单」，每行一个索引，
//             给出名字、是否唯一、是否部分索引等元信息；但不含「索引覆盖哪些列」。
//     第 2 步 PRAGMA index_info(I) —— 针对每个索引 I，再查它「覆盖的列」明细。
//   之所以两级，是因为 SQLite 把「索引列表」与「索引列明细」分在两个 PRAGMA 里。
//
//   实现上先把第 1 步的结果完整收进本地 idxList，再循环做第 2 步——避免在同一个
//   QSqlQuery 游标遍历未结束时又复用它发新查询。
bool SchemaIntrospector::readIndexes(QSqlDatabase& db, TableInfo* info, QString* err) {
    QSqlQuery q(db);
    // M-01 fix：双引号标识符引用（同 readColumns，处理含特殊字符的表名）。
    // M-01 fix: use double-quote identifier quoting.
    const QString quotedTableName =
        QStringLiteral("\"") +
        QString(info->name).replace(QLatin1Char('"'), QLatin1String("\"\"")) + QStringLiteral("\"");
    QString sql = QStringLiteral("PRAGMA index_list(") + quotedTableName + QLatin1Char(')');
    if (!q.exec(sql)) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    // index_list 结果列序：seq, name, unique, origin, partial
    //   下标0 seq     —— 序号；下标1 name —— 索引名；下标2 unique —— 是否唯一(0/1)；
    //   下标3 origin  —— 来源('c'=显式CREATE INDEX / 'u'=UNIQUE约束 / 'pk'=主键)；
    //   下标4 partial —— 是否部分索引(0/1)。
    // seq, name, unique, origin, partial
    // 临时承载第 1 步元信息的小结构（仅函数内用）。
    struct IdxMeta {
        QString name;
        bool unique;
        bool partial;
    };
    QVector<IdxMeta> idxList;
    while (q.next()) {
        IdxMeta m;
        m.name = q.value(1).toString();   // ← name
        m.unique = q.value(2).toBool();   // ← unique
        m.partial = q.value(4).toBool();  // H-02 fix：第 4 列（下标 4）= partial 标志
        // H-02 fix: column index 4 = partial flag
        idxList.append(m);
    }

    // 第 2 步：对清单中每个索引，查其覆盖列，组装成完整 IndexInfo 加入 info->indexes。
    for (auto& meta : idxList) {
        IndexInfo idx;
        idx.name = meta.name;
        idx.unique = meta.unique;
        idx.partial = meta.partial;  // H-02 fix：把 partial 标志透传到目录里
        // H-02 fix: propagate partial flag

        QSqlQuery qi(db);  // 用「另一个」查询对象，避免干扰外层 q 的游标。
        // M-01 fix：索引名同样做双引号标识符引用。
        // M-01 fix: double-quote index name.
        const QString quotedIdx =
            QStringLiteral("\"") +
            QString(meta.name).replace(QLatin1Char('"'), QLatin1String("\"\"")) +
            QStringLiteral("\"");
        qi.exec(QStringLiteral("PRAGMA index_info(") + quotedIdx + QLatin1Char(')'));
        // index_info 结果列序：seqno, cid, name
        //   下标0 seqno —— 列在索引内的位次；下标1 cid —— 列在表内的列号；
        //   下标2 name  —— 列名（我们要的就是它，按 PRAGMA 返回的顺序即索引列序）。
        // seqno, cid, name
        while (qi.next()) {
            idx.columns.append(qi.value(2).toString());  // ← name
        }
        info->indexes.append(idx);
    }
    return true;
}

// ── readForeignKeys —— 读取一张表的全部外键关系 ─────────────────────────────
//
//   核心 PRAGMA：foreign_key_list(T)。它列出表 T（作为子表）声明的所有外键，
//   每行描述「本表某列 → 父表某列」的一条引用。这是 ETL 外键查找与拓扑排序的命脉：
//     · 外键查找：据 from→(refTable.to) 用业务键查父表代理主键、注入子表；
//     · 拓扑排序：每行即一条「本表依赖 refTable」的有向边。
//
//   注意：复合外键（一个外键约束跨多列）会在结果里以「同一 id、不同 seq」的多行出现；
//   本实现按行平铺成多条 FkInfo（不在此聚合分组），消费方如需可按列对应关系自行组合。
bool SchemaIntrospector::readForeignKeys(QSqlDatabase& db, TableInfo* info, QString* err) {
    QSqlQuery q(db);
    // M-01 fix：双引号标识符引用。
    // M-01 fix: double-quote identifier.
    const QString quotedName2 =
        QStringLiteral("\"") +
        QString(info->name).replace(QLatin1Char('"'), QLatin1String("\"\"")) + QStringLiteral("\"");
    QString sql = QStringLiteral("PRAGMA foreign_key_list(") + quotedName2 + QLatin1Char(')');
    if (!q.exec(sql)) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    // foreign_key_list 结果列序：id, seq, table, from, to, on_update, on_delete, match
    //   下标0 id        —— 外键编号（同一复合外键的多列共享同一 id）
    //   下标1 seq       —— 该列在复合外键中的位次
    //   下标2 table     —— 被引用的父表名（→ refTable）
    //   下标3 from      —— 本表（子表）中的外键列名（→ fromColumn）
    //   下标4 to        —— 父表中被引用的列名（→ toColumn）
    //   下标5 on_update —— 父行更新时的级联动作（本实现未采集）
    //   下标6 on_delete —— 父行删除时的级联动作（本实现未采集）
    //   下标7 match     —— 匹配模式（本实现未采集）
    // id, seq, table, from, to, on_update, on_delete, match
    while (q.next()) {
        FkInfo fk;
        fk.refTable = q.value(2).toString();    // 父表 ← table
        fk.fromColumn = q.value(3).toString();  // 子表外键列 ← from
        fk.toColumn = q.value(4).toString();    // 父表被引用列 ← to
        info->foreignKeys.append(fk);
    }
    return true;
}

// ── load —— 顶层编排：把整库结构自省进 out 目录 ────────────────────────────
//
//   流程：清空目录 → 列出所有用户表 → 对每张表依次读「列 / 索引 / 外键」→ 入目录。
//   错误策略：任一步失败立即「短路」返回 false，错误文本已由子函数写入 err；
//             此时 out 里可能已含先前成功的部分表（调用方拿到 false 时应整体作废重来）。
//   副作用：先 clear 再逐表 addTable，最终 *out 持有全库结构。不修改数据库。
//   复杂度：O(表数 × 每表的列/索引/外键数)，外加每表若干次 PRAGMA 往返。
bool SchemaIntrospector::load(QSqlDatabase& db, SchemaCatalog* out, QString* err) {
    out->clear();  // 重新自省前先清空，杜绝上一次的旧结构残留。

    QStringList tables;
    if (!readTables(db, &tables, err))  // 第一步：拿到全部用户表名。
        return false;

    for (const auto& tname : tables) {
        // 为每张表组装一个 TableInfo，三类自省按「列→索引→外键」顺序填充。
        TableInfo info;
        info.name = tname;
        if (!readColumns(db, &info, err))  // 列（含生成列、自增判定）
            return false;
        if (!readIndexes(db, &info, err))  // 索引（含 unique/partial 与覆盖列）
            return false;
        if (!readForeignKeys(db, &info, err))  // 外键关系
            return false;
        out->addTable(info);  // 该表结构齐备，存入目录。
    }
    return true;
}

}  // namespace dbridge::detail
