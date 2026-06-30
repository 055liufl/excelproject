#pragma once
#include <QHash>
#include <QVariant>

#include "profile/ProfileSpec.h"

// ============================================================================
// Router.h — 混合模式（Mixed）下的「行分类器」：按判别列值把行路由到某个类别
// ============================================================================
//
// 【单一职责】
//   仅用于 Profile 的 Mixed 模式。Mixed 模式把一张 Excel 的每一行，按某个「判别列
//   discriminator」的值分派到不同「类别 class」，每个类别再各自拥有一组路由
//   （见 ProfileSpec 的 ClassSpec）。Router 就是这个「判别值 → ClassSpec」的查表器：
//     · init()  —— 从 Mixed Profile 建立 matchEquals → 类别 的索引（并校验判别值唯一）。
//     · match() —— 给定一行的判别值，返回命中的 ClassSpec（未命中返回 nullptr）。
//
// 【在 ETL 流水线中的位置（仅 Mixed 模式）】
//   读 Excel 一行 → Router.match(判别列值) → 得到 ClassSpec
//     → 取该类别的 routes → Mapper.compileValidators/map 按这组路由处理本行。
//   非 Mixed 模式（SingleTable/MultiTable）不经过 Router，直接用顶层 routes。
//
// 【错误模式】
//   · init 阶段：两个类别的 matchEquals 相同（判别值撞车，无法唯一分派）→ 报错。
//   · match 阶段：某行判别值不匹配任何类别 → 返回 nullptr；上游通常据此报
//     E_ROUTE_UNMATCHED（该行无法决定写入哪张表，见 Errors.h）。
//
// 【协作者】
//   · ProfileSpec.h —— ProfileSpec.classes / discriminatorSource / ClassSpec.matchEquals。
//   · Mapper        —— 拿到 ClassSpec 后用其 routes 做单行映射。
//   · Errors.h      —— 关联 E_ROUTE_UNMATCHED（由上游在 match 返回 nullptr 时发出）。
//
// 【命名空间】dbridge::detail —— 库内部实现细节。
// ============================================================================

namespace dbridge::detail {

class Router {
   public:
    // init —— 从一份 Mixed ProfileSpec 初始化路由表。
    // Initialize from a Mixed ProfileSpec.
    // Validates that matchEquals values are unique.
    // 译：从 Mixed Profile 初始化；校验各类别的 matchEquals（判别值）互不重复。
    // 做什么：保存判别列名与类别列表，建立 matchEquals → 类别下标 的哈希索引。
    // 参数：profile=Mixed 模式的 Profile；err=出参，判别值重复时写诊断。
    // 返回：成功 true；存在重复 matchEquals → false（*err 已填）。
    // 副作用：填充内部状态（discriminatorSource_/classes_/matchToClassIdx_）。无 I/O。
    bool init(const ProfileSpec& profile, QString* err);

    // match —— 把一个判别值匹配到对应的类别。
    // Match a discriminator value to a ClassSpec.
    // Returns nullptr if no match found.
    // 译：按等值匹配把判别值映射到 ClassSpec；未命中返回 nullptr。
    // 参数：discriminator=该行判别列的单元格值（null 视为未命中）。
    // 返回：命中的 ClassSpec 指针（指向内部 classes_，生命周期随 Router）；未命中 nullptr。
    // 副作用：无（const 方法）。复杂度：O(1) 哈希查找。
    const ClassSpec* match(const QVariant& discriminator) const;

    // discriminatorSource —— 返回判别列的 Excel 表头名（调用方据此从 reader 取判别值）。
    QString discriminatorSource() const {
        return discriminatorSource_;
    }

   private:
    QString discriminatorSource_;  // 判别列的 Excel 表头名
    QHash<QString, int> matchToClassIdx_;  // matchEquals -> index in classes_（判别值 → 类别下标）
    QVector<ClassSpec> classes_;  // 类别集合（match 返回的指针指向此处元素）
};

}  // namespace dbridge::detail
