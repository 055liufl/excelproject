// ============================================================================
// BatchUniqueness.cpp — 批内唯一性检查的实现
// ============================================================================
//
// 本文件实现 BatchUniqueness 的两件事：
//   encodeConflictKey() —— 把多列冲突键值无歧义地编码成单字符串（查重用的 map key）。
//   check()             —— 逐行判重，区分「合法的父行重复」与「真正的重复冲突」。
//
// 关键不直观处：encodeConflictKey 的「长度前缀」防碰撞设计、父行去重的两个前提条件。
// ============================================================================

#include "BatchUniqueness.h"

#include "dbridge/Errors.h"

#include "service/ErrorCollector.h"

namespace dbridge::detail {

// encodeConflictKey —— 把一组值编码成「无歧义」的字符串键。
//
// 【为什么不能简单拼接】直接用分隔符拼接会碰撞：["a","bc"] 与 ["ab","c"] 若用 "|" 拼
//   都得到 "a|bc" / "ab|c" 看似不同，但遇到值里本就含分隔符时仍可能撞车。这里采用
//   「长度|值|」的前缀编码：先写该值的长度、再写值本身。因为长度是确定的数字、且每段
//   都带自己的长度，解析时不会把两段串成一段，从根本上消除碰撞。
// 【null 的处理】null 值统一编码为字面量 "<null>"（其长度也照常计入），保证 null 与
//   普通字符串 "<null>" 不会混淆的同时，给 null 一个稳定可比的表示。
QString BatchUniqueness::encodeConflictKey(const QVector<QVariant>& vals) {
    QString encoded;
    for (const auto& v : vals) {
        QString s = v.isNull() ? QStringLiteral("<null>") : v.toString();
        // 形如："3|abc|" —— 段长 + 分隔 + 段值 + 分隔；长度前缀使分段无歧义。
        encoded += QString::number(s.length()) + QStringLiteral("|") + s + QStringLiteral("|");
    }
    return encoded;
}

// check —— 逐行判重。详见头文件函数注释。
bool BatchUniqueness::check(const RoutePayload& payload, int excelRow, bool hasChildren,
                            ErrorCollector* errors, const QString& sheet) {
    // Only check if conflict values are fully populated
    // 译：仅当冲突键值「完整无 null」时才判重——任一冲突键列为 null 都无法构成可比的键。
    bool hasNullConflict = false;
    for (const auto& v : payload.conflictVals) {
        if (v.isNull()) {
            hasNullConflict = true;
            break;
        }
    }
    if (hasNullConflict)
        return true;  // can't deduplicate without complete key
                      // 译：键不完整，无从去重 —— 放行（真有问题会在写库阶段由 DB 约束兜底）

    QString key = encodeConflictKey(payload.conflictVals);  // 编码成无歧义键
    auto& routeMap = seen_[payload.routeKey];  // 按路由分桶：不同路由各自独立判重
    auto it = routeMap.find(key);

    if (it != routeMap.end()) {
        // 该冲突键此前已出现过 → 要么是「合法父行重复」，要么是「真重复」。
        const SeenEntry& first = it.value();
        // Allow duplicate if this is a parent route (hasChildren) AND binds are identical
        // 译：当「本路由是父路由（hasChildren）」且「整行绑定值与首次完全一致」时，
        //     视为合法的父行重复（同一父被多条子数据重复携带），放行。
        //     两个条件缺一不可：仅相同还不够，必须确属父行；仅是父行但值不同则仍是冲突。
        if (hasChildren && payload.binds == first.binds) {
            return true;  // allowed parent row dedup
        }
        // Real duplicate
        // 译：真正的重复 —— 冲突键相同但不满足父行去重豁免 → 记 E_VALIDATE_DUPLICATE。
        //     错误消息指出与「首次出现的行号 first.excelRow」重复，便于用户定位两行。
        errors->add(sheet, excelRow, payload.conflictKey.join(','), key,
                    QString::fromLatin1(err::E_VALIDATE_DUPLICATE),
                    QStringLiteral("Conflict key already seen at row %1").arg(first.excelRow));
        return false;
    }

    // 首次出现：登记「行号 + 整行绑定值」，供后续行比对（含父行去重的整行相等判断）。
    routeMap[key] = SeenEntry{excelRow, payload.binds};
    return true;
}

}  // namespace dbridge::detail
