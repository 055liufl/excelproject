#pragma once
#include <QJsonDocument>
#include <QJsonObject>

#include "ProfileSpec.h"

// ============================================================================
// ProfileLoader.h — Profile（JSON 配置）→ ProfileSpec（内存模型）的「解析器」声明
// ============================================================================
//
// 【这个类是什么】
//   ProfileLoader 是 ETL 子系统的「配置入口的第一道工序」。它把一段已经被 Qt 解析成
//   QJsonDocument 的 Profile 配置，逐字段读出、做语法/取值合法性校验、补默认值、并归一化
//   （例如把时间格式的「旧式写法」翻译成「新式写法」），最终填充出一个 ProfileSpec
//   （内存数据模型，定义见 ProfileSpec.h）。
//
// 【在 ETL 管线中的位置】
//   JSON 文本 ──Qt 解析──▶ QJsonDocument ──[ProfileLoader.load]──▶ ProfileSpec
//        ──[ProfileValidator]──▶ 与数据库 schema 比对 ──▶ 导入/导出引擎据此执行。
//   即：Loader 只管「配置本身是否结构正确、取值是否合法」；它不碰数据库——
//   「配置里引用的表/列在库里是否真的存在」由后续的 ProfileValidator 负责。
//
// 【协作者】
//   · 上游：DataBridge::loadProfile（读文件 → QJsonDocument → 调本类 load）。
//   · 数据模型：ProfileSpec.h（本类的产物；其中各 struct 字段含义见该头注释）。
//   · 下游：ProfileValidator（拿 ProfileSpec 做语义/schema 校验）、Mapper/Router/
//           ImportService/ExportService（按 ProfileSpec 执行）。
//   · 错误码：解析失败时通过 err 出参回报人类可读信息；对外归类码见 Errors.h
//             （E_PROFILE_PARSE 等，由调用方据失败语境贴码）。
//
// 【返回约定（全类统一）】所有 read*/validate* 方法均返回 bool：
//   true=成功；false=失败（且若 err 非空，已写入一条说明性错误文本）。
//   失败即「短路返回」——一处出错，整份 Profile 加载即告失败。
// ============================================================================

namespace dbridge::detail {

// ProfileLoader —— 把 Profile JSON 解析为 ProfileSpec 的纯工具类。
// 无成员状态（不持有任何字段）：所有产出都写进调用方传入的 out 指针，
// 因此一个实例可重复用于加载多份 Profile，也天然线程无共享状态。
class ProfileLoader {
   public:
    // load —— 解析入口：把整份 Profile 文档解析进 *out。
    // 【做什么】读取顶层公共字段（profileName/sheet/headerRow/mode）→ 按 mode 分派到
    //   readSingleTable / readMultiTable / readMixed → 再解析公共的 export 段与
    //   profile 级时间格式槽 → 最后做跨字段的后置校验（多槽互斥、type×format 自洽等）。
    // 【参数】doc=已解析好的 JSON 文档（必须是顶层对象）；out=输出的内存模型（非空）；
    //         err=可选错误文本出参（失败时填一条人类可读说明）。
    // 【返回】true=加载并校验通过；false=任一步失败（详情写入 *err）。
    // 【副作用】只写 *out 与 *err，不触碰数据库/文件系统。
    // 【错误模式】关联 Errors.h 的 E_PROFILE_PARSE（结构/取值非法）等，由调用方贴码。
    bool load(const QJsonDocument& doc, ProfileSpec* out, QString* err);

   private:
    // 以下三者对应三种 mode 的解析分支，均把解析出的路由追加进 out（routes 或 classes）。
    bool readSingleTable(const QJsonObject& o, ProfileSpec* out, QString* err);  // 单表模式
    bool readMultiTable(const QJsonObject& o, ProfileSpec* out, QString* err);   // 多表模式
    bool readMixed(const QJsonObject& o, ProfileSpec* out, QString* err);        // 混合模式
    // 解析「一条路由」对象（table/parent/conflict/fkInject/lookups/columns）。
    // warnings 可选：用于回收「非阻断的信息级诊断」（如 exportOnMissing 无效配置提示）。
    bool readRoute(const QJsonObject& o, RouteSpec* out, QString* err,
                   QStringList* warnings = nullptr);
    // 解析「一列」对象（source/validators/三个时间格式槽）；dbCol 为该列的 DB 列名（JSON 键）。
    bool readColumn(const QString& dbCol, const QJsonObject& o, ColumnSpec* out, QString* err);
    // 校验单个 validator token 的语法是否属于受支持的词汇表（notNull/int/len.../regex: 等）。
    bool validateToken(const QString& token, QString* err);
};

}  // namespace dbridge::detail
