#include "SqlBuilder.h"

#include <QSet>

// ============================================================================
// SqlBuilder.cpp — SqlBuilder 的实现：把结构化映射拼成安全 SQL 文本
// ============================================================================
//
// 【本文件实现什么】SqlBuilder.h 声明的三个函数：
//   · quoteIdent          —— 标识符引用/转义（其它两个函数处处依赖它，是安全基石）。
//   · buildUpsert         —— INSERT ... ON CONFLICT(...) DO UPDATE/NOTHING（写库）。
//   · buildAutoJoinSelect —— 自动 LEFT JOIN 的导出 SELECT（多表拉平成一张宽表）。
//
// 【贯穿全文件的两条安全/正确性铁律】
//   ① 标识符（表名、列名、别名）一律走 quoteIdent —— 双引号包裹 + 内部双引号翻倍。
//      作用：防 SQL 注入（恶意名字无法越权改变语句结构）、防与 SQLite 关键字冲突。
//   ② 数据值绝不拼进 SQL 文本 —— 一律用占位符 `?`，真实值由调用方在执行时绑定。
//      作用：值即便含引号 / 分号也不可能被解释成 SQL，是防注入的另一半。
//      buildUpsert 因此额外产出 bindOrder，告诉调用方「第 i 个 ? 绑哪一列」。
//
// 【被谁使用】ETL 的 service/、mapping/ 子系统（导入用 buildUpsert、导出用
//   buildAutoJoinSelect），以及同步子系统对 quoteIdent 的复用。详见头文件。
// ============================================================================

namespace dbridge::detail {

// ── quoteIdent —— 标识符引用：本文件一切安全性的基石 ─────────────────────────
//
// H-05 fix: double-quote identifier, escaping any embedded double-quotes per SQL standard.
// 【翻译保留原注释】H-05 修复：按 SQL 标准给标识符加双引号，并转义其中内嵌的双引号。
//
// 【做什么】把入参 name 变成形如  "name"  的「带引号标识符」（quoted identifier）：
//   外层套一对双引号，并把 name 内部出现的每一个双引号 `"` 替换成两个 `""`。
// 【为什么这样就能防注入与关键字冲突】
//   · SQL 标准里，双引号包裹的内容被解析器当作「一个标识符」整体看待，而非语法的一
//     部分。于是名字里即便藏有 `;`、空格、`--`、`)` 等，也只是这个标识符的普通字符，
//     无法越权闭合语句或注入新子句 —— 这就是「标识符引用防注入」的原理。
//   · 名字本身就是双引号是唯一能「逃出」这层包裹的字符，故必须把它翻倍（`"` → `""`），
//     让解析器把 `""` 读成「一个属于标识符的字面双引号」，而非「引用结束」。这正是
//     SQL 标准规定的双引号转义法（与 C 字符串用 \\ 转义是两回事）。
//   · 像 order / select / index 这类 SQLite 关键字，一旦被双引号包裹即可合法充当
//     表名 / 列名，不会被误解析为关键字 —— 这就是「防关键字冲突」。
// 【参数】name：原始标识符（来自 profile 配置或数据库目录的表名 / 列名）。
// 【返回】两端加引号、内部双引号已转义的新字符串（永远非空，至少是一对引号 `""`）。
// 【副作用】无——纯函数；用 QString(name) 拷贝一份再 replace，不改动传入的 name。
// 【复杂度】O(name 长度)：一次拷贝 + 一次线性扫描替换。
// 【实现细节】QLatin1Char('"') 是单个引号字符；replace 的第二参 QLatin1String("\"\"")
//   是「两个双引号」——源码里的 \" 只是 C++ 字符串里写双引号的写法，实际内容是 ""。
QString SqlBuilder::quoteIdent(const QString& name) {
    return QLatin1Char('"') + QString(name).replace(QLatin1Char('"'), QLatin1String("\"\"")) +
           QLatin1Char('"');
}

// ── buildUpsert —— 构建 SQLite UPSERT：INSERT ... ON CONFLICT DO UPDATE/NOTHING ─
//
// 【做什么】把「写一张表所需材料」（RoutePayload）翻译成一条 UPSERT 语句：
//     INSERT INTO "表" ("列1","列2",...) VALUES (?,?,...)
//     ON CONFLICT("冲突键列",...) DO UPDATE SET "非键列" = excluded."非键列", ...
//   即「按冲突键判定：库里没有这行就 INSERT；已存在就改成 UPDATE（用本次要插入的值
//   覆盖那些非冲突键列）」——这正是 SQLite 的 UPSERT 语义。
// 【为什么用 ON CONFLICT ... excluded】
//   · ON CONFLICT(列...) 指定「以哪几列上的唯一约束作为判重依据」——必须与目标表的
//     PRIMARY KEY 或某个 UNIQUE 约束精确对应（冲突键来自 Profile 的 ConflictSpec）。
//   · excluded 是 SQLite 在 UPSERT 里提供的特殊表名，代表「本次本想 INSERT 的那一行」。
//     于是 `"列" = excluded."列"` 的含义是「把该列更新为本次试插入的新值」。
//   · 冲突键列本身不写进 SET：它们是判重依据、值已相等，重复赋值无意义（且部分场景
//     不允许更新键列），故下面用 conflictSet 把它们从 updateParts 中排除。
// 【为什么值用 ? 而不直接拼】见文件头铁律②：值走占位符，调用方按 bindOrder 绑定，
//   从根本上杜绝「值里含引号 / 分号」造成的 SQL 注入。
// 【参数】payload：含目标表名 table、列名集合 dbColumns、冲突键 conflictKey（值字段
//   binds/conflictVals 本函数不读，只用列名拼 SQL 与绑定顺序）。
// 【返回】UpsertSql{ sql, bindOrder }；当 dbColumns 为空时返回「空壳」（sql 为空串、
//   bindOrder 为空）——调用方应据此判断「无可写列」而跳过执行。
// 【副作用】无——纯函数，只构造并返回字符串结果。
// 【复杂度】O(列数)：列上一次遍历 + 冲突键一次遍历 + 若干 join 拼接。
UpsertSql SqlBuilder::buildUpsert(const RoutePayload& payload) {
    UpsertSql result;

    // 防御：没有任何待写列就没有可执行的 INSERT。返回空壳（result.sql 仍为空字符串），
    // 让调用方识别为「无操作」而非生成一条非法 SQL。
    if (payload.dbColumns.isEmpty()) {
        return result;
    }

    // 把冲突键列放进 QSet，便于下面 O(1) 判断「某列是不是冲突键」——决定它要不要进
    // DO UPDATE SET（冲突键列不参与更新，见上方说明）。
    QSet<QString> conflictSet = QSet<QString>::fromList(payload.conflictKey);

    // H-05 fix: quote all identifiers so reserved words and special chars are safe.
    // 【翻译保留原注释】H-05 修复：所有标识符都加引号，使关键字与特殊字符也安全。
    QStringList allCols;  // 收集已引用的列名 → 用于 INSERT 的 (列,列,...) 子句
    QStringList placeholders;  // 收集与列数等量的 `?` → 用于 VALUES (?,?,...)
    QStringList updateParts;   // 收集 `"列" = excluded."列"` → 用于 DO UPDATE SET

    // 遍历每一个待写列，一趟同时填好「列名 / 占位符 / 绑定顺序 / 可选的更新项」。
    for (const auto& col : payload.dbColumns) {
        allCols.append(quoteIdent(col));  // 列名引用后入 INSERT 列清单（铁律①）
        placeholders.append(QStringLiteral("?"));  // 每列对应一个匿名占位符（铁律②）
        // 关键：bindOrder 与这里 append 的 `?` 严格同序——记录「第 i 个 ? 该绑 col」，
        // 调用方据此从 payload.binds 取值绑定，保证不串位。
        result.bindOrder.append(col);

        // 仅「非冲突键列」才进 DO UPDATE SET；冲突键列是判重依据，不更新（见上）。
        if (!conflictSet.contains(col)) {
            // 拼出  "col" = excluded."col" ：把该列更新为本次试插入的新值。
            updateParts.append(quoteIdent(col) + QStringLiteral(" = excluded.") + quoteIdent(col));
        }
    }

    // 冲突键列也要逐个引用，供 ON CONFLICT(...) 子句使用。
    QStringList quotedConflict;
    for (const auto& ck : payload.conflictKey)
        quotedConflict.append(quoteIdent(ck));

    // 拼装语句主体：表名与所有列名均已引用，值位置全是 `?`。
    //   INSERT INTO "table" ("c1", "c2", ...) VALUES (?, ?, ...) ON CONFLICT("k1", ...)<空格>
    // 末尾留一个空格，方便下面无论接 DO NOTHING 还是 DO UPDATE 都不粘连。
    QString sql = QStringLiteral("INSERT INTO ") + quoteIdent(payload.table) +
                  QStringLiteral(" (") + allCols.join(QStringLiteral(", ")) +
                  QStringLiteral(") VALUES (") + placeholders.join(QStringLiteral(", ")) +
                  QStringLiteral(") ON CONFLICT(") + quotedConflict.join(QStringLiteral(", ")) +
                  QStringLiteral(") ");

    // 冲突时的动作分两种：
    if (updateParts.isEmpty()) {
        // 所有列都是冲突键（没有可更新的非键列）→ 冲突时无事可做，用 DO NOTHING：
        // 既不报错也不改动，相当于「存在即跳过」的幂等插入。
        sql += QStringLiteral("DO NOTHING");
    } else {
        // 有非键列 → 冲突时用本次新值覆盖这些列（逗号连接所有 "列"=excluded."列"）。
        sql += QStringLiteral("DO UPDATE SET ") + updateParts.join(QStringLiteral(", "));
    }

    result.sql = sql;
    return result;
}

// ── buildAutoJoinSelect —— 导出方向：把多张关联表自动 JOIN 成一张可导出的宽表 ──
//
// 【做什么】根据若干条路由（每条 = 一张表 + 它的列映射 + 外键注入关系）和导出规格，
//   拼出一条形如：
//     SELECT "根表"."列" AS "Excel表头", "子表"."列" AS "Excel表头", ...
//     FROM "根表"
//     LEFT JOIN "子表" ON "子表"."外键列" = "父表"."主键列" [AND ...]
//     [ORDER BY ...]
//   的 SELECT。把分散在多张库表里的列「拉平」为一行，列别名用 Excel 表头名，便于
//   导出引擎直接按别名取列写进 Excel。
// 【为什么用 LEFT JOIN（而非 INNER）】子表若没有匹配行，根表的行仍应出现在结果里
//   （对应的子表列为 NULL），不能因缺子记录而丢整行——导出要尽量完整还原数据。
// 【为什么 routes[0] 是 FROM 的根】routes 已由上游（拓扑排序）保证 [0] 为根路由
//   （无父表），其余子路由再依据各自 fkInject 挂到已在范围内的父表上。
// 【参数】routes：路由集合（[0] 根，其余子表）；exportSpec：导出配置（这里只用 orderBy）。
// 【返回】完整 SELECT 文本；routes 为空时返回空 QString（无表可查）。
// 【副作用】无——纯函数。
// 【复杂度】O(总列数 + 总外键对数 + orderBy 项数)：构建 SELECT 列、JOIN 条件、排序项各一遍。
QString SqlBuilder::buildAutoJoinSelect(const QVector<RouteSpec>& routes,
                                        const ExportSpec& exportSpec) {
    // 无路由 → 无 FROM 目标，返回空串让调用方识别为「无可导出」。
    if (routes.isEmpty())
        return QString();

    QStringList selectCols;   // SELECT 后的「表.列 AS 别名」清单
    QString fromClause;       // FROM 后的根表（已引用）
    QStringList joinClauses;  // 各子表的 LEFT JOIN ... ON ... 子句

    // 根表 = 第一条路由；FROM 直接挂它（标识符引用，铁律①）。
    const RouteSpec& root = routes[0];
    fromClause = quoteIdent(root.table);

    // 遍历每一条路由：① 把它的列加入 SELECT；② 若是子表，按外键关系生成 JOIN。
    for (const auto& route : routes) {
        for (const auto& col : route.columns) {
            // H-05: quote table and column names; use column source (Excel header) as alias
            // 【翻译保留原注释】H-05：表名与列名都加引号；用列的 source（Excel 表头）作别名。
            // 拼出  "route.table"."col.dbColumn" AS "col.source" ：
            //   · 用「表.列」全限定，避免多表 JOIN 后同名列产生「列名歧义」错误；
            //   · 别名取 Excel 表头名，导出引擎按表头取值，无需关心底层库列名。
            selectCols.append(quoteIdent(route.table) + QStringLiteral(".") +
                              quoteIdent(col.dbColumn) + QStringLiteral(" AS ") +
                              quoteIdent(col.source));
        }

        // 仅「非根 且 有外键注入关系」的路由才需要 JOIN：根表已在 FROM 里，
        // 无 fkInject 的路由不知道怎样与已有表关联，跳过（不挂 JOIN）。
        // 【注意】&route != &routes[0] 是按「地址」比对，判断当前迭代项是否就是根那一条。
        if (&route != &routes[0] && !route.fkInject.isEmpty()) {
            // H-06 fix: use quoteIdent() for all identifiers in JOIN ON clauses.
            // M-06 fix: iterate ALL fkInject groups (not just [0]).
            // Multiple FK inject groups on the same child route share ONE JOIN with all ON
            // conditions AND-ed together (all FK conditions must hold simultaneously).
            // Using one combined JOIN avoids duplicate LEFT JOIN on the same table, which would
            // require aliases that break the SELECT clause's unaliased table.column references.
            // 【翻译保留原注释】
            //   H-06 修复：JOIN ON 子句里的所有标识符都用 quoteIdent()。
            //   M-06 修复：遍历「全部」fkInject 组（不只是 [0]）。
            //   同一条子路由上的多个外键注入组共享「一个」JOIN，其各 ON 条件用 AND 串起来
            //   （所有外键条件必须同时成立）。
            //   用单个合并的 JOIN，可避免对同一张表重复 LEFT JOIN——重复 JOIN 会被迫起别名，
            //   而别名会破坏 SELECT 子句里「未起别名的 表.列」那种引用方式。
            QStringList allOnParts;  // 收集本子表所有 ON 等值条件，最后用 AND 连接
            for (const FkInjectSpec& fk : route.fkInject) {
                // 每个 fkInject 组指定一个父表 fk.fromTable，以及若干 (父列, 子列) 对。
                // pairs 里 pair.first=父列、pair.second=子列（见 ProfileSpec.h FkInjectSpec）。
                for (const auto& pair : fk.pairs) {
                    // 拼一条等值条件：  "子表"."子列" = "父表"."父列"  （全部引用）。
                    allOnParts.append(quoteIdent(route.table) + QStringLiteral(".") +
                                      quoteIdent(pair.second) + QStringLiteral(" = ") +
                                      quoteIdent(fk.fromTable) + QStringLiteral(".") +
                                      quoteIdent(pair.first));
                }
            }
            if (!allOnParts.isEmpty()) {
                // 把本子表的全部 ON 条件 AND 在一起，生成一条 LEFT JOIN（见上「单 JOIN」说明）。
                joinClauses.append(QStringLiteral("LEFT JOIN ") + quoteIdent(route.table) +
                                   QStringLiteral(" ON ") +
                                   allOnParts.join(QStringLiteral(" AND ")));
            }
        }
    }

    // 组装语句骨架：SELECT <列清单> FROM <根表>。
    QString sql = QStringLiteral("SELECT ") + selectCols.join(QStringLiteral(", ")) +
                  QStringLiteral(" FROM ") + fromClause;

    // 依次追加各子表的 LEFT JOIN（前置一个空格避免与前文粘连）。
    for (const auto& j : joinClauses) {
        sql += QStringLiteral(" ") + j;
    }

    // 可选的 ORDER BY：把 exportSpec.orderBy 里的每一项变成安全、无歧义的排序键。
    if (!exportSpec.orderBy.isEmpty()) {
        // H-06 fix: qualify unqualified ORDER BY columns with their owning table name to avoid
        // "ambiguous column name" errors when the same column name appears in multiple joined
        // tables (e.g. both orders.order_no and order_items.order_no are in scope after a JOIN).
        // 【翻译保留原注释】H-06 修复：给未限定（没带表名）的 ORDER BY 列补上其所属表名，
        //   以避免「列名歧义」错误——当同一个列名在多张被 JOIN 的表里都存在时
        //   （例如 JOIN 后 orders.order_no 和 order_items.order_no 都在作用域内）。
        //
        // 先建「裸列名 → 所属表名」索引：遍历所有路由的列，记录每个 dbColumn 第一次
        // 出现时所属的表。用「第一次出现者优先」（contains 判空再 insert），保证同名列
        // 有一个确定的归属表，使补限定的结果可预测。
        QHash<QString, QString> dbColToTable;
        for (const auto& route : routes) {
            for (const auto& col : route.columns) {
                if (!dbColToTable.contains(col.dbColumn))
                    dbColToTable.insert(col.dbColumn, route.table);
            }
        }

        // 逐个处理 orderBy 项，分三种情形决定如何引用 / 限定。
        QStringList orderParts;
        for (const auto& ob : exportSpec.orderBy) {
            // 用第一个 '.' 把「table.col」切两段；dot>0 才算调用方已写了「表.列」形式。
            const int dot = ob.indexOf(QLatin1Char('.'));
            if (dot > 0) {
                // Already qualified by the caller — just quote each part.
                // 【翻译保留原注释】调用方已自行限定 → 只需把两段各自加引号。
                // 切成左右两段分别引用，得到  "table"."col"  ——不能整串引用，否则
                // 会把含点的整体当成一个标识符名。
                orderParts.append(quoteIdent(ob.left(dot)) + QLatin1Char('.') +
                                  quoteIdent(ob.mid(dot + 1)));
            } else if (dbColToTable.contains(ob)) {
                // Qualify with the owning table so the identifier is unambiguous across joins.
                // 【翻译保留原注释】用所属表限定，使该标识符在多表 JOIN 中无歧义。
                // 裸列名且能在索引里查到归属表 → 补成  "归属表"."列"  消除歧义。
                orderParts.append(quoteIdent(dbColToTable[ob]) + QLatin1Char('.') + quoteIdent(ob));
            } else {
                // 裸列名且不在任何路由列里（如合成列 / 表达式别名）→ 无从限定，原样引用。
                orderParts.append(quoteIdent(ob));
            }
        }
        // 用逗号连接所有排序键，追加 ORDER BY 子句。
        sql += QStringLiteral(" ORDER BY ") + orderParts.join(QStringLiteral(", "));
    }

    return sql;
}

}  // namespace dbridge::detail
