// ============================================================================
// Mapper.cpp — Mapper 的实现：逐行映射、跑校验链、做时间格式转换
// ============================================================================
//
// 本文件实现 Mapper.h 声明的三件事：
//   1) routeKey()           —— 计算路由键（详见头文件）。
//   2) compileValidators()  —— 一次性编译每列的校验链。
//   3) map()                —— 逐行把 Excel 单元格翻译成 RoutePayload。
//
// 阅读顺序建议：先看 routeKey（最简单）→ compileValidators（建表）→ map（核心循环）。
// ============================================================================

#include "Mapper.h"

#include "dbridge/Errors.h"

#include "TemporalConvert.h"         // tconv::*：时间值的判空/识别/解析/序列化
#include "excel/ExcelReader.h"       // ExcelReader::cellBySource：按表头名取单元格
#include "service/ErrorCollector.h"  // ErrorCollector::add：追加行级错误

namespace dbridge::detail {

// routeKey —— 见头文件注释：非 Mixed 用表名，Mixed 用「类别:表名」。
// 例：classId="" → "orders"；classId="A" → "A:orders"。
QString Mapper::routeKey(const RouteSpec& route, const QString& classId) {
    if (classId.isEmpty())
        return route.table;
    return classId + QStringLiteral(":") + route.table;
}

// compileValidators —— 把每条路由每列的 validator token 编译成可执行校验链。
// 输出写入 *vm（二级表 routeKey → dbColumn → ValidatorChain）。
bool Mapper::compileValidators(const QVector<RouteSpec>& routes, const QString& classId,
                               const ProfileSpec& profile, ValidatorMap* vm, QString* err) {
    for (const auto& route : routes) {
        QString rk = routeKey(route, classId);  // 本路由的查找键
        for (const auto& col : route.columns) {
            QStringList tokens = col.validatorTokens;  // 拷贝一份：可能要就地剥离 date:* token

            // ── 关键：决定是否剥离该列的 date:* 校验 token ──────────────────────
            // 若本列声明了「有效时间槽」（即列级或 Profile 级声明了 date/datetime/time
            // 格式对象，effectiveTemporalFor(...).declared == true），则它的时间解析将由
            // map() 中的 tconv 时间转换层负责，这里必须把 date:* 校验剥掉，避免：
            //   ① 重复解析；② validator 与 tconv 对同一格式理解不一致导致误判。
            // 反之，「遗留」列（只写了 date:fmt 校验、没声明时间槽对象）保留其 validator，
            // 由校验链自己解析日期——这是为兼容旧 Profile 而保留的路径。
            TemporalSlotKind kind = temporalSlotKindFor(col, profile);
            if (kind != TemporalSlotKind::None &&
                effectiveTemporalFor(kind, col, profile).declared) {
                // erase-remove 惯用法：把所有以 "date:" 开头的 token 从 tokens 中移除。
                tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                                            [](const QString& t) {
                                                return t.startsWith(QStringLiteral("date:"));
                                            }),
                             tokens.end());
            }

            // 编译剩余 token 为校验链；任一 token 非法则整体失败（err 已由 compile 填写）。
            ValidatorChain chain;
            if (!chain.compile(tokens, err))
                return false;  // 上游通常将此类失败归为 E_PROFILE_PARSE（配置阶段错误）
            // 存入二级表；std::move 转移所有权，避免拷贝 std::function 向量。
            (*vm)[rk][col.dbColumn] = std::move(chain);
        }
    }
    return true;
}

// map —— 把「一行 Excel」翻译为一组 RoutePayload（每条路由一个）。详见头文件函数注释。
QVector<RoutePayload> Mapper::map(const QVector<RouteSpec>& routes, int excelRow,
                                  const QString& classId, const ExcelReader& reader,
                                  const ValidatorMap& vm, const ProfileSpec& profile,
                                  ErrorCollector* errors, const QString& sheetName) const {
    QVector<RoutePayload> payloads;

    // ── 外层循环：每条路由产出一个 RoutePayload（= 写一张目标表的材料）────────────
    for (const auto& route : routes) {
        QString rk = routeKey(route, classId);
        RoutePayload payload;
        payload.table = route.table;                   // 目标表名
        payload.routeKey = rk;                         // 路由键（供下游/查重定位）
        payload.conflictKey = route.conflict.columns;  // UPSERT 冲突键列名（值稍后回填）

        // 取该路由对应的「列名 → 校验链」子表；缺失返回空 QHash（每列再取得空链）。
        const auto& chainMap = vm.value(rk);

        // rowHasError：本路由处理过程中是否已出错。一旦置位，后续列的时间转换会被跳过
        //   （见下方时间转换的 `!rowHasError` 守卫）——因为该路由已注定失败，无需再算。
        bool rowHasError = false;

        // ── 中层循环：逐列取值 → 校验 → 时间转换 → 入 payload ────────────────────
        for (const auto& col : route.columns) {
            // 按列的 source（Excel 表头名）取该行单元格原始值。
            QVariant rawVal = reader.cellBySource(excelRow, col.source);
            QVariant normalizedVal = rawVal;  // 规整后的值；校验/转换会逐步改写它

            // ── 步骤 1：跑校验链（非空/类型/正则/枚举/长度等）──────────────────
            const ValidatorChain& chain = chainMap.value(col.dbColumn);
            if (!chain.isEmpty()) {
                QString errCode, errMsg;
                // chain.run：成功时把规整值写入 normalizedVal；失败时返回 false 并填错误码。
                if (!chain.run(rawVal, &normalizedVal, &errCode, &errMsg)) {
                    // M-06 fix：带上 sheetName，使错误条目定位完整（表/行/列俱全）。
                    errors->add(sheetName, excelRow, col.source, rawVal.toString(), errCode,
                                errMsg);
                    normalizedVal = rawVal;  // 失败则恢复原值（不让半成品污染 payload）
                    rowHasError = true;
                    // H-01 fix：只把「本路由」的 payload 标记为失败，不牵连整行。
                    //   ImportService 据此把该 routeIndex 加入 failedRouteIndices，
                    //   而非置 hasNonRouteError（后者会跳过整行的所有路由）。
                    payload.hasError = true;
                }
            }

            // ── 步骤 2：时间格式转换（解析 U → 结构化 → 序列化 V）──────────────
            // 仅当本列存在「有效时间槽」（声明了 date/datetime/time 格式对象）时才启用；
            // 遗留的「只有 date:fmt 校验」列在步骤 1 已由 validator 处理，这里不再介入。
            // `!rowHasError` 守卫：若步骤 1 已失败，本列无需再转换。
            TemporalSlotKind kind = temporalSlotKindFor(col, profile);
            if (kind != TemporalSlotKind::None && !rowHasError) {
                TemporalFormatSpec eff = effectiveTemporalFor(kind, col, profile);
                // eff.excel = 输入侧(U)格式；eff.db = 输出侧(V)格式。任一侧声明才转换。
                if (eff.excel.declared || eff.db.declared) {
                    if (tconv::isEmptyForTemporal(normalizedVal)) {
                        // 空/纯空白 → 视作无时间值，写 NULL（不报错）。
                        normalizedVal = QVariant();
                    } else if (tconv::isStructuredTemporal(normalizedVal, kind)) {
                        // 情形 A：单元格本身已是 Excel 原生日期/时间（QDate/QDateTime/QTime），
                        //   无需用 U 解析字符串，直接用 V 序列化为目标侧表示。
                        QVariant serialized = tconv::formatValue(normalizedVal, kind, eff.db);
                        // 序列化失败（如类型与槽不符）→ 退化为 NULL（不视为行级错误）。
                        normalizedVal = (serialized.isValid() && !serialized.isNull()) ? serialized
                                                                                       : QVariant();
                    } else {
                        // 情形 B：单元格是字符串 → 先用 U（含 fallback）解析为结构化时间，
                        //   再用 V 序列化为目标侧表示。
                        QString errCode, errMsg;
                        QVariant structured =
                            tconv::toStructured(normalizedVal, kind, eff.excel, &errCode, &errMsg);
                        if (!structured.isValid()) {
                            // 解析失败 → 行级错误（E_TIME_PARSE，由 toStructured 填码）。
                            errors->add(sheetName, excelRow, col.source, rawVal.toString(), errCode,
                                        errMsg);
                            normalizedVal = rawVal;  // 恢复原值
                            rowHasError = true;
                            // H-01 fix：时间解析失败同样是「路由局部」错误，只废本路由 payload，
                            //   不波及整行的其它路由。
                            payload.hasError = true;
                        } else {
                            // 解析成功 → 序列化为目标侧；失败则退化为 NULL（不报错）。
                            QVariant serialized = tconv::formatValue(structured, kind, eff.db);
                            normalizedVal = (serialized.isValid() && !serialized.isNull())
                                                ? serialized
                                                : QVariant();
                        }
                    }
                }
            }

            // 把（列名, 规整值）按序追加：dbColumns 与 binds 始终一一对应、同序同长。
            payload.dbColumns.append(col.dbColumn);
            payload.binds.append(normalizedVal);
        }

        // ── 步骤 3：回填冲突键值 conflictVals ────────────────────────────────────
        // 冲突键列名取自 route.conflict.columns；逐个在本 payload 的已绑定列里找其值。
        for (const auto& ck : route.conflict.columns) {
            int idx = payload.indexOf(ck);  // 在 dbColumns 中定位列下标
            if (idx >= 0) {
                payload.conflictVals.append(payload.binds[idx]);  // 该列已绑定 → 取其值
            } else {
                // 冲突键列尚未出现在本路由的列里（典型：外键列由 FkInjector 后续注入）。
                // 先占位 QVariant()，待 FkInjector 注入父表主键后再回填（见 FkInjector.cpp）。
                payload.conflictVals.append(QVariant());
            }
        }

        payloads.append(payload);
    }

    return payloads;
}

}  // namespace dbridge::detail
