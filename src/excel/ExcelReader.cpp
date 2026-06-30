#include "ExcelReader.h"

#include <memory>
#include <xlsxcellrange.h>
#include <xlsxdocument.h>

// ============================================================================
// ExcelReader.cpp — .xlsx 读取器的实现（QXlsx 用法 + 表头/行列/空值处理）
// ============================================================================
//
// 【这个文件做什么】
//   实现 ExcelReader.h 声明的所有动作。本文件是整个项目里唯一直接 #include QXlsx
//   实现头（xlsxdocument.h / xlsxcellrange.h）的地方——把第三方库的具体用法集中
//   隔离在此，对上层只暴露干净接口（见头文件的 PIMPL 说明）。
//
// 【依赖的 QXlsx 头】
//   · xlsxdocument.h  —— 提供 QXlsx::Document：工作簿对象，是读 .xlsx 的总入口。
//                        本文件用到它的 load()/selectSheet()/dimension()/read()。
//   · xlsxcellrange.h —— 提供 QXlsx::CellRange：一个矩形单元格区域（左上角到右下角）。
//                        Document::dimension() 返回它，用来问「这张表有多大」。
//
// 【贯穿全文件的 QXlsx 心智模型】
//   1) 行列号都是 **1 基**：第 1 行第 1 列写作 read(1, 1)；这与 Excel 界面一致。
//   2) 「当前活动表」：Document 内部记着一张「当前选中的工作表」，selectSheet()
//      切换它，之后 dimension()/read() 都作用在这张当前表上——所以读数据前必须
//      先 selectSheet()，否则读到的是默认（第一张）表。
//   3) 读单元格用 Document::read(row, col)：直接返回 QVariant，省去手动取 Cell。
//   4) 空单元格 / 越界：read() 返回一个「无效或空」的 QVariant，下面用
//      isValid()/isNull() 来识别（详见 readHeader 的表头扫描逻辑）。
// ============================================================================

namespace dbridge::detail {

// ── PIMPL 实现体定义 ────────────────────────────────────────────────────────
// 头文件里 struct Impl 只是前向声明；这里给出它的完整定义。把它放在 .cpp，使得
// QXlsx::Document 这个重型类型只在本翻译单元出现，上层完全看不到 QXlsx。
struct ExcelReader::Impl {
    std::unique_ptr<QXlsx::Document> doc;  // 当前打开的工作簿（open() 后非空）
    QString sheet;  // 当前选中的工作表名（selectSheet() 后记录，便于诊断）
};

// 构造：用 make_unique 分配一个空的 Impl。
// 此刻 impl_->doc 仍是空指针——尚未打开任何文件，必须先调用 open()。
ExcelReader::ExcelReader() : impl_(std::make_unique<Impl>()) {
}

// 析构：= default，但「定义」必须落在这里（.cpp）而非头文件。
// 原因（重申 PIMPL 要点）：impl_ 是 unique_ptr<Impl>，销毁它需要 Impl 的完整定义；
// 而 Impl 直到本文件上方才完整。若把析构留给头里隐式生成，编译器在「Impl 不完整」
// 处尝试销毁就会报错。故在此显式 = default。
ExcelReader::~ExcelReader() = default;

// ── open —— 打开并加载工作簿 ─────────────────────────────────────────────────
// 做什么：用路径构造一个 QXlsx::Document，再调用 load() 真正读盘解析（.xlsx 本质
//         是一个 zip 压缩包，load() 会解压并解析其中的 XML）。
// 为什么分两步（构造 + load）：QXlsx 的 Document 构造只记下路径，load() 才是「执行
//         加载」并返回成败——把构造与加载分开，便于在此处用返回值判断失败。
bool ExcelReader::open(const QString& xlsxPath, QString* err) {
    impl_->doc = std::make_unique<QXlsx::Document>(xlsxPath);
    if (!impl_->doc->load()) {
        // 加载失败：文件不存在 / 不是有效 xlsx / 损坏 / 无读权限等都会走到这。
        // err 可能为 nullptr（调用方不关心原因），故先判空再写。
        if (err)
            *err = QStringLiteral("Failed to open xlsx: ") + xlsxPath;
        return false;  // 对应上层错误码 E_OPEN_XLSX
    }
    return true;
}

// ── selectSheet —— 选中某张工作表 ───────────────────────────────────────────
// 做什么：把 Document 的「当前活动表」切到名为 name 的表；之后的 dimension()/read()
//         都作用在它上面。
// 错误模式：name 不存在于工作簿时 selectSheet() 返回 false，这里据此回报。
bool ExcelReader::selectSheet(const QString& name, QString* err) {
    if (!impl_->doc->selectSheet(name)) {
        if (err)
            *err = QStringLiteral("Sheet not found: ") + name;
        return false;
    }
    impl_->sheet = name;  // 记下当前表名（仅用于后续诊断/日志，读数据不再依赖它）
    return true;
}

// ── readHeader —— 定位表头、读列标题、建立列映射、算出数据行范围 ──────────────
// 这是本类信息量最大的函数，分四步：
//   (1) 复位状态并记下表头行号；
//   (2) 问 QXlsx「这张表有多大」，得到要扫描的最大列数（含「维度缺失」的兜底）；
//   (3) 从第 1 列起向右连续扫描表头行，遇空即停，逐列登记标题与列号；
//   (4) 校验至少读到一个标题，并算出 lastRow_（最后数据行）。
bool ExcelReader::readHeader(int headerRow, QString* err) {
    // (1) 记下表头行号，并清空上次的结果（支持对同一对象重复调用 readHeader）。
    headerRow_ = headerRow;
    headers_.clear();
    sourceToCol_.clear();

    // (2) 求「扫描宽度」maxCol。
    // Real QXlsx exposes the active worksheet dimension as CellRange.
    // When the workbook has no data, dimension is invalid; fall back to a
    // reasonable scan width so callers still get a useful error.
    //   —— 译：真实的 QXlsx 把「当前活动表的数据区域」暴露为一个 CellRange（矩形
    //      区域，含左上/右下角坐标）。当工作簿没有任何数据时，这个 dimension 是
    //      无效的；此时我们退回一个合理的扫描宽度，好让调用方仍能拿到一条有用的
    //      错误（即下面的「No headers found」），而不是因为宽度为 0 直接漏报。
    QXlsx::CellRange dim = impl_->doc->dimension();  // 取当前表的数据矩形区域
    int maxCol = dim.lastColumn();                   // 该区域最右列号（1 基）
    if (maxCol <= 0)
        maxCol = 256;  // fallback for empty/broken dimension metadata
                       // 译：当维度元数据为空/损坏（lastColumn 给出 <=0）时，
                       // 兜底用 256 列宽度去扫——256 是 Excel 早期版本的列数上限，
                       // 取它作为「足够宽」的保守值，确保表头不会因维度缺失而漏读。

    // (3) 扫描表头行：从第 1 列开始，逐列向右读，**遇第一个空单元格立即停止**。
    // 这条「连续、左对齐、中间不留空」的约定，是本读取器对表头形状的核心假设：
    // 表头必须从最左列起连成一片，中间一旦出现空列，就认为表头到此为止。
    for (int col = 1; col <= maxCol; ++col) {
        // 读表头行、第 col 列的单元格。read(row, col) 均为 1 基坐标。
        QVariant v = impl_->doc->read(headerRow, col);
        // 空单元格的两种表现都要拦：isValid()==false（QXlsx 认为该格不存在）或
        // isNull()（值为空）。任一成立即视为「表头到头了」，跳出循环。
        if (!v.isValid() || v.isNull())
            break;
        // 取出标题文本并去首尾空白（Excel 里常有不经意的前后空格）。
        QString hdr = v.toString().trimmed();
        // 去空白后若变成空串，同样视为表头结束（例如该格只含若干空格）。
        if (hdr.isEmpty())
            break;
        headers_.append(hdr);  // 按列序追加到标题列表（第 1 列 → headers_[0]）
        sourceToCol_[hdr] = col;  // 建立「标题 → 1 基列号」映射，供 cellBySource 按名取列
    }

    // (4a) 一个标题都没读到 → 该行为空，判为「无表头」错误。
    // 注意这里把「维度兜底」的价值兑现了：即便 dimension 无效，上面也至少扫了一格，
    // 从而能在这里给出明确的「第 N 行无表头」诊断，而不是悄无声息地返回。
    if (headers_.isEmpty()) {
        if (err)
            *err = QStringLiteral("No headers found in row ") + QString::number(headerRow);
        return false;
    }

    // (4b) 算出最后一个数据行号。dim.lastRow() 是当前表数据区域的最末行（1 基）。
    lastRow_ = dim.lastRow();
    // 防御：若 lastRow 反而小于表头行（数据区异常/只有表头一行/维度怪异），
    // 把它夹到 headerRow_，避免出现 firstDataRow() > lastRow() 时本应为空却出现
    // 负向区间。结果是上层 for(row=firstDataRow; row<=lastRow) 自然不执行（空表体）。
    if (lastRow_ < headerRow_)
        lastRow_ = headerRow_;

    return true;
}

// ── cellBySource —— 按列标题取某行的单元格值 ────────────────────────────────
// 流程：先用 sourceToCol_ 把列标题翻成 1 基列号，再用 Document::read(row, col) 取值。
QVariant ExcelReader::cellBySource(int row, const QString& source) const {
    auto it = sourceToCol_.find(source);
    if (it == sourceToCol_.end())
        return QVariant();  // 未知列标题：返回无效 QVariant，上层据此知道「这列不存在」
    // it.value() 是该标题对应的 1 基列号。read() 直接返回单元格 QVariant：
    // 空单元格会返回无效/空 QVariant，调用方（Mapper）据此区分「空值」与有值。
    QVariant v = impl_->doc->read(row, it.value());
    return v;
}

// ── columnOfSource —— 列标题 → 1 基列号（未知返回 -1）─────────────────────────
// QHash::value(key, default) 在键不存在时返回提供的默认值，这里用 -1 表「无此列」。
int ExcelReader::columnOfSource(const QString& source) const {
    return sourceToCol_.value(source, -1);
}

}  // namespace dbridge::detail
