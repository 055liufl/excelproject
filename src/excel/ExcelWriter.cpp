#include "ExcelWriter.h"

#include <memory>
#include <xlsxdocument.h>

// ============================================================================
// ExcelWriter.cpp — .xlsx「写出」薄封装的实现（QXlsx::Document 落地）
// ============================================================================
//
// 【本文件职责】
//   实现 ExcelWriter.h 声明的 4 个动作（open/writeHeader/writeRow/save），把它们
//   翻译成对第三方库 QXlsx 的调用。这里是整个导出管线里**唯一直接接触 QXlsx 的
//   翻译单元**——其它代码只通过 ExcelWriter 的窄接口写 .xlsx。
//
// 【QXlsx 的心智模型（建立直觉，看懂下面代码的前提）】
//   · QXlsx::Document 代表“一整本工作簿”，在内存里攒好所有单元格，最后用
//     saveAs(path) 一次性序列化成磁盘上的 .xlsx 文件（写盘只发生一次）。
//   · 新建的 Document 默认就**自带一张名为 "Sheet1" 的工作表**（重要：见 open）。
//   · 多表时用 addSheet(name) 增表、selectSheet(name) 切换“当前活动表”；
//     之后的 write 都作用在“当前活动表”上。
//   · 写格子：doc->write(row, col, value)，行列均 **1 基**（左上角是 (1,1)）。
//     value 是 QVariant，QXlsx 据其动态类型自动决定单元格类型（数值/文本/日期…），
//     这正是本封装“QVariant → 单元格”免做手工类型分发的原因。
//
// 【Pimpl 落地】QXlsx 的头文件 <xlsxdocument.h> 只在本 .cpp 里 #include；头文件中
//   仅前置声明的 struct Impl，在此给出完整定义，从而把 QXlsx 依赖关在 .cpp 内。
//
// 【错误处理风格】open/save 返回 bool 并通过 QString* err 出参回传原因；调用方
//   （ExportService）再把它包成表级错误码 err::E_WRITE_XLSX（见 include/dbridge/Errors.h）。
//   writeHeader/writeRow 不返回错误（QXlsx 内存写入不失败），错误只可能在收尾 save 暴露。
// ============================================================================

namespace dbridge::detail {

// ── Pimpl 实现体 ─────────────────────────────────────────────────────────────
// Impl 持有真正的 QXlsx 状态。把它定义在 .cpp，使头文件无须见到 QXlsx 任何符号。
struct ExcelWriter::Impl {
    std::unique_ptr<QXlsx::Document> doc;  // 内存中的工作簿；save() 时 saveAs 到磁盘
    QString sheet;                         // 当前写入的目标工作表名（open 时设定）
};

// 构造：仅创建空的 Impl（此时 doc 还是空指针，尚未建文档）。建文档推迟到 open()。
ExcelWriter::ExcelWriter() : impl_(std::make_unique<Impl>()) {
}

// 析构：=default。必须写在 .cpp（而非 .h 内联）——因为 unique_ptr<Impl> 的析构需要
// Impl 的**完整定义**才能正确销毁；而 Impl 的完整定义只在本文件可见。若放在头里
// 用默认析构，编译器在 Impl 仍是不完整类型处生成析构，会报错。这是 Pimpl 的固定套路。
ExcelWriter::~ExcelWriter() = default;

// ── open：新建文档并准备好目标工作表 ─────────────────────────────────────────
bool ExcelWriter::open(const QString& xlsxPath, const QString& sheetName, QString* err) {
    path_ = xlsxPath;  // 仅记下路径，真正写盘在 save() 的 saveAs(path_)
    rowCursor_ = 1;    // 行游标复位：本次写出从第 1 行（表头）开始
    impl_->doc = std::make_unique<QXlsx::Document>();  // empty doc; saveAs writes path_
                                                       // 译：新建空工作簿；保存时写到 path_
    impl_->sheet = sheetName;
    // Fresh QXlsx::Document already contains "Sheet1"; rename it if user wants
    // a different sheet name, otherwise addSheet+selectSheet.
    // 译：新建的 QXlsx::Document 已自带名为 "Sheet1" 的工作表。
    //     若用户想要别的表名，就（这里采取的策略是）新增一张该名字的表再选中它；
    //     否则直接选中既有同名表即可。
    QStringList names = impl_->doc->sheetNames();  // 取当前已有的工作表名清单
    if (!names.contains(sheetName)) {
        // 不存在该名字的表 → 新增一张。注意：这里**没有删除**默认的 "Sheet1"，
        // 因此当 sheetName != "Sheet1" 时，输出文件会同时含有 "Sheet1"（空）与目标表。
        // 这是当前实现的既有行为（薄封装不替调用方做清理决策）。
        if (!impl_->doc->addSheet(sheetName)) {
            // 新增失败（如表名非法）：填写失败原因并返回 false，open 失败。
            if (err)
                *err = QStringLiteral("Failed to add sheet: ") + sheetName;
            return false;
        }
    }
    // 把目标表设为“当前活动表”：此后所有 write(row,col,…) 都落在这张表上。
    if (!impl_->doc->selectSheet(sheetName)) {
        if (err)
            *err = QStringLiteral("Failed to select sheet: ") + sheetName;
        return false;
    }
    return true;  // 文档已建、目标表已选中，可以开始写
}

// ── writeHeader：把表头写入当前行，并把游标推到下一行 ─────────────────────────
void ExcelWriter::writeHeader(const QStringList& headers) {
    for (int i = 0; i < headers.size(); ++i) {
        // 列号 i + 1 ：QXlsx 列为 1 基，故第 0 个表头写到第 1 列。
        // 行号 rowCursor_ ：首次调用即第 1 行（表头行）。
        impl_->doc->write(rowCursor_, i + 1, headers[i]);
    }
    ++rowCursor_;  // 表头占掉一行，游标下移；后续数据行从下一行开始
}

// ── writeRow：把一行 QVariant 值写入当前行（null 跳过 = 留空单元格）────────────
void ExcelWriter::writeRow(const QVector<QVariant>& row) {
    for (int i = 0; i < row.size(); ++i) {
        if (!row[i].isNull()) {
            // 仅写非空值；null 值不调用 write，对应单元格保持“空”（NULL 语义）。
            // QXlsx 依据 row[i] 的实际 QVariant 类型自动选单元格类型（数字/文本/日期等），
            // 无需在此手工区分 int/double/QString/QDate……（QVariant → 单元格 的便利所在）。
            impl_->doc->write(rowCursor_, i + 1, row[i]);
        }
    }
    ++rowCursor_;  // 无论本行写了几格（甚至全空），都消费一行，保证行号与数据行一一对应
}

// ── save：把整本工作簿一次性序列化为 .xlsx 文件 ──────────────────────────────
bool ExcelWriter::save(QString* err) {
    if (!impl_->doc->saveAs(path_)) {
        // saveAs 失败（路径不可写 / 目录不存在 / 磁盘满 等）：回传原因，返回 false。
        // 调用方据此记一条表级错误（err::E_WRITE_XLSX）。
        if (err)
            *err = QStringLiteral("Failed to save xlsx: ") + path_;
        return false;
    }
    return true;  // 文件已落盘
}

}  // namespace dbridge::detail
