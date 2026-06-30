#pragma once
#include <QSqlDatabase>
#include <QVector>

#include "mapping/RowPayload.h"
#include "profile/ProfileSpec.h"
#include <functional>

// ============================================================================
// ForeignKeyPreflight.h — 写库前的「外键预检」：批量确认父表记录确实存在
// ============================================================================
//
// 【单一职责】
//   在真正 UPSERT 写库之前，对所有「带 fkInject 的子表载荷」逐一确认：它引用的
//   父表那一行是否真的存在（要么在本批次里同时写入、要么早已存在于数据库）。
//   不存在则记 E_VALIDATE_FK 错误，并把该路由标记为失败——这样写阶段能「只跳过
//   受影响的路由」而非整行（细粒度的部分成功，见下方 H-02 fix）。
//
// 【为什么叫 Preflight（预检 / 起飞前检查）】
//   外键约束本可交给数据库在 INSERT 时报错，但那样：① 错误信息含糊、难定位到 Excel 行；
//   ② 在 abortOnError 语义下会因单条约束失败而中断整批。预检在写库前主动批量探测，
//   能给出「第几行、哪几列、引用的父键是什么、在哪张父表找不到」的精确诊断。
//
// 【在 ETL 流水线中的位置】
//   Mapper.map() → FkInjector（注入父表主键到子表）→ BatchUniqueness（批内查重）
//     → 【ForeignKeyPreflight.check()  ← 本类】→ UPSERT 写库。
//   它在 fkInject 之后跑——此时子表载荷里的外键列值已注入完毕，才有得可查。
//
// 【两条「省去 SQL 探测」的优化（详见 .cpp）】
//   1) 批内命中：父行就在本批待写集合里（同次导入的另一行）→ 视为存在，免查库。
//   2) lookup 派生：父键来自父路由的 lookup select（已在 prefetch 阶段校验过存在性）
//      → 整组免查库。
//
// 【协作者】
//   · RowPayload.h    —— RowContext/RoutePayload（被检查与被回写 failedRouteIndices 的对象）。
//   · ProfileSpec.h   —— RouteSpec/FkInjectSpec/LookupSpec（外键依赖的配置来源）。
//   · ErrorCollector  —— 记录 E_VALIDATE_FK 行级错误。
//   · SqlBuilder      —— quoteIdent 安全转义表名/列名（见 H-05 fix）。
//   · Errors.h        —— 错误码 E_VALIDATE_FK。
//
// 【命名空间】dbridge::detail —— 库内部实现细节。
// ============================================================================

namespace dbridge::detail {

class ErrorCollector;

class ForeignKeyPreflight {
   public:
    // §7.7 Optional probe counter hook — called each time a DB probe is actually executed.
    // Inject a counting lambda in tests; leave nullptr for production (noop).
    // 译：可选的「探测计数」钩子——每当真正向数据库发起一次 FK 存在性探测时被回调。
    //     测试里注入一个计数 lambda 以断言「省查优化」确实生效（即探测次数符合预期）；
    //     生产环境保持 nullptr（不回调，零开销）。
    std::function<void(const QString& table)> onProbe;

    // check —— 对一批行的所有「带 fkInject 的子表载荷」做外键存在性预检。
    //
    // Check that referenced parent rows exist in DB for payloads that have fkInject.
    // Parent rows that are present in the same batch are considered OK.
    // 译：确认每个带 fkInject 的载荷所引用的父行在数据库中存在；若父行就在同一批次内
    //     待写（本次导入的另一行），同样视为通过（无需它已落库）。
    //
    // H-02 fix: contexts is non-const so that failed route indices can be written back into
    // ctx.failedRouteIndices, enabling the write phase to skip only affected payloads instead
    // of the entire row.
    // 译：H-02 修复——contexts 取非 const 引用，以便把「预检失败的路由下标」回写进
    //     ctx.failedRouteIndices；这样写阶段只跳过受影响的那条路由（及其子孙），
    //     而非把整行一并丢弃（实现细粒度「部分成功」）。
    //
    // 参数：
    //   contexts  —— 本批所有行（非 const：会回写 failedRouteIndices）。
    //   allRoutes —— 全部路由规格（用于按表名反查 RouteSpec、判断有无 fkInject）。
    //   db        —— 数据库连接（探测父行存在性时执行 SELECT）。
    //   sheet     —— 工作表名（写入错误条目以便定位）。
    //   errors    —— 出参：记录 E_VALIDATE_FK 错误。
    // 返回：全部通过 true；任一外键预检失败 false（失败处已记错误并回写 failedRouteIndices）。
    // 副作用：可能向 db 发起只读 SELECT 探测；向 *errors 追加错误；回写 contexts。
    // 错误模式：E_VALIDATE_FK——父行不存在，或探测 SQL 执行失败（消息区分两者）。
    bool check(QVector<RowContext>& contexts, const QVector<RouteSpec>& allRoutes, QSqlDatabase& db,
               const QString& sheet, ErrorCollector* errors);

   private:
    // checkPayload —— 对「单个子表载荷」逐条 fkInject 规则做预检。
    //   流程：① 若整组父键来自父路由 lookup → 跳过（已在 prefetch 校验，§7.5）；
    //         ② 组装子侧外键元组，遇缺列/含 null → 跳过（错误由上游负责）；
    //         ③ 先在「本批父载荷」里找匹配 → 命中即过，免查库；
    //         ④ 未命中再向 db 发 `SELECT 1 ... WHERE 各父列=? LIMIT 1` 探测。
    //   返回：本载荷全部外键都通过 true；否则 false（已记 E_VALIDATE_FK）。
    bool checkPayload(const RoutePayload& payload, const RouteSpec& routeSpec,
                      const QHash<QString, QVector<RoutePayload>>& batchParentPayloads,
                      const QHash<QString, const RouteSpec*>& routeByTable, QSqlDatabase& db,
                      const QString& sheet, int excelRow, ErrorCollector* errors);
};

}  // namespace dbridge::detail
