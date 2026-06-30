#pragma once
#include <QString>
#include <QStringList>
#include <QVariant>

#include "profile/ProfileSpec.h"

// ============================================================================
// TemporalConvert.h — 时间值在「Excel 表示 ↔ DB 表示」之间的转换工具集（tconv）
// ============================================================================
//
// 【单一职责】
//   一组无状态的自由函数（namespace tconv），负责时间列的格式转换：
//     · 判空：什么样的单元格算「没有时间值」（→ 写 NULL）。
//     · 识别：单元格是否本身就是 Excel 原生的结构化时间（QDate/QDateTime/QTime）。
//     · 解析（输入侧 U）：把字符串 / 纪元秒解析成内存里的结构化时间。
//     · 序列化（输出侧 V）：把结构化时间格式化为目标侧的物理表示（格式串文本 或 纪元秒整数）。
//
// 【为什么独立成一层（不放进普通 validator）】
//   时间列两侧（Excel 侧 / DB 侧）各有自己的物理类型与格式（见 ProfileSpec 的 TemporalSideSpec），
//   导入要「按 Excel 侧解析 → 按 DB 侧写」，导出要反过来；且支持 fallback 多格式、纪元秒等。
//   这套「两侧解析/序列化」逻辑比单值校验复杂，故抽成独立转换层，由 Mapper 调度，
//   而 validator 只保留遗留的 date:fmt 简单路径（见 Mapper.h「与时间转换层的分工」）。
//
// 【内存中间表示（关键直觉）】
//   时间值在内存里统一是结构化的 QDate/QDateTime/QTime。两侧转换都经过它：
//     导入：Excel字符串/纪元秒 --toStructured(U)--> 结构化 --formatValue(V)--> DB 表示
//     导出：DB字符串/纪元秒     --toStructured(U)--> 结构化 --formatValue(V)--> Excel 表示
//   （U=输入侧规格，V=输出侧规格；具体哪侧充当 U/V 由调用方 Mapper 按方向决定。）
//
// 【协作者】
//   · ProfileSpec.h —— TemporalSlotKind（Date/DateTime/Time/None）、TemporalSideSpec
//                      （type=String/EpochSec、format、fallback）是本层的输入配置。
//   · Mapper.cpp    —— 唯一调度方：判空→识别→（解析）→序列化，串起整套转换。
//   · Errors.h      —— 解析失败回报 E_TIME_PARSE（导入）/ E_TIME_PARSE_DB（导出）。
//
// 【命名空间】dbridge::detail::tconv —— 库内部、时间转换专用子命名空间。
// ============================================================================

namespace dbridge::detail::tconv {

// isEmptyForTemporal —— 判断一个值是否应被当作「无时间值」。
// True if the value should be treated as "no temporal value" — null QVariant
// or an empty/whitespace-only string. Numeric zero is NOT empty.
// 译：当值为 null（无效 QVariant）或「空 / 纯空白字符串」时返回 true（视作无时间值，应写 NULL）。
//     特别注意：数值 0 不算空——纪元秒 0（1970-01-01T00:00:00Z）是一个合法时间，不能误判为空。
// 副作用：无（纯函数）。
bool isEmptyForTemporal(const QVariant& v);

// isStructuredTemporal —— 判断值是否已是「与所求种类相符」的结构化时间。
// True if the value is already a structured QDate / QDateTime / QTime,
// matching the kind requested. None matches no structured value.
// 译：当 v 已经是结构化时间、且其类型与请求的 kind 相符时返回 true：
//     Date↔QDate、DateTime↔QDateTime、Time↔QTime；kind=None 永远返回 false。
//     用途：Excel 原生日期单元格无需走「字符串解析」，可直接进入序列化（见 Mapper 情形 A）。
// 副作用：无（纯函数）。
bool isStructuredTemporal(const QVariant& v, TemporalSlotKind kind);

// ParseResult —— parseString 的返回：是否成功、解析出的结构化值、命中的格式串。
struct ParseResult {
    bool ok = false;  // 是否成功解析出有效时间
    QVariant value;   // QDate / QDateTime / QTime on success
                     // 译：成功时为对应种类的结构化时间；失败时为无效 QVariant
    QString
        triedFormat;  // format string that succeeded, if any
                      // 译：成功时记录「最终命中的那个格式串」（主格式或某个 fallback），便于诊断
};

// parseString —— 用「主格式 + 一组 fallback 格式」依次尝试解析字符串为结构化时间。
// Parse a string into the requested kind using `primary`; on failure, try each
// `fallback` in order. Returns the first successful parse. Empty `primary` →
// the function still tries fallbacks; if both are empty the result is ok=false.
// 译：先用 primary 解析；失败则按顺序逐个试 fallback，返回首个成功的结果。
//     primary 为空时仍会尝试 fallback；若两者都为空，则无任何格式可试 → ok=false。
// 参数：s=待解析文本；kind=目标种类（决定用 QDate/QDateTime/QTime::fromString）；
//       primary=首选格式；fallback=备选格式列表（按序尝试）。
// 返回：ParseResult（见上）。失败模式不直接产错误码，由上层 toStructured 统一映射。
// 复杂度：O(格式个数)，每个格式一次 fromString 尝试，命中即止。副作用：无（纯函数）。
ParseResult parseString(const QString& s, TemporalSlotKind kind, const QString& primary,
                        const QStringList& fallback);

// formatValue —— 把「结构化时间」序列化为目标侧（side）的物理表示。
// Format a structured temporal QVariant to a target side value.
// type=string: returns QVariant(QString) using Qt format string.
// type=epochSec: returns QVariant(qlonglong) from QDateTime::toSecsSinceEpoch().
// Failure or wrong kind: returns QVariant() (invalid).
// 译：把结构化时间按目标侧规格序列化：
//       side.type=string   → 用 side.format 格式串得到 QVariant(QString)；
//       side.type=epochSec → 由 QDateTime::toSecsSinceEpoch() 得到 QVariant(qlonglong)。
//     当 structured 的类型与 kind 不符、或 epochSec 用于非 DateTime 槽等情况 → 返回无效
//     QVariant()。
// 参数：structured=待序列化的结构化时间；kind=时间槽种类；side=目标侧规格（type/format）。
// 返回：序列化结果；不可序列化时返回无效 QVariant()（上层据此退化为写 NULL，不报错）。
// 副作用：无（纯函数）。
QVariant formatValue(const QVariant& structured, TemporalSlotKind kind,
                     const TemporalSideSpec& side);

// toStructured —— 把「来源侧（side）原始值」解析为内存里的结构化 QDate/QDateTime/QTime。
// Parse raw value (from source side) to structured QDate/QDateTime/QTime.
// type=string: calls parseString; type=epochSec: calls QDateTime::fromSecsSinceEpoch.
// On failure sets errCode/errMsg and returns QVariant() (invalid).
// 译：把来源侧原始值解析为结构化时间：
//       side.type=string   → 转调 parseString（primary=side.format，fallback=side.fallback）；
//       side.type=epochSec → 把整数秒交给 QDateTime::fromSecsSinceEpoch。
//     失败时设置 *errCode/*errMsg 并返回无效 QVariant()。
// 参数：raw=来源侧原始值；kind=种类；side=来源侧规格；errCode/errMsg=出参（失败诊断）。
// 返回：结构化时间；失败返回无效 QVariant()。
// 错误模式：E_TIME_PARSE（解析失败 / 纪元秒非整数 / 纪元秒超范围）——见 Errors.h。
// 副作用：失败时写 *errCode/*errMsg；否则无。纯解析、不碰数据库。
QVariant toStructured(const QVariant& raw, TemporalSlotKind kind, const TemporalSideSpec& side,
                      QString* errCode, QString* errMsg);

}  // namespace dbridge::detail::tconv
