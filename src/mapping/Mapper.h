#pragma once
#include <QHash>

#include "RowPayload.h"
#include "profile/ProfileSpec.h"
#include "validation/ValidatorChain.h"

// ============================================================================
// Mapper.h — Excel 导入管线的「单行映射」核心：把一行 Excel 变成若干 RoutePayload
// ============================================================================
//
// 【单一职责】
//   Mapper 是 mapping 子系统的总装配工。给它「一行 Excel 单元格 + 一组路由规则
//   (RouteSpec) + 编译好的校验器」，它产出该行展开后的所有 RoutePayload
//   （每个 RoutePayload = 写一张目标表所需的列名/绑定值/冲突键，见 RowPayload.h）。
//
// 【它在 mapping/validation 流水线中的位置】
//   读 Excel → ┌─ Router 选类别（仅 Mixed 模式）
//              ├─ Mapper.compileValidators()  一次性把校验 token 编译成可执行链
//              └─ Mapper.map()  逐行映射 + 跑校验 + 时间格式转换 → RoutePayload[]
//   → FkInjector 注入父表外键 → BatchUniqueness 批内查重
//   → ForeignKeyPreflight 外键预检 → UPSERT 写库。
//   Mapper 只负责「映射 + 校验 + 时间转换」这三件事，不碰数据库；外键查找/写库由下游做。
//
// 【与时间转换层的分工（关键，不直观）】
//   一列若声明了 dateFormat/datetimeFormat/timeFormat「时间槽」，其时间解析/序列化
//   交给 tconv 层（TemporalConvert.*）处理，而不再走普通 validator。因此：
//     · compileValidators() 会把这些列的 `date:*` 校验 token「剥掉」（避免双重处理）；
//     · map() 里对这些列改走 tconv 的 解析(U)→序列化(V) 两步。
//   遗留写法（只写 `date:fmt` 校验、没声明时间槽对象）则仍由 validator 自己解析，不剥离。
//
// 【命名空间】dbridge::detail —— 属内部实现细节，不对外暴露。
// ============================================================================

namespace dbridge::detail {

class ExcelReader;     // 前置声明：Excel 行/单元格读取器（按表头名取值）
class ErrorCollector;  // 前置声明：错误/警告收集器（见 service/ErrorCollector.h）

// ValidatorMap —— 「编译后的校验器」二级查找表：routeKey → dbColumn → ValidatorChain。
//   外层键 routeKey 区分同名表在不同类别下的路由（见下方 Mapper::routeKey 说明）；
//   内层键是数据库列名；值是该列待跑的校验链（可能为空链 = 该列无校验）。
//   一次 compileValidators() 建表，之后每行 map() 反复查表运行，避免重复解析 token。
using ValidatorMap = QHash<QString, QHash<QString, ValidatorChain>>;

class Mapper {
   public:
    // 把所有路由的校验 token 一次性「编译」成可执行的校验链，填入 *vm。
    //
    // 做什么：遍历每条路由的每一列，将其 validatorTokens（如 "notNull"、"len<=32"、
    //         "regex:^...$"）编译为 ValidatorChain，按 (routeKey, dbColumn) 存入 *vm。
    // 为什么：编译只需做一次；逐行映射时直接复用，避免每行重复解析正则/枚举等。
    // 参数：
    //   routes   —— 当前类别（或非 Mixed 模式下的全局）路由列表。
    //   classId  —— 类别标识；非 Mixed 模式传空串（此时 routeKey 退化为表名）。
    //   profile  —— 整个 Profile（用于判定某列是否声明了「时间槽」）。
    //   vm       —— 出参：编译结果写入此二级表。
    //   err      —— 出参：任一 token 非法时写入诊断文本。
    // 返回：全部编译成功返回 true；任一 token 非法立即返回 false（并已写 *err）。
    // 副作用：写 *vm、*err。无 I/O、不碰数据库。
    // 错误模式：token 语法错误 → ValidatorChain::compile 失败 →（上游归类为 E_PROFILE_PARSE）。
    // 关键：声明了「有效时间槽」(effectiveTemporalFor(...).declared==true) 的列，
    //       其 `date:*` token 会被剥离——这些列由 map() 中的时间转换层处理，不再当作普通校验。
    //       遗留的「只有 date:fmt、没有时间槽对象」的列保留其 validator。
    bool compileValidators(const QVector<RouteSpec>& routes, const QString& classId,
                           const ProfileSpec& profile, ValidatorMap* vm, QString* err);

    // 把「一行 Excel」映射为一组 RoutePayload（每条路由对应一个，即每张目标表一个）。
    //
    // 做什么：对每条路由，按其 columns 从 reader 取单元格值，依次执行
    //         「校验链 → 时间格式转换」，把规整后的值与列名装进 RoutePayload；
    //         最后从已绑定列里回填冲突键值 conflictVals。
    // 为什么：这是「Excel 单元格 → 数据库写入材料」的核心翻译步骤。
    // 参数：
    //   routes    —— 该行要写入的路由集合。
    //   excelRow  —— 该行在 Excel 中的行号（仅用于错误定位回报）。
    //   classId   —— 类别标识（同上，非 Mixed 传空）。
    //   reader    —— Excel 读取器，按列的 source（表头名）取该行单元格。
    //   vm        —— compileValidators() 产出的校验链查找表。
    //   profile   —— 用于判定时间槽种类与取有效时间格式规格。
    //   errors    —— 出参：校验失败/时间解析失败时追加 RowError。
    //   sheetName —— 工作表名，填入错误条目的 sheet 字段以便定位（默认空，向后兼容）。
    //               M-06 fix：早期版本错误条目缺 sheet，此参数补全定位信息。
    // 返回：该行展开的 RoutePayload 列表（长度 == routes.size()）。
    //       即便某些路由有错误，仍会返回对应 payload，只是其 hasError 被置位（见 H-01 fix）。
    // 副作用：向 *errors 追加错误条目。const 方法、无成员状态改动、不碰数据库。
    // 错误模式（均关联 Errors.h，行级、只影响出错路由而非整行——见 RowPayload.h 的 H-01）：
    //   E_VALIDATE_*  —— 校验链失败（非空/类型/正则/枚举等）。
    //   E_TIME_PARSE  —— 字符串时间解析失败（导入方向）。
    // 线程：无共享可变状态，对不同行可并行调用（reader/errors 的线程安全由调用方保证）。
    // 复杂度：O(路由数 × 每路由列数)，每列含一次校验链运行与至多一次时间转换。
    QVector<RoutePayload> map(const QVector<RouteSpec>& routes, int excelRow,
                              const QString& classId, const ExcelReader& reader,
                              const ValidatorMap& vm, const ProfileSpec& profile,
                              ErrorCollector* errors, const QString& sheetName = QString()) const;

   private:
    // 计算一条路由的「路由键」routeKey：
    //   · 非 Mixed（classId 为空）→ 直接用表名，如 "orders"；
    //   · Mixed（classId 非空）  → "类别:表名"，如 "A:orders"。
    // 为什么要带类别前缀：Mixed 模式下不同类别可能写同名表，但校验规则不同；
    //   带前缀才能在 ValidatorMap 中把它们区分开（一行只会归属一个类别）。
    static QString routeKey(const RouteSpec& route, const QString& classId);
};

}  // namespace dbridge::detail
