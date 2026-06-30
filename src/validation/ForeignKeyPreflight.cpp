// ============================================================================
// ForeignKeyPreflight.cpp — 外键预检的实现：建批内父行索引 + 逐载荷探测父行存在性
// ============================================================================
//
// 本文件实现 ForeignKeyPreflight.h 声明的两件事：
//   1) check()        —— 顶层：建「批内父行缓存」与「表名→RouteSpec」索引，逐行逐载荷预检，
//                        失败处回写 ctx.failedRouteIndices（H-02 fix）。
//   2) checkPayload() —— 对单个子载荷逐条 fkInject 规则做预检（含两层免查优化 + 最终 SQL 探测）。
//
// 【一次预检的判定顺序（建立直觉，详见 checkPayload 内逐段注释）】
//   对载荷的每条 fkInject 规则：
//     §7.5 整组父键都来自父路由 lookup？ → 是则跳过（prefetch 已校验存在性）
//       ↓ 否
//     组装子侧外键元组；遇「列还没注入」或「值为 null」？ → 跳过（错误另有人报）
//       ↓ 否
//     该元组在「本批待写父载荷」里能匹配上？ → 能则通过（免查库）
//       ↓ 否
//     向 DB 发 `SELECT 1 FROM 父表 WHERE 各父列=? LIMIT 1`：查到 → 过；查不到 → E_VALIDATE_FK
// ============================================================================

#include "ForeignKeyPreflight.h"

#include "dbridge/Errors.h"

#include <QSet>
#include <QSqlError>
#include <QSqlQuery>

#include "service/ErrorCollector.h"
#include "sql/SqlBuilder.h"

namespace dbridge::detail {

// check —— 顶层入口。详见头文件函数注释。
bool ForeignKeyPreflight::check(QVector<RowContext>& contexts, const QVector<RouteSpec>& allRoutes,
                                QSqlDatabase& db, const QString& sheet, ErrorCollector* errors) {
    // M-02 fix: exclude already-failed routes from the in-batch parent cache.
    // Including failed payloads would let a child route incorrectly pass FK preflight
    // even when its parent will not actually be written (because it failed earlier).
    // 译：M-02 修复——构建「批内父行缓存」时，必须排除「已经失败」的路由载荷。
    //     若把失败的父载荷也算作「批内存在的父行」，子路由会被误判为通过预检——
    //     可那个父行其实根本不会被写库（它早先已失败），结果造成悬空外键。
    //
    // batchParentPayloads：表名 → 该表在本批中「仍有效（未失败）」的所有待写载荷。
    //   后续 checkPayload 的「批内命中」优化就是在这张表里找匹配父行。
    QHash<QString, QVector<RoutePayload>> batchParentPayloads;
    for (const auto& ctx : contexts) {
        for (int pi = 0; pi < ctx.payloads.size(); ++pi) {
            if (!ctx.failedRouteIndices.contains(pi))  // 跳过已失败的路由载荷
                batchParentPayloads[ctx.payloads[pi].table].append(ctx.payloads[pi]);
        }
    }

    // routeByTable：表名 → 对应 RouteSpec 指针。用于按载荷的目标表名快速取回其路由规格
    //   （读取 fkInject / lookups 等配置）。allRoutes 生命周期覆盖本次 check，存裸指针安全。
    QHash<QString, const RouteSpec*> routeByTable;
    for (const auto& r : allRoutes)
        routeByTable[r.table] = &r;

    bool allOk = true;
    // H-02 fix: iterate with index so we can write back to ctx.failedRouteIndices.
    // 译：H-02 修复——带下标 pi 遍历，以便把失败的路由下标精确回写到 ctx.failedRouteIndices。
    for (auto& ctx : contexts) {
        for (int pi = 0; pi < ctx.payloads.size(); ++pi) {
            const RoutePayload& payload = ctx.payloads[pi];
            const RouteSpec* rs = routeByTable.value(payload.table, nullptr);
            // 没有对应路由规格、或该表压根没有 fkInject → 无外键可检，跳过。
            if (!rs || rs->fkInject.isEmpty())
                continue;
            if (!checkPayload(payload, *rs, batchParentPayloads, routeByTable, db, sheet,
                              ctx.excelRow, errors)) {
                allOk = false;
                // H-02 fix: record which payload (route index) failed FK preflight so the write
                // phase skips only that route (and its descendants), not the entire Excel row.
                // 译：H-02 修复——记下「哪个载荷（路由下标）」外键预检失败，使写阶段只跳过
                //     这一条路由（及其子孙路由），而不是丢弃整行 Excel 数据。
                ctx.failedRouteIndices.insert(pi);
            }
        }
    }
    return allOk;
}

// checkPayload —— 对单个子载荷逐条 fkInject 规则预检。详见头文件 + 文件头判定顺序。
bool ForeignKeyPreflight::checkPayload(
    const RoutePayload& payload, const RouteSpec& routeSpec,
    const QHash<QString, QVector<RoutePayload>>& batchParentPayloads,
    const QHash<QString, const RouteSpec*>& routeByTable, QSqlDatabase& db, const QString& sheet,
    int excelRow, ErrorCollector* errors) {
    bool ok = true;

    // 一个子表可能引用多个父表 → routeSpec.fkInject 是一组规则，逐条检查。
    for (const FkInjectSpec& fk : routeSpec.fkInject) {
        // §7.5 Group-level skip for lookup-derived groups.
        // If every pair.first in this group is a lookup select target of the parent route,
        // the parent values were validated at prefetch time — skip the DB probe entirely.
        // 译：§7.5 「lookup 派生组」整组跳过优化。
        //     若本组每个 pair.first（父列名）都恰好是「父路由 lookup 的 select 取出列」，
        //     说明这些父键值在 prefetch（lookup 预取）阶段就已确认能在参照表里查到——
        //     它们本就源自一次成功的查找，无需再向数据库探测父行存在性，整组直接跳过。
        const RouteSpec* parentSpec = routeByTable.value(fk.fromTable, nullptr);
        if (parentSpec && !parentSpec->lookups.isEmpty()) {
            // 收集父路由所有 lookup 的 select 目标列名（即「由查找产出的局部列」）。
            QSet<QString> parentLookupTargets;
            for (const LookupSpec& lk : parentSpec->lookups) {
                for (const auto& sp : lk.select)
                    parentLookupTargets.insert(sp.second);  // sp.second = 写入父路由的局部列名
            }
            // 仅当本组「每一个」父列都在上述集合内，才算「整组派生自 lookup」。
            bool allLookupDerived = true;
            for (const auto& pair : fk.pairs) {
                if (!parentLookupTargets.contains(pair.first)) {
                    allLookupDerived = false;
                    break;
                }
            }
            if (allLookupDerived)
                continue;  // validated at prefetch; no SQL probe needed
                           // 译：已在 prefetch 阶段校验过，无需 SQL 探测——跳过本组
        }

        // Build the child-side tuple and the SQL column list from pair.second (child cols).
        // 译：组装「子侧外键元组」以及供 SQL 用的列名清单。pair.first=父列、pair.second=子列。
        //     childTuple = 子表这几列当前的值（即将拿去和父表比对）；
        //     childCols  = 子列名（仅用于错误信息定位）；
        //     parentCols = 父列名（用于拼 WHERE 子句 与 错误信息）。
        QVector<QVariant> childTuple;
        QStringList childCols;
        QStringList parentCols;
        bool anyMissing = false;  // 是否有「子列尚未出现在载荷里」（外键还没被 FkInjector 注入）
        bool anyNull = false;  // 是否有「子列值为 null」

        for (const auto& pair : fk.pairs) {
            int idx = payload.indexOf(pair.second);  // 在子载荷的 dbColumns 中定位该子列
            if (idx < 0) {
                anyMissing = true;  // 该列还没被注入 → 本组暂不可检
                break;
            }
            QVariant v = payload.binds[idx];
            if (v.isNull()) {
                anyNull = true;  // 外键值为 null → 本组不检（可空外键 / 上游另报）
                break;
            }
            childTuple.append(v);
            childCols.append(pair.second);
            parentCols.append(pair.first);
        }

        if (anyMissing || anyNull)
            continue;  // not yet injected or null — upstream reports the error
                       // 译：尚未注入、或值为 null ——
                       // 这类问题由上游（FkInjector）负责报错，本组跳过

        // Check in-batch: compare tuple against parent payload binds by parent column name
        // 译：先查「本批次」——把子侧元组按「父列名」与本批所有父载荷的绑定值逐列比对。
        //     只要批内存在一行父载荷的对应列值全部相等，即认定父行「将随本批写入」→ 通过。
        const auto& batchPayloads = batchParentPayloads.value(fk.fromTable);
        bool foundInBatch = false;
        for (const RoutePayload& parentPayload : batchPayloads) {
            bool match = true;
            for (int i = 0; i < fk.pairs.size(); ++i) {
                int pIdx = parentPayload.indexOf(fk.pairs[i].first);  // 父载荷里定位该父列
                // 父载荷缺该列、或值与子元组对应项不等 → 此父载荷不匹配。
                if (pIdx < 0 || parentPayload.binds[pIdx] != childTuple[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                foundInBatch = true;
                break;
            }
        }
        if (foundInBatch)
            continue;  // 批内已有对应父行 → 通过，免查库

        // Not in batch — probe the DB with a composite WHERE clause
        // 译：批内没有 → 退而向数据库探测（用「各父列 AND 相等」的复合 WHERE）。
        if (onProbe)
            onProbe(fk.fromTable);  // 测试钩子：记一次真实探测（验证免查优化的命中率）

        // H-05 fix: quote all identifiers (table name and column names).
        // 译：H-05 修复——对所有标识符（表名、列名）做转义，防注入并兼容含特殊字符的名字。
        QStringList conditions;
        for (const auto& pc : parentCols)
            conditions.append(detail::SqlBuilder::quoteIdent(pc) + QStringLiteral(" = ?"));
        // 形如：SELECT 1 FROM "父表" WHERE "c1" = ? AND "c2" = ? LIMIT 1
        //   只取 1、LIMIT 1：只关心「存在与否」，不取数据、命中一条即可。
        QString sql = QStringLiteral("SELECT 1 FROM ") +
                      detail::SqlBuilder::quoteIdent(fk.fromTable) + QStringLiteral(" WHERE ") +
                      conditions.join(QStringLiteral(" AND ")) + QStringLiteral(" LIMIT 1");

        QSqlQuery q(db);
        q.prepare(sql);
        for (const auto& v : childTuple)
            q.addBindValue(v);  // 占位符顺序与 conditions 一致，故按 childTuple 顺序绑定

        if (!q.exec()) {
            // 探测 SQL 执行失败（如表不存在 / 连接异常）——记为 E_VALIDATE_FK，消息含底层报错。
            errors->add(sheet, excelRow, childCols.join(QLatin1Char(',')), childTuple[0].toString(),
                        QString::fromLatin1(err::E_VALIDATE_FK),
                        QStringLiteral("FK check query failed: ") + q.lastError().text());
            ok = false;
            continue;  // 继续检查本载荷的下一条 fkInject（不因一条失败而早退）
        }

        if (!q.next()) {
            // 查询成功但「零结果」→ 父行不存在 → 外键违例。组装详尽诊断：列=值, 列=值 ...
            QStringList tupleParts;  // 人类可读："父列=值" 形式，拼进消息
            QStringList tupleVals;   // 仅各值，拼进错误条目的 rawValue 字段
            for (int i = 0; i < parentCols.size(); ++i) {
                tupleParts.append(parentCols[i] + QLatin1Char('=') + childTuple[i].toString());
                tupleVals.append(childTuple[i].toString());
            }
            errors->add(sheet, excelRow, childCols.join(QLatin1Char(',')),
                        tupleVals.join(QLatin1Char(',')), QString::fromLatin1(err::E_VALIDATE_FK),
                        QStringLiteral("Foreign key (") + tupleParts.join(QStringLiteral(", ")) +
                            QStringLiteral(") not found in ") + fk.fromTable);
            ok = false;
        }
    }

    return ok;
}

}  // namespace dbridge::detail
