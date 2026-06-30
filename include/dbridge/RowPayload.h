#pragma once
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

// ============================================================================
// RowPayload.h — Excel 导入管线（ETL）的“一行数据”内部表示
// ============================================================================
//
// 【背景：什么是“路由 + 多表写入”】
//   dbridge 的 Excel 导入并非简单地把一行写进一张表。一张 Excel 表里的一行，
//   按 Profile 配置的“路由规则”可能被拆分、映射、写入到多张数据库表中
//   （例如一行订单数据，既写 orders 表，又写 order_items 表）。
//   因此“一行 Excel” = 一个 RowContext，内部含有若干 RoutePayload（每个对应
//   一张目标表的一次写入）。
//
// 【这两个结构在管线中的位置】
//   Excel 读取 → 解析成 RowContext（含多个 RoutePayload）→ 校验/类型转换/外键查找
//   → fkInject（把查到的父表主键注入子表）→ UPSERT 写库。
//   它们是 ImportService 与 Mapper 之间传递的“工作单元”，属内部细节，
//   故放在 dbridge::detail 命名空间。dryRun 模式下也会原样回报给调用方观察。
// ============================================================================

namespace dbridge::detail {

// RoutePayload —— 一行数据“写入某一张目标表”所需的全部材料。
// 一个 RouteContext（下面的 RowContext）里有 N 个 RoutePayload = 写 N 张表。
struct RoutePayload {
    QString table;     // 目标数据库表名
    QString routeKey;  // 路由键，标识这条路由；混合场景形如 "orders" 或 "A:orders"
    QVector<QString> dbColumns;  // 本次写入涉及的数据库列名（与 binds 一一对应）
    QVector<QVariant> binds;     // 各列待写入的值（与 dbColumns 同序、同长）
    QStringList conflictKey;  // UPSERT 的冲突键列（唯一约束）：决定“插入还是更新”
    QVector<QVariant> conflictVals;  // 冲突键各列对应的值
    // H-01 fix：当本路由发生校验错误或时间转换错误时，由 Mapper 置位。
    // 这样 ImportService 只需把受影响的 routeIndex 加入 failedRouteIndices，
    // 而不必把整行标记为 hasNonRouteError（其它无关路由仍可正常写入）。
    bool hasError = false;

    // 在 dbColumns 中查找某列的下标；找不到返回 -1。
    // 用于按列名定位其在 binds 中对应的值。
    int indexOf(const QString& col) const {
        for (int i = 0; i < dbColumns.size(); ++i) {
            if (dbColumns[i] == col)
                return i;
        }
        return -1;
    }
};

// RowContext —— 一整行 Excel 数据的内部表示（可能对应多张表的多次写入）。
struct RowContext {
    int excelRow = 0;  // 该行在 Excel 中的行号（用于错误定位回报给用户）
    QString classId;   // 该行被判定归属的“类别/路由类”标识
    QVector<RoutePayload> payloads;  // 该行展开后的各路由载荷（每个写一张表）
    // H-04 fix：payloads[] 中“外键注入失败”的下标集合；这些路由的子孙路由在写阶段
    // 也会被一并跳过，而未受影响的兄弟路由仍照常写入（实现“部分成功”的细粒度控制）。
    QSet<int> failedRouteIndices;
    // M-04 fix：当本行存在“非路由局部”的错误（结构性/类型/无法绑定等）时置为 true。
    // 一旦置位，即便 failedRouteIndices 非空，写阶段也会跳过整行——因为载荷数据本身
    // 已不可用（区别于“只是某条路由的外键注入失败”这种局部问题）。
    bool hasNonRouteError = false;
};

}  // namespace dbridge::detail
