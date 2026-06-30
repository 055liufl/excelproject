#pragma once
#include <QSet>
#include <QVector>

#include "RowPayload.h"
#include "profile/ProfileSpec.h"

// ============================================================================
// FkInjector.h — 「外键注入」：把父表载荷的主键值复制进子表载荷的外键列
// ============================================================================
//
// 【单一职责】
//   一行 Excel 经 Mapper 展开成多个 RoutePayload（每张目标表一个）。当子表要引用
//   父表的主键（多表关联写入）时，子表载荷里那几列外键此刻还是空的——父表的主键值
//   要等父表写入后才知道（代理主键），或要从父载荷已有的列里取。FkInjector 负责：
//   按 Profile 的 fkInject 规则，把「父载荷对应列的值」复制到「子载荷的外键列」。
//
// 【在 ETL 流水线中的位置】
//   Mapper.map()（产出各路由载荷）
//     → 【FkInjector.inject()  ← 本类：父键注入子表】
//       → BatchUniqueness（批内查重，此时外键已注入、冲突键才完整）
//         → ForeignKeyPreflight（外键预检，确认父行真的存在）
//           → UPSERT 写库（按 TopoSorter 排好的父→子顺序）。
//   注意：注入要按「父先于子」的依赖关系进行，故依赖路由间的 parent 关系。
//
// 【与 lookup / ForeignKeyPreflight 的区别（关键，勿混淆）】
//   · FkInjector 处理的是「同一份 Profile 内、路由之间」的父子外键（父载荷→子载荷）；
//   · LookupSpec 处理的是「向外部参照表 G 查代理主键」（业务键→代理键），是另一回事；
//   · ForeignKeyPreflight 在注入之后，校验注入好的外键在 DB / 批内是否真有父行。
//
// 【协作者】
//   · RowPayload.h  —— RoutePayload（被读父值、被写子外键列与冲突键值的对象）。
//   · ProfileSpec.h —— RouteSpec.parent / FkInjectSpec.pairs（父子关系与列对的配置来源）。
//   · ErrorCollector—— 记录注入失败（父值为 null / 父子值冲突）的 E_VALIDATE_FK 错误。
//   · Errors.h      —— 错误码 E_VALIDATE_FK。
//
// 【命名空间】dbridge::detail —— 库内部实现细节。
// ============================================================================

namespace dbridge {
struct RowError;
}

namespace dbridge::detail {
class ErrorCollector;
}

namespace dbridge::detail {

class FkInjector {
   public:
    // inject（旧重载，已废弃为「故意失败的桩」）——
    // Inject FK business keys into child payloads from their parent payloads.
    // Modifies payloads in-place. Returns false if a required parent payload is missing.
    // 译：本应「把外键业务键从父载荷注入子载荷、就地修改 payloads」；但此重载是历史遗留桩，
    //     缺少 RouteSpec 上下文（不知父子关系），无法真正注入。调用它必定返回 false 并写 *err，
    //     提示改用下面带 RouteSpec 的重载（见 .cpp 的 L-01 fix）。请勿使用本重载。
    bool inject(QVector<RoutePayload>& payloads, QString* err);

    // inject（正式重载）—— 按路由父子关系与 fkInject 规则，把父键注入各子载荷。
    //
    // 做什么：①建「表名→载荷下标」与「路由→父载荷下标」索引；②对每条带 fkInject 的路由，
    //         若其任一祖先已失败则跳过（失败会沿父子链传播）；否则按 pairs 把父列值写入
    //         子列（缺列则新增列；已有值且冲突则报错），并同步回填该列在冲突键中的值。
    // 为什么：让子表能携带正确的父表主键写库，是多表关联导入的核心一步。
    // 参数：
    //   payloads      —— 本行的全部路由载荷（就地修改：注入外键列/回填冲突键）。
    //   routes        —— 与 payloads 对应的路由规格（提供 parent 与 fkInject）。
    //   excelRow      —— Excel 行号（错误定位用）。
    //   sheet         —— 工作表名（错误定位用）。
    //   errors        —— 出参：注入失败时记 E_VALIDATE_FK。
    //   initialFailed —— 入参的「已失败路由下标集」（如先前校验阶段已失败的路由），
    //                    用于让失败沿父子链向下传播；默认空集。
    // 返回：更新后的「失败路由下标集」（含 initialFailed 与本次新失败的）。调用方据此跳过这些路由。
    // 副作用：就地改 payloads；向 *errors 追加错误。无 I/O、不碰数据库。
    // 错误模式（均 E_VALIDATE_FK，行级、只影响相关路由）：
    //   · 父列值为 NULL —— 无法注入；
    //   · 子列已有非空值且与父值不等 —— 父子外键冲突。
    QSet<int> inject(QVector<RoutePayload>& payloads, const QVector<RouteSpec>& routes,
                     int excelRow, const QString& sheet, ErrorCollector* errors,
                     QSet<int> initialFailed = {});
};

}  // namespace dbridge::detail
