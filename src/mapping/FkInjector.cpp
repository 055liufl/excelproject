// ============================================================================
// FkInjector.cpp — 外键注入的实现：把父载荷主键复制进子载荷外键列
// ============================================================================
//
// 本文件实现 FkInjector.h 的两个重载：
//   1) inject(payloads, err)            —— 废弃桩，必失败（L-01 fix），见下。
//   2) inject(payloads, routes, ...)    —— 正式实现。
//
// 【正式重载的处理骨架（建立直觉，详见函数内逐段注释）】
//   建索引：表名→载荷下标、路由 i→父载荷下标 parentIdx[i]
//   对每条带 fkInject 的路由 i：
//     ① 沿 parentIdx 向上回溯：任一祖先已失败 → 跳过（失败沿父子链向下传播）
//     ② 逐条 fkInject、逐个 (父列,子列) 对：
//          取父值 → 父值 null？报错、标失败
//                 → 子列已存在且非空且≠父值？报「父子冲突」、标失败
//                 → 否则：写入/追加子列值，并同步回填该列在 conflictKey 中的冲突值
//     ③ 本路由若有任何失败 → 加入 failedIdxs
// ============================================================================

#include "FkInjector.h"

#include "dbridge/Errors.h"

#include "profile/ProfileSpec.h"
#include "service/ErrorCollector.h"

namespace dbridge::detail {

// inject（废弃桩）—— 见头文件。此重载缺 RouteSpec 上下文，无法注入，故意失败。
bool FkInjector::inject(QVector<RoutePayload>& /*payloads*/, QString* err) {
    // L-01 fix: this overload is a no-op stub that predates the RouteSpec-aware overload below.
    // Callers MUST use inject(payloads, routes, excelRow, sheet, errors, initialFailed) instead.
    // Leaving this callable would silently skip FK injection if called by mistake.
    // 译：L-01 修复——此重载是早于下方「带 RouteSpec」版本的历史空操作桩。
    //     调用方必须改用 inject(payloads, routes, excelRow, sheet, errors, initialFailed)。
    //     之所以不直接删除而保留并令其「显式失败」：若误调旧重载而它静默不做事，
    //     外键将悄无声息地不被注入（极难排查）；返回 false + 明确报错能立刻暴露误用。
    if (err)
        *err = QStringLiteral(
            "FkInjector::inject() called without RouteSpec context — "
            "use the RouteSpec overload (fk-injection spec §1)");
    return false;
}

// inject（正式重载）—— 详见头文件函数注释与文件头骨架。
QSet<int> FkInjector::inject(QVector<RoutePayload>& payloads, const QVector<RouteSpec>& routes,
                             int excelRow, const QString& sheet, ErrorCollector* errors,
                             QSet<int> initialFailed) {
    // ── 索引 1：表名 → 该表载荷在 payloads 中的下标 ─────────────────────────────
    // 便于按「父表名」快速取到父载荷。
    QHash<QString, int> tableToPayloadIdx;
    for (int i = 0; i < payloads.size(); ++i)
        tableToPayloadIdx[payloads[i].table] = i;

    // ── 索引 2：路由 i → 其父载荷的下标 parentIdx[i]（无父或父不在本行则为 -1）────
    // 后面要沿这条「子→父」链向上回溯，判断祖先是否失败。
    QHash<int, int> parentIdx;
    for (int i = 0; i < routes.size(); ++i) {
        const QString& parentTable = routes[i].parent;
        if (!parentTable.isEmpty() && tableToPayloadIdx.contains(parentTable))
            parentIdx[i] = tableToPayloadIdx[parentTable];
        else
            parentIdx[i] = -1;  // 根路由，或父表未在本行展开
    }

    // 失败集：以入参 initialFailed 为起点（move 接管，避免拷贝），本函数继续往里加。
    QSet<int> failedIdxs = std::move(initialFailed);

    for (int i = 0; i < routes.size(); ++i) {
        const RouteSpec& route = routes[i];
        if (route.fkInject.isEmpty())
            continue;  // 该路由无外键注入需求

        // ── ① 失败沿父子链传播：任一祖先失败，则本路由也跳过 ─────────────────────
        // 道理：父行不会被写库（已失败），其代理主键无从谈起，子表注入毫无意义。
        // 沿 parentIdx 一路向上走到根，途中遇到失败的祖先即判定 ancestorFailed。
        bool ancestorFailed = false;
        int ancestor = parentIdx.value(i, -1);
        while (ancestor >= 0) {
            if (failedIdxs.contains(ancestor)) {
                ancestorFailed = true;
                break;
            }
            ancestor = parentIdx.value(ancestor, -1);  // 继续往上一代
        }
        if (ancestorFailed)
            continue;

        RoutePayload& childPayload = payloads[i];  // 本（子）路由载荷，将被就地写入
        bool rowFailed = false;                    // 本路由是否在注入中出现失败

        // ── ② 逐条 fkInject 规则、逐个 (父列,子列) 对处理 ───────────────────────
        for (const FkInjectSpec& fk : route.fkInject) {
            auto parentIt = tableToPayloadIdx.find(fk.fromTable);
            if (parentIt == tableToPayloadIdx.end())
                continue;  // 父表载荷不在本行（可能该父路由未触发）→ 此组无从注入，跳过

            const RoutePayload& parentPayload = payloads[parentIt.value()];
            for (const auto& pair : fk.pairs) {
                const QString& parentCol = pair.first;  // 父表里取值的列
                const QString& childCol = pair.second;  // 子表里写入的列

                // 取父值：父载荷里若没有这列，跳过这一对（无值可注）。
                const int fromIdx = parentPayload.indexOf(parentCol);
                if (fromIdx < 0)
                    continue;
                const QVariant fkVal = parentPayload.binds[fromIdx];

                // 父值为 NULL → 无法作为外键注入 → 记错误、标失败。
                if (fkVal.isNull()) {
                    if (errors) {
                        errors->add(sheet, excelRow, childCol, QString(),
                                    QString::fromLatin1(err::E_VALIDATE_FK),
                                    QStringLiteral("fkInject from '") + fk.fromTable +
                                        QStringLiteral("': parent column '") + parentCol +
                                        QStringLiteral("' is NULL; cannot inject into '") +
                                        childCol + '\'');
                    }
                    rowFailed = true;
                    continue;
                }

                int toIdx = childPayload.indexOf(childCol);
                if (toIdx >= 0) {
                    // 子列已存在：
                    const QVariant current = childPayload.binds[toIdx];
                    // 若子列已有「非空且与父值不等」的值 → 父子外键冲突（Excel 显式填了别的值）。
                    if (!current.isNull() && current != fkVal) {
                        if (errors) {
                            errors->add(sheet, excelRow, childCol, current.toString(),
                                        QString::fromLatin1(err::E_VALIDATE_FK),
                                        QStringLiteral("fkInject conflict for '") + childCol +
                                            QStringLiteral("': child value '") +
                                            current.toString() +
                                            QStringLiteral("' does not match parent value '") +
                                            fkVal.toString() + '\'');
                        }
                        rowFailed = true;
                        continue;
                    }
                    // 子列原为空、或已等于父值 → 用父值覆盖（幂等）。
                    childPayload.binds[toIdx] = fkVal;
                } else {
                    // 子列尚不存在 → 新增一列（dbColumns/binds 保持一一对应、同序追加）。
                    childPayload.dbColumns.append(childCol);
                    childPayload.binds.append(fkVal);
                }

                // ── 同步回填冲突键值 ─────────────────────────────────────────────
                // 若该子列恰是 UPSERT 冲突键的一员，把刚注入的父值同步填进 conflictVals
                // 对应槽位——否则冲突键里这一项还是 Mapper 阶段留下的占位 null（见 Mapper.cpp
                // 步骤 3「外键列由 FkInjector 后续注入」的占位说明），会导致 UPSERT 判键错误。
                for (int ci = 0; ci < childPayload.conflictKey.size(); ++ci) {
                    if (childPayload.conflictKey[ci] == childCol &&
                        ci < childPayload.conflictVals.size()) {
                        childPayload.conflictVals[ci] = fkVal;
                    }
                }
            }
        }

        // ── ③ 本路由若有失败，记入失败集（供调用方与后续传播使用）──────────────────
        if (rowFailed)
            failedIdxs.insert(i);
    }

    return failedIdxs;
}

}  // namespace dbridge::detail
