#pragma once
#include "dbridge/Types.h"

#include <QList>

// ============================================================================
// ErrorCollector.h — 导入/导出流水线的“错误与警告”累加器
// ============================================================================
//
// 【这个类是什么】
//   ImportService / ExportService 在整条 ETL 管线里会发现各式各样的问题（打不开
//   文件、列校验失败、外键查无此项、写库失败……）。ErrorCollector 就是这些问题的
//   统一收集箱：每发现一处，就 append 一条 RowError（载体定义见 Types.h）。
//   流水线全程持有同一个 ErrorCollector，最后把它整体倒进 ImportResult/ExportResult。
//
// 【两条独立的列表：errors_ 与 warnings_】
//   · errors_   —— 阻断性错误（导致行被跳过，或整个操作中止）。
//   · warnings_ —— 非阻断诊断（流程照常进行，只是给调用方一个提示，
//                   如 W_TIME_ORDERBY_NONSORTABLE）。
//   关键：把两者拆成两个 QList，是为了让“if (有错) then 中止”这类既有调用方
//   的判断逻辑保持原意——它们只看 errors_，不会被警告误伤。
//
// 【行级 / 表级的约定】
//   RowError.row == 0 且 column 为空 → “表级错误”（与具体某一行无关，例如打不开
//   文件、整张 sheet 的预取查询失败）。row > 0 → “行级错误”（定位到具体 Excel 行）。
//   addTable()/addTableWarning() 就是 add()/addWarning() 在 row=0、column/raw 为空
//   时的便捷封装。
//
// 【线程模型】无内部加锁；约定在搬运工作线程内单线程使用。需要跨线程读取时，
//   由上层（如 IBatchTransfer 实现）在外部加锁拷贝快照。
// ============================================================================

namespace dbridge::detail {

class ErrorCollector {
   public:
    // 追加一条“阻断性错误”。
    // sheet/row/column/rawValue 用于把问题精确定位回用户的 Excel（哪张表、第几行、
    //   哪一列、触发问题的原始文本）；code 取自 Errors.h 的码字典；message 为人类可读说明。
    void add(const QString& sheet, int row, const QString& column, const QString& rawValue,
             const QString& code, const QString& message);

    // 追加一条“表级”阻断性错误（与具体行无关）：等价于 add(sheet, 0, "", "", code, message)。
    void addTable(const QString& sheet, const QString& code, const QString& message);

    // Non-blocking warnings (e.g. W_TIME_ORDERBY_NONSORTABLE). Stored in a separate
    // list so the existing "if errors then abort" callers behave unchanged.
    // 译：非阻断警告（如 W_TIME_ORDERBY_NONSORTABLE）。存放在独立的 warnings_ 列表里，
    //     这样既有“若 errors 非空则中止”的调用方行为不受影响（它们只看 errors_）。
    void addWarning(const QString& sheet, int row, const QString& column, const QString& rawValue,
                    const QString& code, const QString& message);
    // 追加一条“表级”非阻断警告。
    void addTableWarning(const QString& sheet, const QString& code, const QString& message);

    // 是否“没有阻断性错误”。注意：只看 errors_，与 warnings_ 无关。
    bool empty() const {
        return errors_.isEmpty();
    }
    // 只读访问错误列表（用于最终倒进 result.errors）。
    const QList<RowError>& list() const {
        return errors_;
    }
    // 可写访问错误列表（极少用；保留以便就地修改）。
    QList<RowError>& list() {
        return errors_;
    }
    // 只读访问警告列表（用于最终倒进 result.warnings）。
    const QList<RowError>& warnings() const {
        return warnings_;
    }
    // 可写访问警告列表。
    QList<RowError>& warnings() {
        return warnings_;
    }

    // 清空两条列表（复用同一个收集器跑多趟时使用）。
    void clear() {
        errors_.clear();
        warnings_.clear();
    }

   private:
    QList<RowError> errors_;    // 阻断性错误累加列表
    QList<RowError> warnings_;  // 非阻断警告累加列表
};

}  // namespace dbridge::detail
