#pragma once
#include <QHash>
#include <QString>
#include <QVector>

#include "RowPayload.h"

// ============================================================================
// BatchUniqueness.h — 「批内唯一性」检查：同一次导入内冲突键不得重复
// ============================================================================
//
// 【单一职责】
//   在一次导入的整批数据内，跟踪「每条路由的冲突键值」是否已出现过。若某行的冲突键
//   与本批先前某行重复，则记为 E_VALIDATE_DUPLICATE——避免同一批里两行竞写同一目标
//   记录（数据库的 UNIQUE 约束只在落库时报错，且无法定位到 Excel 行；批内预查更友好）。
//
// 【一个重要例外：父行去重（parent-row dedup）】
//   多表导入中，「同一个父」常被多行子数据重复携带（如多件订单明细都带着同一张订单头）。
//   这种「父路由 + 冲突键相同 + 整行绑定值完全一致」的重复是合法的（它们指的就是同一父行，
//   UPSERT 会幂等合并），不应报重复错。只有「冲突键相同但其它值不同」才是真正的重复冲突。
//
// 【在 ETL 流水线中的位置】
//   Mapper.map() → FkInjector（注入外键，使冲突键值完整）
//     → 【BatchUniqueness.check()  ← 本类：逐行查重】→ ForeignKeyPreflight → 写库。
//   必须在 FkInjector 之后跑：外键往往是冲突键的一部分，注入后冲突键才齐全可比。
//
// 【协作者】
//   · RowPayload.h   —— RoutePayload（提供 routeKey / conflictKey / conflictVals / binds）。
//   · ErrorCollector —— 记录 E_VALIDATE_DUPLICATE。
//   · Errors.h       —— 错误码 E_VALIDATE_DUPLICATE。
//
// 【命名空间】dbridge::detail —— 库内部实现细节。
// ============================================================================

namespace dbridge::detail {

class ErrorCollector;

// SeenEntry —— 「某冲突键首次出现」的记录：在哪一行、当时的整行绑定值。
//   excelRow 用于在报重复时指出「与第几行重复」；binds 用于父行去重时比对「是否完全一致」。
struct SeenEntry {
    int excelRow = 0;  // 该冲突键首次出现的 Excel 行号
    QVector<QVariant> binds;  // 首次出现时该路由载荷的全部绑定值（父行去重用于「整行相等」比对）
};

class BatchUniqueness {
   public:
    // reset —— 清空已见记录。每开始一次新的批量导入前调用，避免跨批误判重复。
    void reset() {
        seen_.clear();
    }

    // check —— 检查本载荷的冲突键是否在本批已出现；按规则区分「合法父行重复」与「真重复」。
    //
    // Check if the given payload's conflict key was already seen.
    // If so, check if it's an allowed parent-row duplicate (same binds, has children).
    // Returns false if it's a real duplicate error.
    // 译：检查该载荷的冲突键此前是否已见过；若见过，再判断是否属于「允许的父行重复」
    //     （绑定值完全相同 且 该路由是父路由 hasChildren）。是真重复则返回 false。
    //
    // 参数：
    //   payload     —— 当前路由载荷（读其 routeKey / conflictKey / conflictVals / binds）。
    //   excelRow    —— 当前 Excel 行号（首见时记入 SeenEntry；报错时指明重复来源）。
    //   hasChildren —— 本路由是否为「有子路由的父路由」；仅父路由才允许整行相同的重复。
    //   errors      —— 出参：判定真重复时记 E_VALIDATE_DUPLICATE。
    //   sheet       —— 工作表名（错误定位用）。
    // 返回：通过（首见 / 允许的父行重复 / 冲突键不完整无法判重）→ true；真重复 → false。
    // 副作用：首见时把记录写入 seen_；真重复时向 *errors 追加错误。
    // 错误模式：E_VALIDATE_DUPLICATE —— 冲突键重复且非「允许的父行重复」。
    bool check(const RoutePayload& payload, int excelRow, bool hasChildren, ErrorCollector* errors,
               const QString& sheet);

   private:
    // routeKey -> conflictKeyEncoded -> SeenEntry
    // 译：二级表——按路由分桶（不同路由各自独立判重），桶内以「编码后的冲突键」为键查首见记录。
    QHash<QString, QHash<QString, SeenEntry>> seen_;

    // encodeConflictKey —— 把一组冲突键值编码成「无歧义」的单字符串键（供 QHash 查重）。
    //   关键：采用「长度|值|」分隔，避免不同值拼接后碰撞（如 ["a","bc"] vs ["ab","c"]）。
    static QString encodeConflictKey(const QVector<QVariant>& vals);
};

}  // namespace dbridge::detail
