#include "ProfileValidator.h"

#include "dbridge/Errors.h"  // E_PROFILE_* / E_HEADER_NOT_FOUND / W_TIME_ORDERBY_NONSORTABLE / E_EXPORT_*

#include <QSet>

#include "service/ErrorCollector.h"  // 错误/警告收集器：addTable / addTableWarning

// ============================================================================
// ProfileValidator.cpp — Profile 语义校验器的实现
// ============================================================================
//
// 【本文件做什么】实现 ProfileValidator（接口与总览见同名 .h）。把一份已解析的
//   ProfileSpec 与「数据库 schema（SchemaCatalog）+ Excel 表头」对账，逐条核验
//   配置自洽、可执行，发现的问题以错误码（dbridge/Errors.h）写入 ErrorCollector。
//
// 【在 ETL 管线中的位置】ProfileLoader（JSON→Spec）→ 本校验器 → Import/Export 引擎。
//   校验是「执行前的体检」，把可静态发现的错误挡在真正读 Excel / 写库之前。
//
// 【贯穿全文件的两条约定】
//   1) 校验风格「尽量收集、不早退」：单条规则失败通常只置 ok=false 并继续，目的是
//      一次把尽可能多的问题报全；只有「硬性前置失败」（如路由目标表根本不存在）才
//      提前 return，因为后续检查已无意义。
//   2) 列名集合比较一律「按集合（去序去重）」：约束匹配只关心「是哪几列」，与书写
//      顺序无关，故大量使用 QSet<QString> 做相等判断。
//
// 【注释里出现的 §3.x】指 Profile 配置规格文档里的条款编号（如 §3.4「禁止 lookup
//   级联」）。原英文注释保留并在中文里对应解释，便于回溯规格出处。
// ============================================================================

namespace dbridge::detail {

// ──────────────────────────────────────────────────────────────────────────
// isConflictValid —— 判断一组冲突列是否为 SQLite 合法的 ON CONFLICT 目标。
// 详细契约见头文件声明处注释。要点：冲突列集合必须「整组等于」PK 列集合，或
// 「整组等于」某个非部分 UNIQUE 索引的列集合，缺一不可。
// ──────────────────────────────────────────────────────────────────────────
bool ProfileValidator::isConflictValid(const ConflictSpec& conflict, const TableInfo& table) {
    // 空冲突键不可能成为有效目标（UPSERT 无从判断「插入还是更新」）。
    if (conflict.columns.isEmpty())
        return false;

    // 把冲突列转成集合：后续所有比较都按「集合相等」（忽略顺序、去重）。
    QSet<QString> conflictSet = QSet<QString>::fromList(conflict.columns);

    // 收集表的全部主键列（复合主键会有多列），转集合。
    QStringList pkList;
    for (const auto& c : table.columns) {
        if (c.primaryKey)
            pkList.append(c.name);
    }
    QSet<QString> pkCols = QSet<QString>::fromList(pkList);
    // 冲突列恰好就是「整个主键」→ 合法目标（注意：表无主键时 pkCols 为空，不命中）。
    if (!pkCols.isEmpty() && pkCols == conflictSet)
        return true;

    // 否则逐个 UNIQUE 索引比对：冲突列集合 == 某个 UNIQUE 索引的列集合 → 合法。
    for (const auto& idx : table.indexes) {
        if (!idx.unique)
            continue;  // 非唯一索引不能作冲突目标，跳过
        // H-02 fix: partial UNIQUE indexes are not valid ON CONFLICT targets in SQLite —
        // the database engine requires the conflict clause to reference a non-partial constraint.
        // 【译】H-02 修复：部分（带 WHERE 条件的）UNIQUE 索引不是合法的 ON CONFLICT 目标——
        // SQLite 要求冲突子句引用的必须是「非部分」的 UNIQUE 约束或主键。故部分索引一律跳过。
        if (idx.partial)
            continue;
        QSet<QString> idxSet = QSet<QString>::fromList(idx.columns);
        if (idxSet == conflictSet)
            return true;
    }

    // 既不匹配主键、也不匹配任何非部分 UNIQUE 索引 → 非法冲突目标。
    return false;
}

// ──────────────────────────────────────────────────────────────────────────
// validateRoute —— 校验「单条路由」的全部自洽性（本类最核心、最长的方法）。
// 总体契约见头文件声明处；本实现内每个分段都标注它对应的 Profile 规格条款（§3.x）。
// 贯穿本函数的关键数据结构：
//   · ok          —— 累积结果；任一规则失败置 false，但循环继续（尽量收集）。
//   · dbColSource —— dbColumn → 它的「来源标签」("excel" / "lookup:<名>" /
//                    "fkInject:<父表>")。这是「三来源唯一性」(§3.11) 的核心：
//                    同一个目标列绝不允许被两个来源同时写入，否则值无法确定。
// ──────────────────────────────────────────────────────────────────────────
bool ProfileValidator::validateRoute(const RouteSpec& route, const QVector<RouteSpec>& allRoutes,
                                     const SchemaCatalog& catalog, const QStringList& excelHeaders,
                                     const QString& sheet, ErrorCollector* errors) {
    bool ok = true;

    // 【硬性前置】路由目标表必须存在于 schema。若连表都没有，后续所有「列存在性」
    // 检查都无从谈起 → 直接 return false（这是为数不多的「提前退出」之一）。
    if (!catalog.hasTable(route.table)) {
        errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_TABLE_NOT_FOUND),
                         QStringLiteral("Table '") + route.table + QStringLiteral("' not found"));
        return false;
    }
    const TableInfo& tableInfo = *catalog.table(route.table);  // 拿到目标表 schema（保证非空）

    // ── 冲突键（conflict）校验：UPSERT 必须能判定「插入还是更新」───────────────
    if (route.conflict.columns.isEmpty()) {
        // 完全没配冲突键 → E_PROFILE_NO_CONFLICT_KEY。
        errors->addTable(
            sheet, QString::fromLatin1(err::E_PROFILE_NO_CONFLICT_KEY),
            QStringLiteral("Route '") + route.table + QStringLiteral("' has no conflict columns"));
        ok = false;
    } else {
        // 第一步：每个冲突列必须真实存在于目标表（拼写/schema 漂移防护）。
        for (const auto& cc : route.conflict.columns) {
            if (!tableInfo.column(cc)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_NO_CONFLICT_KEY),
                                 QStringLiteral("Conflict column '") + cc +
                                     QStringLiteral("' not found in ") + route.table);
                ok = false;
            }
        }
        // 第二步：仅当上面全部列都存在（ok 仍为真）时，才进一步核验这组列「整组匹配」
        // 某个 PK 或非部分 UNIQUE 约束（否则 SQLite 的 ON CONFLICT 会拒绝）。
        // 之所以加 ok 守卫：若有列根本不存在，isConflictValid 必然失败，再报一条
        // 「不匹配」属于噪声——先让用户修好「列不存在」更清晰。
        if (ok && !isConflictValid(route.conflict, tableInfo)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_NO_CONFLICT_KEY),
                             QStringLiteral("Conflict columns in '") + route.table +
                                 QStringLiteral("' do not match any PRIMARY KEY or UNIQUE index"));
            ok = false;
        }
    }

    // §3.8 Build dbColumn source map for three-source uniqueness (§3.2, §3.11)
    // source string: "excel", "lookup:<name>", "fkInject:<from>"
    // 【译】§3.8 建立 dbColumn→来源 的映射，用于「三来源唯一性」校验（§3.2、§3.11）。
    //   一个目标列的值只能来自三处之一：Excel 直接映射、lookup 查出、fkInject 注入。
    //   下面三段（Excel / lookup / fkInject）会各自往 dbColSource 登记，谁先占了某列，
    //   后来者再写同列即报「来源冲突」——保证每列的写入值唯一确定。
    QHash<QString, QString> dbColSource;

    // ── 来源 1：Excel 直接映射列（§3.11 source 1）──────────────────────────────
    for (const auto& col : route.columns) {
        // (a) 目标 dbColumn 必须存在于表中。
        if (!tableInfo.column(col.dbColumn)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                             QStringLiteral("Column '") + col.dbColumn +
                                 QStringLiteral("' not found in table '") + route.table + '\'');
            ok = false;
        }
        // (b) 该列的来源 Excel 表头必须真实出现在 Excel 中（导出向已由合成表头放行）。
        if (!excelHeaders.contains(col.source)) {
            errors->addTable(
                sheet, QString::fromLatin1(err::E_HEADER_NOT_FOUND),
                QStringLiteral("Excel header '") + col.source + QStringLiteral("' not found"));
            ok = false;
        }
        // (c) 同一目标 dbColumn 在本路由的 Excel 列里不可重复映射（否则值二义）。
        //     首次出现就把它登记为来源 "excel"，供后续 lookup/fkInject 做冲突检测。
        if (dbColSource.contains(col.dbColumn)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                             QStringLiteral("route '") + route.table +
                                 QStringLiteral("': duplicate dbColumn '") + col.dbColumn + '\'');
            ok = false;
        } else {
            dbColSource[col.dbColumn] = QStringLiteral("excel");
        }
    }

    // Build route index for fkInject cross-route checks
    // 【译】建立「表名→路由指针」索引，供后面 fkInject 校验「父表是否为本组某条路由」时
    //   O(1) 查找（fkInject 的父表必须是本 Profile 里声明过的另一条路由，而非任意 schema 表）。
    QHash<QString, const RouteSpec*> routeByTable;
    for (const auto& r : allRoutes)
        routeByTable[r.table] = &r;

    // §3.1, §3.2, §3.4, §3.9, §3.10 — Lookup validation
    // 【译】── 来源 2：lookup（外键正向查找）校验，覆盖规格 §3.1/§3.2/§3.4/§3.9/§3.10 ──
    QSet<QString> lookupNames;  // 本路由内已出现的 lookup 名（§3.9 名字唯一性）
    QSet<QString>
        allLookupTargets;  // all select.second across all lookups (for §3.4 cascade check)
                           // 【译】本路由所有 lookup 的输出列（select 的目标 dbColumn）汇总，
                           //   供 §3.4「禁止级联」检查使用——见本块之后那段独立循环。

    for (const LookupSpec& lk : route.lookups) {
        // §3.9 name unique within route
        // 【译】§3.9 lookup 名在本路由内必须唯一（名字用作诊断标识，重名无法区分）。
        //   重名即报错并 continue：跳过这条重名 lookup 的其余检查，避免基于错误前提连环报错。
        if (lookupNames.contains(lk.name)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                             QStringLiteral("route '") + route.table +
                                 QStringLiteral("': duplicate lookup name '") + lk.name + '\'');
            ok = false;
            continue;
        }
        lookupNames.insert(lk.name);

        // §3.1 fromTable in catalog
        // 【译】§3.1 参照表 G（fromTable）必须存在于 schema。不存在则无法查找，
        //   报 E_PROFILE_TABLE_NOT_FOUND 并 continue（后面的 match/select 都依赖 gTable）。
        if (!catalog.hasTable(lk.fromTable)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_TABLE_NOT_FOUND),
                             QStringLiteral("route '") + route.table +
                                 QStringLiteral("': lookup '") + lk.name +
                                 QStringLiteral("' from table '") + lk.fromTable +
                                 QStringLiteral("' not found in schema"));
            ok = false;
            continue;
        }
        const TableInfo& gTable = *catalog.table(lk.fromTable);  // 参照表 G 的 schema

        // §3.1 match.first must be G column; match.second must be Excel header
        // 【译】§3.1 match 是等值匹配对 (G 列, Excel 表头)：
        //   .first 必须是参照表 G 上真实存在的列；.second 必须是真实存在的 Excel 表头。
        for (const auto& mp : lk.match) {
            if (!gTable.column(mp.first)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': lookup '") + lk.name +
                                     QStringLiteral("' match column '") + mp.first +
                                     QStringLiteral("' not found in '") + lk.fromTable + '\'');
                ok = false;
            }
            if (!excelHeaders.contains(mp.second)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_HEADER_NOT_FOUND),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': lookup '") + lk.name +
                                     QStringLiteral("' match excel header '") + mp.second +
                                     QStringLiteral("' not found"));
                ok = false;
            }
        }

        // §3.1 select.first must be G column; §3.10 select targets unique within this lookup
        // 【译】§3.1 select 是取出对 (G 列, 写入本路由的局部 dbColumn)：
        //   .first 必须是 G 上真实存在的列；§3.10 各 .second（输出目标列）在「本 lookup 内」唯一。
        QSet<QString> selectTargetsThisLookup;  // 本 lookup 内已用过的输出目标列
        for (const auto& sp : lk.select) {
            if (!gTable.column(sp.first)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': lookup '") + lk.name +
                                     QStringLiteral("' select column '") + sp.first +
                                     QStringLiteral("' not found in '") + lk.fromTable + '\'');
                ok = false;
            }
            // §3.10 internal uniqueness
            // 【译】§3.10 同一 lookup 不能把两列都写入同一个目标 dbColumn（值二义）。
            if (selectTargetsThisLookup.contains(sp.second)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': lookup '") + lk.name +
                                     QStringLiteral("' has duplicate select target '") + sp.second +
                                     '\'');
                ok = false;
            } else {
                selectTargetsThisLookup.insert(sp.second);
            }
            // §3.2+§3.11 cross-source uniqueness
            // 【译】§3.2+§3.11 跨来源唯一性：本 lookup 的输出列若已被「Excel 列」或「另一个
            //   lookup」占用过（在 dbColSource 中），即冲突——同一目标列不能有两个值来源。
            //   否则登记本列来源为 "lookup:<本lookup名>"，并并入 allLookupTargets（供 §3.4）。
            if (dbColSource.contains(sp.second)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': lookup '") + lk.name +
                                     QStringLiteral("' select target '") + sp.second +
                                     QStringLiteral("' conflicts with ") + dbColSource[sp.second] +
                                     QStringLiteral(" source"));
                ok = false;
            } else {
                dbColSource[sp.second] = QStringLiteral("lookup:") + lk.name;
                allLookupTargets.insert(sp.second);
            }
        }
    }

    // §3.4 No lookup cascading: match.second must not be a lookup output target
    // 【译】§3.4 禁止 lookup 级联：一个 lookup 的「匹配键」(match.second，即某个 Excel 表头)
    //   不能恰好是「另一个 lookup 的输出列」。
    // 【为什么】级联意味着「先查出 A，再用 A 去查 B」——这会引入查找间的求值依赖与顺序问题，
    //   MVP 不支持，故在校验期直接拒绝。需要级联时应改用多步显式建模。
    // 【为什么单独成一轮循环】必须等上面那轮把「全部」lookup 的输出列收齐到 allLookupTargets
    //   之后才能判断，否则后声明的 lookup 输出会漏检（不能边收边判）。
    for (const LookupSpec& lk : route.lookups) {
        for (const auto& mp : lk.match) {
            if (allLookupTargets.contains(mp.second)) {
                errors->addTable(
                    sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                    QStringLiteral("route '") + route.table + QStringLiteral("': lookup '") +
                        lk.name + QStringLiteral("' match header '") + mp.second +
                        QStringLiteral("' is a lookup output — cascading not allowed"));
                ok = false;
            }
        }
    }

    // §3.3, §3.5, §3.6, §3.11 — fkInject validation
    // 【译】── 来源 3：fkInject（外键注入）校验，覆盖规格 §3.3/§3.5/§3.6/§3.11 ──
    //   fkInject 把「父路由那一行的某些列值」复制到本（子）路由的对应列。这是多表写入
    //   产生父子依赖、进而需要拓扑排序的根源。每个 FkInjectSpec = 一个父表 + 一组
    //   (父列 parent_column, 子列 child_column) 对（pairs）。
    QSet<QString> fkChildCols;  // 本路由所有注入组用过的子列（§3.6 跨组唯一）
    for (const FkInjectSpec& fk : route.fkInject) {
        // §3.3 from must be a declared route in this profile
        // 【译】§3.3 父表必须是本 Profile 里声明过的「另一条路由」，而非任意 schema 表。
        //   关键区分：要引用一张「也由本次导入写入」的表才用 fkInject（其主键导入时才生成）；
        //   若只是引用一张已存在的外部参照表，应改用 lookups。下面的错误信息正是据此分流：
        //   表在 schema 里存在但不是路由 → 提示「请改用 lookups」；表压根不存在 → 「未找到」。
        if (!routeByTable.contains(fk.fromTable)) {
            QString msg = QStringLiteral("route '") + route.table +
                          QStringLiteral("': fkInject from '") + fk.fromTable + '\'';
            if (catalog.hasTable(fk.fromTable)) {
                msg += QStringLiteral(
                    " exists in schema but is not a route in this profile; use lookups instead");
            } else {
                msg += QStringLiteral(" not found");
            }
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_TABLE_NOT_FOUND), msg);
            ok = false;
            continue;  // 父路由都不存在，pairs 的逐对检查无意义，跳过本注入组
        }
        const RouteSpec* parentRoute = routeByTable[fk.fromTable];

        // Build parent route's declared column sets (Excel + lookup outputs)
        // 【译】预先收集「父路由能产出的列」两类：Excel 直接映射列 + lookup 输出列。
        //   注入的父列（pair.first）必须来自这两类之一（§3.3），且这两类还用于 §3.5 判定。
        QSet<QString> parentExcelCols;
        for (const auto& col : parentRoute->columns)
            parentExcelCols.insert(col.dbColumn);
        QSet<QString> parentLookupTargets;
        for (const LookupSpec& lk : parentRoute->lookups) {
            for (const auto& sp : lk.select)
                parentLookupTargets.insert(sp.second);
        }

        // §3.5 All pairs must be either all lookup-derived or all Excel-derived
        // 【译】§3.5 同一注入组内的所有父列，必须「同源」——要么全是 lookup 派生，要么
        //   全是 Excel 派生，不可混用。
        // 【为什么】两类来源在导入时的求值时机/可用性不同；混在一组会让注入语义含糊。
        //   规格要求「混用时拆成两个独立注入组」。下面用「首对定基调、后续对比对」的方式探测混用。
        bool groupIsLookup = false;  // 本组基准：是否 lookup 派生（由首对决定）
        bool firstPair = true;
        bool mixedGroup = false;
        for (const auto& pair : fk.pairs) {
            bool pairIsLookup = parentLookupTargets.contains(pair.first);
            if (firstPair) {
                groupIsLookup = pairIsLookup;  // 首对确立本组基调
                firstPair = false;
            } else if (pairIsLookup != groupIsLookup) {
                mixedGroup = true;  // 出现与基调不同的对 → 混用
                break;
            }
        }
        if (mixedGroup) {
            errors->addTable(
                sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                QStringLiteral("route '") + route.table + QStringLiteral("': fkInject from='") +
                    fk.fromTable +
                    QStringLiteral("' mixes lookup-derived and Excel-derived parent_columns; "
                                   "split into two separate groups"));
            ok = false;
        }

        // 逐对校验 (父列, 子列)。
        for (const auto& pair : fk.pairs) {
            // §3.3 pair.first in parent route's Excel columns or lookup outputs
            // 【译】§3.3 父列必须确实是父路由能产出的列（Excel 映射列 或 lookup 输出列之一）。
            if (!parentExcelCols.contains(pair.first) &&
                !parentLookupTargets.contains(pair.first)) {
                errors->addTable(
                    sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                    QStringLiteral("route '") + route.table + QStringLiteral("': fkInject from='") +
                        fk.fromTable + QStringLiteral("' parent_column '") + pair.first +
                        QStringLiteral("' not found in parent route columns or lookup outputs"));
                ok = false;
            }
            // §3.3 pair.second in target table
            // 【译】§3.3 子列（注入目标）必须真实存在于本路由的目标表中。
            if (!tableInfo.column(pair.second)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': fkInject child_column '") + pair.second +
                                     QStringLiteral("' not found in table '") + route.table + '\'');
                ok = false;
            }
            // §3.6 child_column unique across all fkInject groups on this route
            // 【译】§3.6 同一子列不能被本路由的多个注入组同时写入（值二义）。
            //   首次见到就登记；之后再见同名子列即报「出现在多个注入组」。
            if (fkChildCols.contains(pair.second)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': fkInject child_column '") + pair.second +
                                     QStringLiteral("' appears in multiple injection groups"));
                ok = false;
            } else {
                fkChildCols.insert(pair.second);
                // §3.11 cross-source uniqueness
                // 【译】§3.11 跨来源唯一性：子列还不能与「Excel 列」或「lookup 输出」撞车
                //   （即不能既被注入又被别处写入）。无冲突则登记来源为 "fkInject:<父表>"。
                if (dbColSource.contains(pair.second)) {
                    errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                                     QStringLiteral("route '") + route.table +
                                         QStringLiteral("': fkInject child_column '") +
                                         pair.second + QStringLiteral("' conflicts with ") +
                                         dbColSource[pair.second] + QStringLiteral(" source"));
                    ok = false;
                } else {
                    dbColSource[pair.second] = QStringLiteral("fkInject:") + fk.fromTable;
                }
            }
        }
    }

    // 全部规则跑完，返回累积结果（true=本路由通过；中途任一失败即为 false）。
    return ok;
}

// ──────────────────────────────────────────────────────────────────────────
// validateRoutes —— 校验「一组路由」：先逐条 validateRoute，再做跨路由的 parent 检查。
// 注意把整组 routes 同时传给每条 validateRoute（作为 allRoutes），让 fkInject 能跨路由查父表。
// ──────────────────────────────────────────────────────────────────────────
bool ProfileValidator::validateRoutes(const QVector<RouteSpec>& routes,
                                      const SchemaCatalog& catalog, const QStringList& excelHeaders,
                                      const QString& sheet, ErrorCollector* errors) {
    bool ok = true;
    // 逐条校验每个路由（任一失败置 ok=false，但继续校验其余路由——尽量收集）。
    for (const auto& route : routes) {
        if (!validateRoute(route, routes, catalog, excelHeaders, sheet, errors))
            ok = false;
    }

    // Parent references
    // 【译】── 跨路由检查：parent 引用 ──
    //   RouteSpec.parent 指明本路由的「父路由表名」（空串=根路由）。这里校验：凡声明了
    //   parent 的路由，其 parent 必须确实是本组里另一条路由的表名，否则父子关系悬空。
    //   先把本组所有路由的表名收成集合，再逐个核对 parent 是否在其中。
    QStringList tableNameList;
    for (const auto& r : routes)
        tableNameList.append(r.table);
    QSet<QString> tableNames = QSet<QString>::fromList(tableNameList);
    for (const auto& r : routes) {
        if (!r.parent.isEmpty() && !tableNames.contains(r.parent)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_TABLE_NOT_FOUND),
                             QStringLiteral("Route '") + r.table + QStringLiteral("' parent '") +
                                 r.parent + QStringLiteral("' is not in routes"));
            ok = false;
        }
    }

    return ok;
}

// ──────────────────────────────────────────────────────────────────────────
// validateForExport —— 导出方向校验入口（H-03 fix）。详见头文件声明处的契约说明。
// 核心手法：导出时没有真实 Excel 表头可比对，于是「合成」一份覆盖 Profile 内全部列来源
// 的表头清单，使表头存在性检查必然通过；再以 importMode=false 委托 validate()，从而只
// 保留导出真正在意的规则（表/列存在、冲突键、orderBy、columnOrder 等）。
// ──────────────────────────────────────────────────────────────────────────
bool ProfileValidator::validateForExport(const ProfileSpec& profile, const SchemaCatalog& catalog,
                                         ErrorCollector* errors) {
    // H-03 fix: build a synthetic excelHeaders list that contains every column source
    // declared in the profile so that header-existence checks always pass.
    // Discriminator and Excel-header checks are import-only concerns — skip them here.
    // 【译】H-03 修复：构造一份「合成的 excelHeaders」——把 Profile 里声明过的每一个列来源
    //   都收进去，这样所有「表头是否存在」的检查都会通过。判别列（discriminator）与 Excel
    //   表头检查属导入专属关切，这里通过 importMode=false 一并跳过。
    QStringList syntheticHeaders;
    // 局部收集器：把一组路由里「columns 的 source」与「lookups 的 match 表头」都纳入合成表头。
    auto collectHeaders = [&](const QVector<RouteSpec>& routes) {
        for (const auto& route : routes) {
            for (const auto& col : route.columns)
                syntheticHeaders.append(col.source);
            for (const auto& lk : route.lookups) {
                for (const auto& mp : lk.match)
                    syntheticHeaders.append(mp.second);
            }
        }
    };
    if (profile.mode == ProfileMode::Mixed) {
        // 混合模式：遍历每个 class 的路由；并把判别列也加入（若非空），避免被误判缺失。
        for (const auto& cls : profile.classes)
            collectHeaders(cls.routes);
        if (!profile.discriminatorSource.isEmpty())
            syntheticHeaders.append(profile.discriminatorSource);
    } else {
        // 单表/多表模式：直接收集顶层 routes。
        collectHeaders(profile.routes);
    }
    // 复用同一套 validate()，仅以 importMode=false 切换为导出语义。
    return validate(profile, catalog, syntheticHeaders, errors, /*importMode=*/false);
}

// ──────────────────────────────────────────────────────────────────────────
// validate —— 导入/导出共用的「总校验入口」。详细契约见头文件声明处。
// 执行顺序：顶层字段 → 各路由（按模式分派 routes 或 classes）→ orderBy 时间列可排序性
//          → 导出 columnOrder。整段贯彻「尽量收集、不早退」。
// ──────────────────────────────────────────────────────────────────────────
bool ProfileValidator::validate(const ProfileSpec& profile, const SchemaCatalog& catalog,
                                const QStringList& excelHeaders, ErrorCollector* errors,
                                bool importMode) {
    bool ok = true;

    // ── 顶层必填字段 ──────────────────────────────────────────────────────────
    // Profile 名、作用工作表、表头行号都是执行的最低前提，缺一不可（E_PROFILE_PARSE）。
    if (profile.name.isEmpty()) {
        errors->addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                         QStringLiteral("profileName is empty"));
        ok = false;
    }
    if (profile.sheet.isEmpty()) {
        errors->addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                         QStringLiteral("sheet is empty"));
        ok = false;
    }
    if (profile.headerRow < 1) {
        // 表头行号 1 基，必须 >= 1（0 或负数无意义）。
        errors->addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                         QStringLiteral("headerRow must be >= 1"));
        ok = false;
    }

    // ── 按模式分派路由校验 ────────────────────────────────────────────────────
    if (profile.mode == ProfileMode::Mixed) {
        // 混合模式：先按 discriminator 列把行分到某个 class，再走该 class 的 routes。
        // M-05 fix: discriminator.source is only required for import; skip for export.
        // 【译】M-05 修复：判别列 discriminator.source 仅导入时必需（导入要据它给行分类）；
        //   导出时无需读 Excel 分类，故 importMode=false 时跳过这两项判别列检查。
        if (importMode && profile.discriminatorSource.isEmpty()) {
            errors->addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                             QStringLiteral("mixed mode requires discriminator.source"));
            ok = false;
        }
        // 判别列若已声明，导入时它必须真实存在于 Excel 表头中（否则无法读取分类依据）。
        if (importMode && !profile.discriminatorSource.isEmpty() &&
            !excelHeaders.contains(profile.discriminatorSource)) {
            errors->addTable(profile.sheet, QString::fromLatin1(err::E_HEADER_NOT_FOUND),
                             QStringLiteral("Discriminator source '") +
                                 profile.discriminatorSource +
                                 QStringLiteral("' not found in Excel headers"));
            ok = false;
        }
        // 逐个 class 校验其路由组（每组独立做路由内 + parent 跨路由检查）。
        for (const auto& cls : profile.classes) {
            if (!validateRoutes(cls.routes, catalog, excelHeaders, profile.sheet, errors))
                ok = false;
        }
    } else {
        // 单表 / 多表模式：直接校验顶层 routes。
        if (!validateRoutes(profile.routes, catalog, excelHeaders, profile.sheet, errors))
            ok = false;
    }

    // Check orderBy columns with non-sortable temporal dbFormat.
    // A dbFormat is dict-sort-safe only when it starts with "yyyy" (year is the MSB).
    // 【译】── orderBy 时间列「可排序性」启发式检查（产出非阻断警告 W_TIME_ORDERBY_NONSORTABLE）──
    // 【背景】导出排序在 SQL 里对存储值排序。若时间被存为字符串，则用的是「字典序」。
    //   只有当格式以 "yyyy" 打头（年份在最高位）时，字典序才恰好等于时间先后；像
    //   "dd-MM-yyyy" 这种以日/月打头的格式，字典序会乱（同月不同年会排错）。
    // 【为什么只是警告】这是一条启发式：用户也可能确有把握（如所有数据同年），故仅提示
    //   「字典序可能与时间序不符」，不阻断导出。
    if (!profile.exportSpec.orderBy.isEmpty()) {
        // Build a flat dbColumn → ColumnSpec pointer map across all routes.
        // 【译】先把所有路由的列摊平成「dbColumn → ColumnSpec*」映射，便于按列名 O(1) 反查
        //   其格式定义。重名 dbColumn 只保留首次出现者（contains 守卫），属启发式的可接受近似。
        QHash<QString, const ColumnSpec*> colByDb;
        auto indexRoutes = [&](const QVector<RouteSpec>& routes) {
            for (const auto& route : routes) {
                for (const auto& col : route.columns) {
                    if (!colByDb.contains(col.dbColumn))
                        colByDb[col.dbColumn] = &col;
                }
            }
        };
        if (profile.mode == ProfileMode::Mixed) {
            for (const auto& cls : profile.classes)
                indexRoutes(cls.routes);
        } else {
            indexRoutes(profile.routes);
        }

        for (const QString& ob : profile.exportSpec.orderBy) {
            // orderBy 项可写成 "col" 或 "table.col"；取最后一段作为纯列名来反查。
            QString colName = ob.contains('.') ? ob.section('.', -1) : ob;
            auto it = colByDb.find(colName);
            if (it == colByDb.end())
                continue;  // 该排序列不在映射列中（可能是原生 SQL 列等）→ 无从判断，略过
            const ColumnSpec& col = *it.value();
            // 判定这列是否为时间列、属哪种时间槽（date/datetime/time）。复用 ProfileSpec.h
            // 里的统一裁决逻辑，保证与导入/导出引擎对「是不是时间列」的判断一致。
            TemporalSlotKind kind = temporalSlotKindFor(col, profile);
            if (kind == TemporalSlotKind::None)
                continue;  // 非时间列 → 不涉及时间字典序问题，略过
            // 解出该列在该槽下「最终生效」的格式规格（合并列级与 profile 级）。
            TemporalFormatSpec eff = effectiveTemporalFor(kind, col, profile);
            // Skip non-string types (e.g. epochSec): INTEGER columns sort numerically, always safe.
            // 【译】跳过非字符串 DB 类型（如 epochSec）：存为 INTEGER 的时间按数值排序，
            //   天然与时间先后一致，永远安全，无需警告。
            if (eff.db.type != TemporalPhysType::String)
                continue;
            // 启发式核心判据：DB 侧格式串是否以 "yyyy" 开头。不是则发警告。
            if (!eff.db.format.startsWith(QStringLiteral("yyyy"))) {
                errors->addTableWarning(
                    profile.sheet, QString::fromLatin1(err::W_TIME_ORDERBY_NONSORTABLE),
                    QStringLiteral("orderBy column '") + colName +
                        QStringLiteral("' has db.format '") + eff.db.format +
                        QStringLiteral("' which does not start with 'yyyy' — dictionary sort order "
                                       "may not match chronological order"));
            }
        }
    }

    // add-export-column-order: validate columnOrder field.
    // 【译】── 导出列顺序 columnOrder 校验（特性 add-export-column-order）──
    //   columnOrder 指定导出 Excel 的列排列。这里校验三件事：①与原生 SQL 互斥；
    //   ②每个列名都是「已知表头」；③不得重复。其中「已知表头集合」需考虑 lookup 的
    //   反向查找会改变最终输出列（A 表头出现、H 列消失），故下面构造 knownHeaders 时
    //   按 exportRoundtrip 做了增删，见各注释。
    if (!profile.exportSpec.columnOrder.isEmpty()) {
        // 3.4: mutually exclusive with explicitSql
        // 【译】§3.4 columnOrder 与原生 SQL（explicitSql）互斥：用了原生 SQL 时，列顺序由
        //   SQL 自己决定，不能再用 columnOrder 干预。
        if (!profile.exportSpec.explicitSql.isEmpty()) {
            errors->addTable(
                profile.sheet, QString::fromLatin1(err::E_EXPORT_ORDER_WITH_RAW_SQL),
                QStringLiteral("export.columnOrder cannot be used together with export.sql; "
                               "raw SQL owns column ordering"));
            ok = false;
        }

        // 3.1: build known-header set (all ColumnSpec.source values across all routes)
        // add-export-reverse-lookup 3.2: A headers (match[].Excel_header for roundtrip=true
        // lookups) are added; H dbColumn names for roundtrip=true are removed from accepted set;
        // H dbColumn names for roundtrip=false are added (they appear in Excel output as-is).
        // 【译】§3.1 构建「已知表头集合」knownHeaders：先纳入所有路由 columns 的 source。
        //   再按反向查找（reverse-lookup）规则调整，使集合反映「替换后真正出现在 Excel 的列」：
        //     · roundtrip=true（要反查回业务键 A）：A 表头(match.second)出现 → 加入；
        //       对应的 H 列(select.second，库里的代理列)不再直接出现 → 移除；
        //     · roundtrip=false（不反查，H 列原样输出）：H 列(select.second)出现 → 加入。
        QSet<QString> knownHeaders;
        auto collectSources = [&](const QVector<RouteSpec>& routes) {
            for (const auto& route : routes) {
                for (const auto& col : route.columns)
                    knownHeaders.insert(col.source);  // 基础：每列的 Excel 表头来源
                // Add/remove lookup-related headers to reflect post-substitution output set
                // 【译】按 lookup 增删表头，反映「替换后」的实际输出列集合。
                for (const auto& lk : route.lookups) {
                    if (lk.exportRoundtrip) {
                        // A headers appear in output; H dbColumns do not
                        // 【译】反查启用：A 表头出现于输出、H 代理列不出现。
                        for (const auto& mp : lk.match)
                            knownHeaders.insert(mp.second);  // A = match[].Excel_header（加）
                        for (const auto& sp : lk.select)
                            knownHeaders.remove(sp.second);  // H = select[].dbColumn（删）
                    } else {
                        // H dbColumns appear in output verbatim
                        // 【译】不反查：H 代理列原样出现于输出。
                        for (const auto& sp : lk.select)
                            knownHeaders.insert(sp.second);  // H = select[].dbColumn（加）
                    }
                }
            }
        };
        if (profile.mode == ProfileMode::Mixed) {
            for (const auto& cls : profile.classes)
                collectSources(cls.routes);
            // classColumn is a synthetic header — valid in columnOrder for Mixed mode
            // 【译】混合模式下 classColumn 是「合成表头」（承载类别 id），在 columnOrder 里合法 →
            // 纳入。
            if (!profile.exportSpec.classColumn.isEmpty())
                knownHeaders.insert(profile.exportSpec.classColumn);
        } else {
            collectSources(profile.routes);
        }

        // Hint: first up-to-5 known headers for error messages
        // 【译】错误信息辅助：取已知表头的前 5 个拼成提示串，帮用户快速对照「应该写哪些列名」。
        auto knownHint = [&]() -> QString {
            QStringList sample = knownHeaders.values().mid(0, 5);
            return QStringLiteral(" (known headers: ") + sample.join(QStringLiteral(", ")) +
                   QStringLiteral(")");
        };

        // 3.2 + 3.3: unknown header and duplicate checks
        // 【译】§3.2+§3.3 逐项检查 columnOrder：先查重复（seen 集合），再查是否为已知表头。
        QSet<QString> seen;
        for (const QString& h : profile.exportSpec.columnOrder) {
            if (seen.contains(h)) {
                errors->addTable(profile.sheet, QString::fromLatin1(err::E_EXPORT_DUPLICATE_ORDER),
                                 QStringLiteral("export.columnOrder contains duplicate entry '") +
                                     h + QStringLiteral("'"));
                ok = false;
                continue;
            }
            seen.insert(h);
            if (!knownHeaders.contains(h)) {
                errors->addTable(profile.sheet, QString::fromLatin1(err::E_EXPORT_UNKNOWN_HEADER),
                                 QStringLiteral("export.columnOrder entry '") + h +
                                     QStringLiteral("' does not match any column source") +
                                     knownHint());
                ok = false;
            }
        }
    }

    return ok;
}

}  // namespace dbridge::detail
