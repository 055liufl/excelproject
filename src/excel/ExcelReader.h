#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>

// ============================================================================
// ExcelReader.h — .xlsx 文件读取器（基于 QXlsx 第三方库的薄封装）
// ============================================================================
//
// 【这个文件是什么】
//   ExcelReader 是 Excel↔SQLite 批量导入导出（ETL）子系统里负责「读 .xlsx」的那
//   一层。它把第三方库 QXlsx 的细节包起来，只对上层暴露几个语义清晰的动作：
//     · open()         —— 打开并加载一个 .xlsx 工作簿文件；
//     · selectSheet()  —— 在工作簿里选中某张工作表（sheet）作为后续读取目标；
//     · readHeader()   —— 定位表头行、读出各列标题、建立「列标题 → 列号」映射；
//     · cellBySource() —— 按「列标题 + 行号」读出某个单元格的值（QVariant）；
//     · firstDataRow()/lastRow()/headers()/columnOfSource()/hasSource()
//                      —— 一组查询接口，供调用方驱动逐行遍历与列定位。
//
// 【在导入管线（ETL）中的位置】
//   .xlsx 文件
//      │  ExcelReader.open()        打开工作簿
//      │  ExcelReader.selectSheet() 选中工作表
//      │  ExcelReader.readHeader()  读表头、建列映射、算出数据行范围
//      ▼
//   ImportService 逐行循环：for (row = firstDataRow(); row <= lastRow(); ++row)
//      │  对每一行，按 Profile 里配置的「源列标题」调用 cellBySource(row, source)
//      │  取出该单元格的 QVariant，交给 Mapper 做类型转换 / 校验 / 路由
//      ▼
//   解析成 RowContext（含多个 RoutePayload）→ 外键查找 → UPSERT 写库
//
//   也就是说：ExcelReader 只管「把表格读成 行×列 的 QVariant 矩阵」这一件事，
//   它不认识 Profile、不认识数据库、不做任何业务解释——这些都在上层 ImportService
//   / Mapper 里。这种「薄封装 + 单一职责」让 QXlsx 一旦换库或升级，影响面被锁死
//   在本文件内。
//
// 【协作者】
//   · ImportService —— 主调用方，驱动「打开→选表→读表头→逐行读单元格」的流程；
//   · Profile（ETL 映射配置）—— 提供「源列标题（source）」字符串，本类据此把
//     标题翻译成物理列号去 QXlsx 取数；
//   · Errors.h —— 打开/读取失败时对应错误码 E_OPEN_XLSX（注意：本类自身只把人类
//     可读的 *err 字符串填好返回 false，错误码 E_OPEN_XLSX 由上层在收到 false 后
//     封装进 RowError 一并回报，本类不直接引用 err:: 常量，保持纯粹）。
//
// 【依赖：QXlsx】
//   QXlsx 是一个纯 C++/Qt 的 .xlsx 读写库（无需安装 MS Office / COM）。它的三个
//   核心抽象：
//     · QXlsx::Document   —— 一个工作簿（对应一个 .xlsx 文件），是顶层入口；
//     · QXlsx::Worksheet  —— 工作簿里的一张工作表（这里我们通过 Document 的
//                            selectSheet/read 间接操作「当前选中表」，不直接持有）；
//     · QXlsx::Cell       —— 单个单元格（本类里不直接用，Document::read() 已经
//                            帮我们把 Cell 拆成了 QVariant）。
//   关键约定：QXlsx 的行号、列号都是 **1 基**（第 1 行 / 第 1 列起算），这与
//   Excel 用户界面一致，但与 C++ 数组的 0 基相反——下面所有行列号都按 1 基理解。
//
// 【为什么在头里只前向声明 QXlsx::Document】
//   见下方 forward declaration：把 QXlsx 的重型头文件（xlsxdocument.h 等）挡在
//   .cpp 里，本头只声明一个「不完整类型」的指针成员（藏在 Impl 里）。这样包含
//   本头的上层代码无需也跟着 #include QXlsx，编译更快、依赖更干净——这正是经典的
//   PIMPL（pointer-to-implementation）手法（见下方 struct Impl）。
// ============================================================================

// 前向声明 QXlsx::Document，避免在面向上层的公开头里拉入 QXlsx 的实现头。
// （原英文注释：Forward declare to avoid pulling in QXlsx headers in public-facing code）
// 这里只声明「有这么个类」，不给定义；只要本头不解引用它、不调用它的成员，编译器
// 就允许用它的指针（实际指针被进一步藏进了 .cpp 里的 struct Impl，见私有段）。
namespace QXlsx {
class Document;
}

namespace dbridge::detail {

// ── ExcelReader —— .xlsx 读取器 ─────────────────────────────────────────────
//
// 【职责】把一个 .xlsx 文件读成「带列标题映射的 行×列 QVariant 矩阵」，供上层
//         逐行解析。它是有状态对象：open→selectSheet→readHeader 是有先后顺序的
//         三步初始化，完成后才能用 cellBySource() 等查询接口。
//
// 【典型用法（伪代码）】
//   ExcelReader r;
//   QString err;
//   if (!r.open("data.xlsx", &err))            { /* 报错：err 内含原因 */ }
//   if (!r.selectSheet("Sheet1", &err))        { /* 报错 */ }
//   if (!r.readHeader(/*headerRow=*/1, &err))  { /* 报错 */ }
//   for (int row = r.firstDataRow(); row <= r.lastRow(); ++row) {
//       QVariant v = r.cellBySource(row, "客户名称");   // 按列标题取值
//       // ... 交给 Mapper 处理 ...
//   }
//
// 【线程】非线程安全：QXlsx::Document 内部有可变状态（如「当前选中表」），多个
//         线程并发读同一个 ExcelReader 会冲突。导入管线本就是单线程逐行跑，无碍。
class ExcelReader {
   public:
    // 构造：仅分配内部实现体 impl_（PIMPL），此时还没有打开任何文件。
    // 必须随后调用 open() 才有意义。详细实现见 .cpp 构造函数。
    ExcelReader();

    // 析构：声明在此、定义在 .cpp（= default）。
    // 【为什么不能用编译器默认的内联析构】因为成员 impl_ 是
    // std::unique_ptr<Impl>，而 Impl 在本头里是「不完整类型」（只在 .cpp 里
    // 定义）。unique_ptr 析构时需要 Impl 的完整定义才能正确销毁，所以析构函数
    // 必须挪到 .cpp（那里 Impl 已完整）里去 = default——这是 PIMPL 的固定套路。
    ~ExcelReader();

    // open —— 打开并加载一个 .xlsx 工作簿。
    // 参数：
    //   xlsxPath —— .xlsx 文件的路径（相对或绝对，按当前工作目录解析）。
    //   err      —— 出参（可为 nullptr）：失败时写入人类可读的错误原因。
    // 返回：true=加载成功；false=打开/解析失败（文件不存在、损坏、非 xlsx 等）。
    // 副作用：在 impl_ 内新建并加载一个 QXlsx::Document（会真正读盘解析 zip）。
    // 错误模式：失败对应错误码 E_OPEN_XLSX（由上层封装）。本函数失败后对象处于
    //           「未打开」状态，不应再调用后续接口。
    bool open(const QString& xlsxPath, QString* err);

    // selectSheet —— 选中工作簿里名为 name 的工作表，作为后续读取目标。
    // 参数：
    //   name —— 工作表名（即 Excel 底部标签上的名字，区分大小写）。
    //   err  —— 出参（可为 nullptr）：表不存在时写入原因。
    // 返回：true=选中成功；false=工作簿里没有这张表。
    // 副作用：改变 QXlsx::Document 内部的「当前活动表」，并记下表名到 impl_。
    // 前置条件：必须先 open() 成功。
    bool selectSheet(const QString& name, QString* err);

    // readHeader —— 定位表头行、读出各列标题、建立「列标题 → 列号」映射。
    // 参数：
    //   headerRow —— 表头所在的行号（1 基）。常见值为 1（首行即表头）。
    //   err       —— 出参（可为 nullptr）：未找到任何表头时写入原因。
    // 返回：true=至少读到一个非空表头；false=该行为空（无表头）。
    // 副作用：填充 headers_（标题列表）、sourceToCol_（标题→列号），并据工作表
    //         维度算出 lastRow_（最后一个有数据的行号）、记下 headerRow_。
    // 关键语义：从第 1 列起向右连续扫描，**遇到第一个空单元格即停止**——即表头
    //           必须是「从最左列开始、中间不留空」的一段连续标题。详见 .cpp。
    // 前置条件：必须先 open() 且 selectSheet() 成功。
    bool readHeader(int headerRow, QString* err);

    // firstDataRow —— 第一条数据行的行号（1 基）。
    // 约定：数据紧接在表头之下，故 = 表头行号 + 1。
    // 这是个内联只读访问器（无副作用、O(1)）；供上层 for 循环作起始边界。
    int firstDataRow() const {
        return headerRow_ + 1;
    }

    // lastRow —— 最后一个有数据的行号（1 基），由 readHeader() 依工作表维度算出。
    // 供上层 for 循环作终止边界（循环条件用 row <= lastRow()）。
    // 内联只读访问器，无副作用，O(1)。
    int lastRow() const {
        return lastRow_;
    }

    // cellBySource —— 按「列标题 source + 行号 row」读出单个单元格的值。
    // 参数：
    //   row    —— 行号（1 基）；通常在 [firstDataRow(), lastRow()] 区间内迭代。
    //   source —— 列标题（即 readHeader 读到的某个表头字符串），由 Profile 提供。
    // 返回：该单元格的 QVariant 值；若 source 不是已知列标题，返回无效 QVariant()。
    //       空单元格也会返回无效/空 QVariant（由 QXlsx 决定，上层据此识别空值）。
    // 副作用：无（const 方法，只读 QXlsx 数据）。
    // 复杂度：O(1)（哈希查列号 + QXlsx 单格读取）。定义见 .cpp。
    QVariant cellBySource(int row, const QString& source) const;

    // headers —— 返回已读到的全部列标题（按从左到右的列序）。
    // 内联只读访问器；返回的是副本（QStringList 隐式共享，开销小）。无副作用。
    QStringList headers() const {
        return headers_;
    }

    // columnOfSource —— 把列标题翻译成它的物理列号（1 基）。
    // 返回：命中返回 1 基列号；未知标题返回 -1（注意：用 -1 而非 0 表示「无」，
    //       因为列号是 1 基，0 并非合法列号也可表「无」，此处选 -1 更直观）。
    // 无副作用，O(1)。定义见 .cpp。
    int columnOfSource(const QString& source) const;

    // hasSource —— 判断某个列标题是否存在于表头中。
    // 内联谓词，等价于「columnOfSource(source) != -1」但更直白。
    // 无副作用，O(1)。
    bool hasSource(const QString& source) const {
        return sourceToCol_.contains(source);
    }

   private:
    // ── PIMPL：把 QXlsx 的具体类型藏进 .cpp ────────────────────────────────
    // struct Impl 在本头里只前向声明（不完整类型），真正定义在 .cpp（那里它含
    // 一个 std::unique_ptr<QXlsx::Document>）。好处：本头无需 #include 任何 QXlsx
    // 头，编译解耦；代价：析构函数必须移到 .cpp（见上文 ~ExcelReader 说明）。
    struct Impl;
    std::unique_ptr<Impl> impl_;  // 指向实现体（持有 QXlsx::Document 与当前表名）

    // ── 由 readHeader() 填充、供查询接口读取的状态 ─────────────────────────
    int headerRow_ = 1;  // 表头所在行号（1 基）；firstDataRow() = 此值 + 1
    int lastRow_ = 0;  // 最后一个有数据的行号（1 基）；初值 0 表「尚未读表头」
    QStringList headers_;              // 各列标题，按列序（第 1 列 → headers_[0]）
    QHash<QString, int> sourceToCol_;  // 列标题 → 1 基列号 的映射（按标题快速定位列）
                                       // （原英文注释：source -> 1-based column index）
};

}  // namespace dbridge::detail
