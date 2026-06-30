#pragma once
#include <QVector>

#include "profile/ProfileSpec.h"

// ============================================================================
// TopoSorter.h — 按外键依赖给路由排「拓扑序」（父表先于子表）
// ============================================================================
//
// 【单一职责】
//   多表写入时，子表引用父表主键，必须「父表先写、子表后写」。本类对一组 RouteSpec
//   按其 parent 关系做拓扑排序（Kahn 算法），输出一个满足「每个表都排在其父表之后」
//   的路由序列；若依赖关系成环（无法定序）则报错。
//
// 【为什么需要】
//   FkInjector 要从已就绪的父载荷取主键注入子载荷、UPSERT 也要先有父行才能满足子表
//   外键约束——这一切都要求按依赖顺序处理路由。拓扑序就是这个「安全写入顺序」。
//
// 【在 ETL 流水线中的位置】
//   Profile 载入后，对各路由（或某类别的路由）排拓扑序，得到写入顺序；其后
//   Mapper/FkInjector/写库阶段都依此顺序处理父先于子。环检测失败属配置错误。
//
// 【协作者】
//   · ProfileSpec.h —— RouteSpec.table / RouteSpec.parent（排序所依据的依赖关系）。
//   · Errors.h      —— 环检测失败回报 E_PROFILE_TOPOLOGY_CYCLE。
//
// 【命名空间】dbridge::detail —— 库内部实现细节。
// ============================================================================

namespace dbridge::detail {

class TopoSorter {
   public:
    // sort —— 对路由按 parent 依赖做拓扑排序（Kahn 算法）。
    //
    // Sort routes by parent dependency (Kahn's algorithm).
    // Returns false and sets *err = E_PROFILE_TOPOLOGY_CYCLE if a cycle is detected.
    // 译：按 parent 依赖排序，使父表恒排在子表之前。检测到环则返回 false 并设
    //     *err = E_PROFILE_TOPOLOGY_CYCLE。
    //
    // 做什么：把每条路由的 table 视为节点、parent→child 视为有向边，跑 Kahn 算法
    //         （反复取「入度为 0」的节点入序、并削减其子节点入度）得到拓扑序，
    //         再按该序重建 RouteSpec 列表写入 *sorted。
    // 参数：
    //   routes —— 待排序的路由集合（空集合直接成功、输出空）。
    //   sorted —— 出参：拓扑序排好的路由列表（父先于子）。
    //   err    —— 出参：父引用未知表、或存在依赖环时写入诊断。
    // 返回：成功 true；引用未知父表 或 存在环 → false（*err 已填）。
    // 错误模式：
    //   · 引用未知父表 —— 写人类可读消息（路由引用了不存在的 parent）。
    //   · 依赖成环     —— E_PROFILE_TOPOLOGY_CYCLE（拓扑序长度 != 路由数即判定有环）。
    // 副作用：写 *sorted、*err。无 I/O、不碰数据库。
    // 复杂度：O(V + E)，V=路由数、E=父子边数（Kahn 算法线性复杂度）。
    bool sort(const QVector<RouteSpec>& routes, QVector<RouteSpec>* sorted, QString* err);
};

}  // namespace dbridge::detail
