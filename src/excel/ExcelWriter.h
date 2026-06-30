#pragma once
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <memory>

// ============================================================================
// ExcelWriter.h — .xlsx「写出」薄封装（基于第三方库 QXlsx）
// ============================================================================
//
// 【这个文件是什么】
//   ExcelWriter 是对 QXlsx::Document 的一层极薄封装：把“创建工作表 → 写表头 →
//   逐行写数据 → 保存 .xlsx”这套固定动作收拢成 4 个方法（open/writeHeader/
//   writeRow/save），让上层导出代码无需直接接触 QXlsx 的细节。
//   它只负责“把已经准备好的格子写进文件”，不含任何业务/映射逻辑。
//
// 【在导出（Export，DB→Excel）管线中的位置】
//   ExportService 决定“写哪些列、按什么顺序、每格放什么值”，本类只管“落盘”：
//
//     SQLite SELECT 取数
//        → ExportService 做：类型/时间格式转换、反向查找、按 columnOrder 重排列
//        → 得到“最终顺序的表头 QStringList + 每行的值 QVector<QVariant>”
//        → ExcelWriter.open(path, sheet)          // 建/选工作表
//        → ExcelWriter.writeHeader(orderedHeaders)// 写第 1 行表头
//        → ExcelWriter.writeRow(rowValues) × N    // 逐行写数据
//        → ExcelWriter.save()                     // 整本保存为 .xlsx
//
//   关键边界：**列顺序（columnOrder）不在本类内处理**。调用方在传入之前就已把
//   表头与每行的值排成最终次序，ExcelWriter 只是“按收到的先后顺序，从第 1 列起
//   依次填格”。这样本类保持无状态业务、可复用。
//
// 【协作者】
//   · ExportService（src/service/ExportService.cpp）—— 唯一调用方；驱动上面 4 步，
//     并把 open/save 的失败翻译成表级错误码 err::E_WRITE_XLSX（见 Errors.h）。
//   · reorderHeaders / ExportHelpers（src/service/ExportHelpers.h）—— 在调用本类之前
//     完成 columnOrder 的列序合并，故本类拿到的就是“成品列序”。
//
// 【依赖】
//   · QXlsx（第三方 .xlsx 读写库）：核心类型 QXlsx::Document，头文件 <xlsxdocument.h>。
//     —— 为避免把 QXlsx 头泄漏到本头文件（减少编译耦合 / 加快增量编译），这里采用
//        **Pimpl（Pointer to IMPLementation）惯用法**：只前置声明 struct Impl，
//        真正持有 QXlsx::Document 的成员藏在 .cpp 的 Impl 定义里。
//        故本头只 #include 了几个 Qt 容器/字符串头，完全不出现任何 QXlsx 符号。
//
// 【命名空间】dbridge::detail —— 标记为“库内部实现细节”，不对外暴露为公共 API。
// ============================================================================

namespace dbridge::detail {

// ── ExcelWriter ──────────────────────────────────────────────────────────────
// 单张工作表的 .xlsx 写出器。典型生命周期：构造 → open → writeHeader → writeRow×N
// → save → 析构。一个实例对应“一个输出文件里的一张工作表”；不可拷贝（持有
// unique_ptr，编译器自动禁用拷贝），按值传递无意义，调用方应持引用/指针使用。
class ExcelWriter {
   public:
    // 构造：仅分配内部 Impl（此时还没有 QXlsx::Document，也未绑定任何文件）。
    // 真正的“建文档/选表”推迟到 open()，符合“构造不做重活、不会失败”的惯例。
    ExcelWriter();

    // 析构：=default 即可——unique_ptr<Impl> 会自动释放 Impl，进而释放其中的
    // QXlsx::Document。注意：析构**不会**自动保存；未调用 save() 的内容会丢失。
    // （析构必须定义在 .cpp 中，因为此处 Impl 仍是不完整类型，详见 .cpp 说明。）
    ~ExcelWriter();

    // open —— 准备好一张可写的工作表。
    //   做什么：新建一个空的 QXlsx::Document，确保其中存在名为 sheetName 的工作表
    //           并将其设为“当前选中表”，随后所有 write 都落在这张表上。
    //   为什么：把“建文档 + 建/选表”这两件易失败的事集中在一处，并通过返回值上报。
    //   参数：
    //     xlsxPath —— 目标 .xlsx 文件路径；此刻只**记下**路径（成员 path_），
    //                 真正写盘发生在 save() 调用的 saveAs(path_)。
    //     sheetName —— 工作表名（如 "Sheet1" / "订单"）。
    //     err       —— 出参；可为 nullptr。失败时若非空，则填入人类可读的失败原因
    //                  （供调用方包装进 E_WRITE_XLSX 的 message）。
    //   返回：true=已就绪可写；false=建表或选表失败（*err 已被填写）。
    //   副作用：分配 QXlsx::Document、把行游标 rowCursor_ 复位为 1、记录 path_。
    //   错误模式：addSheet/selectSheet 失败（如名称非法/重复异常）→ 返回 false。
    //   复杂度：O(已有表数)（内部要在 sheetNames() 里查重）；常数级。
    bool open(const QString& xlsxPath, const QString& sheetName, QString* err);

    // writeHeader —— 把表头写到当前行（首次调用即第 1 行）。
    //   做什么：从第 1 列起，依次把 headers[0], headers[1], … 写入当前行的各列，
    //           写完后行游标 +1（下一次写入自动落到下一行）。
    //   为什么：表头与数据行的写法本质相同，单独成函数只是语义清晰（首行=标题）。
    //   参数：headers —— **已是最终列序**的表头文本列表（列序由调用方负责，见文件头）。
    //   返回：void（QXlsx 写文本不会失败，故无需返回值）。
    //   副作用：写入若干单元格；rowCursor_ 自增 1。
    //   复杂度：O(headers.size())。
    void writeHeader(const QStringList& headers);

    // writeRow —— 写一行数据（QVariant → 单元格）。
    //   做什么：从第 1 列起逐列写 row[i]；**跳过 null 值**——QVariant 为 null 的格子
    //           留空不写（对应导出语义里的“该单元格无值/NULL”，写空而非写 "" 或 0）。
    //           QXlsx 会按 QVariant 的实际类型自动选择单元格类型（数字/文本/日期等）。
    //   为什么：列与 binds 一一对应；null 跳过可保持空单元格语义、也省一次无谓写入。
    //   参数：row —— **已是最终列序**的一行值；下标 i 对应输出第 i+1 列。
    //   返回：void。
    //   副作用：写入非空单元格；无论本行是否全空，rowCursor_ 都自增 1（保留空行占位）。
    //   复杂度：O(row.size())。
    void writeRow(const QVector<QVariant>& row);

    // save —— 把整本工作簿写盘为 .xlsx（落地 open() 记下的 path_）。
    //   做什么：调用 QXlsx::Document::saveAs(path_) 一次性序列化为磁盘文件。
    //   为什么：QXlsx 是“内存里攒好整本、最后一次性保存”的模型，故写盘只在收尾做一次。
    //   参数：err —— 出参；可为 nullptr。失败时若非空则填入失败原因。
    //   返回：true=保存成功；false=saveAs 失败（如路径不可写/磁盘满，*err 已填）。
    //   副作用：在磁盘上创建/覆盖 path_ 文件。
    //   错误模式：失败映射到调用方的表级错误码 E_WRITE_XLSX。
    //   复杂度：O(单元格总数)（序列化整本）。
    bool save(QString* err);

   private:
    // Pimpl：仅前置声明，真正定义在 .cpp。藏起 QXlsx::Document，隔绝其头文件依赖。
    struct Impl;
    // impl_ —— 拥有内部实现（含 QXlsx 文档与当前表名）。
    std::unique_ptr<Impl> impl_;
    // path_ —— open() 记下的目标路径；save() 时交给 saveAs() 写盘。
    QString path_;

    // 行游标：下一次写入要落在第几行（QXlsx 行号 1 基）。初值 1 = 第一行。
    // 每写完一行（writeHeader/writeRow）自增 1，从而表头在第 1 行、数据从第 2 行起。
    int rowCursor_ = 1;
};

}  // namespace dbridge::detail
