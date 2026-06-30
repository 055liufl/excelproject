#pragma once
#include "ProfileSpec.h"
#include "schema/SchemaCatalog.h"

// ============================================================================
// AutoProfileBuilder.h — 「从数据库自省 → 自动生成单表 Profile 草稿」的构建器
// ============================================================================
//
// 【这个文件是什么】
//   AutoProfileBuilder 是一个无状态的小工具类：给它一张表的「自省结果」
//   （TableInfo：表名 / 列 / 类型 / 主键 / 唯一索引 / 外键，来自 SchemaCatalog），
//   它就替你「猜」出一份合理的单表 Profile（ProfileSpec，见 ProfileSpec.h），
//   并能把这份 Profile 序列化成一段缩进规整、字段顺序固定的 JSON 文本。
//   目的不是「一步到位生成最终配置」，而是「生成一份草稿（draft）」——
//   省去用户从零手写 columns/conflict 的体力活，用户拿到后只需在其上微调
//   （改表头名、加正则校验、补冲突键等）再正式载入。
//
// 【在 ETL 管线中的位置】
//   它处在「正式导入之前」的「配置生成」阶段，方向与运行期引擎相反：
//
//     数据库 schema ──自省──> SchemaCatalog（TableInfo 集合）
//                                   │
//                                   ▼  本类 build()
//                            ProfileSpec（内存草稿）
//                                   │  本类 toJson()
//                                   ▼
//                            Profile JSON 文本（交给用户微调）
//                                   │  用户编辑后，由 ProfileLoader 解析
//                                   ▼
//                            正式 ProfileSpec ──> ImportService / ExportService 执行
//
//   也就是说：本类是「写 Profile」，ProfileLoader 是「读 Profile」，二者是逆操作；
//   本类产出的 JSON 必须能被 ProfileLoader 重新吃回去（字段命名/结构需对齐）。
//
// 【协作者】
//   · SchemaCatalog.h —— 提供输入 TableInfo / ColumnInfo / IndexInfo（自省数据来源）。
//   · ProfileSpec.h   —— 提供输出的内存模型 ProfileSpec / RouteSpec / ColumnSpec 等。
//   · ProfileLoader   —— 本类 JSON 的「下游消费者」（用户微调后由它解析回内存）。
//   · Errors.h        —— 草稿不可执行时写入 issues 的错误码（E_PROFILE_NO_CONFLICT_KEY）。
//
// 【命名空间】dbridge::detail —— 库内部实现细节，不作为对外公共 API。
// ============================================================================

namespace dbridge::detail {

// AutoProfileBuilder —— 无成员状态的构建器（每次调用自包含，可安全复用同一实例）。
class AutoProfileBuilder {
   public:
    // build —— 由一张表的自省信息生成一份「单表」ProfileSpec。
    //
    // 【做什么】读 table 的列 / 主键 / 唯一索引，推断出：
    //   · profile 名（"auto_<表名>"）、默认 sheet（= 表名）、headerRow=1、mode=SingleTable；
    //   · 冲突键 conflict.columns（UPSERT 靠哪几列判定插入还是更新，见 .cpp 的推断优先级）；
    //   · 可写列映射 columns（跳过自增主键与生成列，Excel 表头默认 = DB 列名，并按列类型
    //     自动附加 notNull/int/decimal/date 校验器）；
    //   · 默认导出排序 orderBy（取冲突键的第一列）。
    //
    // 【为什么需要】让用户「在草稿上改」而非「从空白写」，显著降低上手成本；
    //   同时把「主键/唯一约束 → 冲突键」「列类型 → 校验器」这类机械推断自动化。
    //
    // 【参数】
    //   · table —— 输入：单张表的自省结果（只读）。
    //   · out   —— 输出：被填充的 ProfileSpec（调用方提供、非空，函数原地写入）。
    //   · err   —— 输出：错误信息槽（非空）。注意：本函数当前从不向 *err 写内容——
    //              「缺冲突键」被视为「草稿」而非「错误」（见返回值说明与 .cpp 的 M-03 fix）。
    //
    // 【返回】
    //   · 始终返回 true（即便无可用冲突键时，也产出一份 executable=false 的草稿）。
    //   · 函数签名注释保留历史语义说明：原设计「无可用冲突键时返回 false」，
    //     但 M-03 fix 后改为「不失败、出草稿」——真正能否执行要看 out->executable。
    //
    // 【副作用】仅写 *out（清不在此做，假定 out 为新构造的空白 ProfileSpec）。
    // 【错误模式】不抛异常；「不可执行」通过 out->executable=false + out->issues 体现。
    // 【复杂度】O(列数 + 索引数)——对列扫描常数遍、对索引扫描一遍。
    //
    // 【原英文注释翻译保留】
    //   Build an auto-generated single-table ProfileSpec.（构建一份自动生成的单表 ProfileSpec。）
    //   Returns false if table has no usable conflict key.
    //   （历史说明：若表无可用冲突键则返回 false——见上，M-03 fix 后已不再如此，改为出草稿。）
    bool build(const TableInfo& table, ProfileSpec* out, QString* err);

    // toJson —— 把一份 ProfileSpec 序列化为「字段顺序固定」的缩进 JSON 文本。
    //
    // 【做什么】按固定键序输出：profileName / sheet / headerRow / mode →
    //   （草稿时）executable + issues → table / conflict / columns → export。
    //   产出的结构与 ProfileLoader 期望的输入格式对齐，可被重新载入。
    //
    // 【为什么字段顺序固定】便于人眼阅读、利于 diff/测试做稳定的逐字节比对
    //   （JSON 对象键本无序，这里靠插入顺序人为固定，避免每次生成顺序抖动）。
    //
    // 【参数】profile —— 待序列化的 Profile（只读）。
    // 【返回】UTF-8 的 JSON 字符串（QJsonDocument::Indented 缩进风格）。
    // 【副作用】无（const 成员函数，纯函数式）。
    // 【复杂度】O(列数)——逐列写出一个 JSON 子对象。
    QString toJson(const ProfileSpec& profile) const;
};

}  // namespace dbridge::detail
