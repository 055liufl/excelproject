#include "ErrorCollector.h"

// ============================================================================
// ErrorCollector.cpp — 错误/警告累加器的实现
// ============================================================================
//
// 实现极薄：每个方法只是把入参装进一个 RowError，append 到对应列表末尾。
//   · 复杂度：均为 O(1) 摊还（QList 尾插）。
//   · 副作用：仅修改成员列表，不抛异常、不做 I/O。
//   · 这里不做任何去重/排序：错误“按发现顺序”原样保留，便于用户顺着流水线追溯。
// ============================================================================

namespace dbridge::detail {

// 追加一条阻断性错误：逐字段填充 RowError 后尾插到 errors_。
void ErrorCollector::add(const QString& sheet, int row, const QString& column,
                         const QString& rawValue, const QString& code, const QString& message) {
    RowError e;
    e.sheet = sheet;        // 出错所在工作表
    e.row = row;            // Excel 行号（>0 行级，==0 表级）
    e.column = column;      // 出错列（表头名）；表级错误为空
    e.rawValue = rawValue;  // 触发问题的原始单元格文本，便于排查
    e.code = code;          // 错误码字符串（取值见 Errors.h）
    e.message = message;    // 人类可读详细说明
    errors_.append(e);
}

// 表级错误便捷封装：row 固定 0、column/rawValue 留空，复用 add()。
void ErrorCollector::addTable(const QString& sheet, const QString& code, const QString& message) {
    add(sheet, 0, QString(), QString(), code, message);
}

// 追加一条非阻断警告：结构与 add() 完全相同，区别仅在于落入 warnings_ 而非 errors_。
void ErrorCollector::addWarning(const QString& sheet, int row, const QString& column,
                                const QString& rawValue, const QString& code,
                                const QString& message) {
    RowError e;
    e.sheet = sheet;
    e.row = row;
    e.column = column;
    e.rawValue = rawValue;
    e.code = code;
    e.message = message;
    warnings_.append(e);
}

// 表级警告便捷封装：复用 addWarning()，row 固定 0、column/rawValue 留空。
void ErrorCollector::addTableWarning(const QString& sheet, const QString& code,
                                     const QString& message) {
    addWarning(sheet, 0, QString(), QString(), code, message);
}

}  // namespace dbridge::detail
