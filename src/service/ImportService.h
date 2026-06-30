#pragma once
#include "dbridge/Types.h"

#include <QSqlDatabase>

#include "profile/ProfileSpec.h"
#include "schema/SchemaCatalog.h"
#include <functional>

// ============================================================================
// ImportService.h — Excel→SQLite「导入编排层」的入口声明
// ============================================================================
//
// 【它在整套 ETL 里的位置】
//   ImportService 是导入方向的“总指挥”：它本身不做底层细活（读 xlsx、编译校验器、
//   拓扑排序、外键查找、写 UPSERT 都委托给各专职组件），而是把这些步骤按正确的
//   阶段顺序串成一条完整流水线，并统一收集错误、决定中止/跳过/提交。
//   实现与各阶段的逐行讲解见 ImportService.cpp。
//
// 【一次 run() 的五大阶段（建立直觉，细节见 .cpp）】
//   Phase A   打开 xlsx、选 sheet、读表头；
//   Phase B   Profile 校验 + 编译校验器 + 拓扑排序路由；
//   Phase A.5 外键“正向查找”的批量预取（一次 SELECT 喂满一批，建立查找缓存）；
//   Phase C   逐行映射成 RowContext（多路由载荷）→ 行级校验 → 套用查找结果 → fkInject
//             → 批内唯一性检查；随后做 FK 预检（preflight）；
//   Phase D   在单个事务内，按外键拓扑序对各路由 UPSERT（dryRun 模式跳过本阶段）。
//
// 【关键概念速查】RowContext / RoutePayload（见 include/dbridge/RowPayload.h）；
//   failedRouteIndices / hasNonRouteError（部分成功的细粒度控制）；正向查找；
//   dryRun（只产出载荷不写库）；abortOnError（出错即中止 vs 跳过该行继续）。
// ============================================================================

namespace dbridge::detail {

class ImportService {
   public:
    // §4.11 Optional prefetch query counter hook — called once per actual SELECT batch.
    // Inject a counting lambda in tests; leave nullptr for production (noop).
    // 译：可选的“预取查询计数钩子”——每真正执行一次外键查找的批量 SELECT 就回调一次，
    //     入参是该查找的 identityKey（同一身份的查找会共享一份预取结果，见 .cpp
    //     buildIdentityKey）。测试里注入一个计数 lambda 来断言“批次数/去重是否生效”；
    //     生产环境保持 nullptr 即无操作。
    std::function<void(const QString& identityKey)> onPrefetch;

    // 执行一次完整导入。
    // 参数：
    //   profile           —— ETL 映射配置（路由/列映射/查找/冲突键/导出列序等）。
    //   catalog           —— 数据库表结构目录（列类型/约束），供类型推断与查找定位列。
    //   xlsxPath          —— 待导入的 .xlsx 文件路径。
    //   options           —— 导入选项（sheet 名、dryRun、abortOnError 等，见 Types.h）。
    //   db                —— 已打开的 SQLite 连接；本服务在其上执行查找与写库。
    //   manageTransaction —— 是否由本服务自管事务（begin/commit/rollback）。默认 true；
    //                         当外层（如同步/批处理）已开好事务时传 false，避免事务嵌套。
    // 返回：ImportResult（ok、读/写行数、errors/warnings；dryRun 时附带 dryRunPayloads）。
    // 错误模式：表级错误（如打不开文件、预取 SELECT 失败、拓扑成环）→ 立即返回、不写库；
    //   行级错误 → 受 abortOnError 控制（true 整体中止，false 跳过该行继续）。错误码见 Errors.h。
    // 线程：在调用线程内同步执行；与传入的 db 连接同线程使用（QSqlDatabase 非线程安全）。
    ImportResult run(const ProfileSpec& profile, const SchemaCatalog& catalog,
                     const QString& xlsxPath, const ImportOptions& options, QSqlDatabase& db,
                     bool manageTransaction = true);
};

}  // namespace dbridge::detail
