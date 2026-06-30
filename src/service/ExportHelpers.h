#pragma once
#include <QSet>
#include <QStringList>

// ============================================================================
// ExportHelpers.h — 导出方向的纯函数小工具（目前仅“列顺序重排”）
// ============================================================================
//
// 【背景：columnOrder（导出列顺序）】
//   导出时，SELECT 查询天然产出的列顺序称为“自然列序”（natural）。Profile 可选地
//   提供 exportSpec.columnOrder，让用户显式指定一部分列“排在前面、按什么次序”。
//   这里把两者合并成“最终列序”，规则见 reorderHeaders。
//   错误码 E_EXPORT_*（未知表头/重复项/与原生 SQL 不兼容）的校验在别处（ProfileValidator）
//   完成；本文件只负责“顺序合并”这一步纯计算，不做校验、不读库。
//
// 头文件内联函数：无状态、无副作用，可被 ExportService 直接包含使用。
// ============================================================================

namespace dbridge::detail {

// Produce the final column sequence: listed headers first (in columnOrder), then unlisted
// headers from `natural` in their original relative order.
// 译：产出最终列序——columnOrder 中“点名”的表头排在最前（且严格按 columnOrder 给定的
//     次序）；其余未点名的表头，按它们在 natural（自然列序）中的相对先后追加在后面。
//
// 参数：
//   natural     —— 自然列序（SELECT 结果字段名顺序，或已合并 A-header 的有效列序）。
//   columnOrder —— 用户指定的优先列序；为空表示“不重排”。
// 返回：合并后的最终列序。
// 复杂度：O(n)（QSet 查表 + 一次遍历）。无副作用。
inline QStringList reorderHeaders(const QStringList& natural, const QStringList& columnOrder) {
    // 未指定列顺序 → 原样返回自然列序（最常见的快路径）。
    if (columnOrder.isEmpty())
        return natural;

    // listed：columnOrder 里所有被点名的列，转成集合用于 O(1) 判定“是否已点名”。
    QSet<QString> listed = QSet<QString>::fromList(columnOrder);
    // 结果先放“被点名的列”，严格保留 columnOrder 的次序。
    QStringList result = columnOrder;
    // 再把自然列序中“未被点名”的列按原相对顺序补到后面（不丢列）。
    for (const QString& h : natural) {
        if (!listed.contains(h))
            result.append(h);
    }
    return result;
}

}  // namespace dbridge::detail
