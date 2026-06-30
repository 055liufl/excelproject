#pragma once
#include <QStringList>

#include "ProfileSpec.h"
#include "schema/SchemaCatalog.h"

// ============================================================================
// ProfileValidator.h — Profile（Excel↔SQLite 映射配置）的「静态校验器」接口
// ============================================================================
//
// 【这个文件是什么】
//   声明 ProfileValidator 类。它在 ETL 管线「真正读 Excel / 写库之前」做一道
//   静态体检：把已被 ProfileLoader 解析好的 ProfileSpec（见 ProfileSpec.h）与
//   两份外部事实——数据库 schema（SchemaCatalog，见 schema/SchemaCatalog.h）和
//   Excel 表头列表（excelHeaders）——逐条比对，确认这份配置「自洽且可执行」。
//   校验只读取、绝不修改 ProfileSpec / SchemaCatalog。
//
// 【在 ETL 管线中的位置】
//   ProfileLoader（JSON→ProfileSpec）          // 上游：语法/结构层校验
//     → SchemaCatalog（自省数据库得到表/列/索引/外键）
//     → ProfileValidator（本类：语义层校验，Profile vs 真实 schema 对账）  ← 你在这里
//     → ImportService / ExportService（下游：按已验证的 Profile 执行导入导出）
//   把错误挡在执行之前，避免「跑到一半才发现某列不存在」这类昂贵失败。
//
// 【协作者】
//   · ProfileSpec.h        ：被校验的数据模型（routes / classes / lookups / fkInject…）。
//   · schema/SchemaCatalog ：数据库真相来源（hasTable / table(name) / 列 / 索引 / PK）。
//   · service/ErrorCollector：错误/警告的「收集器」，校验中发现的每处问题都 addTable /
//                             addTableWarning 进去（错误码取自 dbridge/Errors.h）。
//
// 【错误/警告码（详见 dbridge/Errors.h）】
//   E_PROFILE_TABLE_NOT_FOUND   引用了 schema 中不存在的表（路由表 / lookup.from /
//                               fkInject.from / parent）。
//   E_PROFILE_COLUMN_NOT_FOUND  引用了不存在的列，或同一 dbColumn 被多个来源重复写入。
//   E_PROFILE_NO_CONFLICT_KEY   UPSERT 缺冲突键，或冲突列不存在 / 不匹配任何 PK/UNIQUE。
//   E_PROFILE_PARSE             结构性自洽问题（重名 lookup、级联查找、混源 fkInject…）。
//   E_HEADER_NOT_FOUND          Profile 期望的 Excel 表头在实际表头里找不到（仅导入向）。
//   W_TIME_ORDERBY_NONSORTABLE  orderBy 命中的时间列 db.format 不以 yyyy 开头（非阻断警告）。
//   E_EXPORT_UNKNOWN_HEADER /   导出 columnOrder 含未知表头 / 重复项 / 与原生 SQL 同用。
//   E_EXPORT_DUPLICATE_ORDER / E_EXPORT_ORDER_WITH_RAW_SQL
//
// 【设计要点】校验「尽量收集、不早退」——单条路由内多数检查即便失败也继续往下查，
//   一次把尽可能多的问题报全（少数硬性前置失败如「表不存在」才提前 return）。
//   返回值 bool 表示「整体是否通过（无 E_ 级错误）」；警告不影响该返回值。
// ============================================================================

namespace dbridge::detail {

// 前置声明：错误/警告收集器（定义见 service/ErrorCollector.h）。
// 仅以指针形式使用，故此处声明即可，无需包含其头文件（降低编译耦合）。
class ErrorCollector;

// ProfileValidator —— Profile 语义校验器（无状态：所有输入经参数传入，无成员变量）。
// 可重复调用、可复用同一实例；线程安全性取决于传入的 ErrorCollector。
class ProfileValidator {
   public:
    // validate —— 导入方向的「总校验入口」。
    // 【做什么】依次校验：顶层字段（name/sheet/headerRow）、混合模式判别列、各路由
    //   （表/列/冲突键/lookup/fkInject 自洽）、orderBy 时间列可排序性、导出 columnOrder。
    // 【参数】
    //   profile      —— 待校验的 Profile（只读）。
    //   catalog      —— 数据库 schema 自省结果，用作「表/列是否存在」的真相来源（只读）。
    //   excelHeaders —— 实际 Excel 文件的表头名列表；用于校验 source/match 表头存在性。
    //   errors       —— 出参：发现的每处错误/警告都追加到这里（不可为 null）。
    //   importMode   —— true=导入向（默认）：检查判别列、Excel 表头等导入专属规则；
    //                   false=导出向：跳过这些导入专属检查（由 validateForExport 走此路径）。
    // 【返回】true=无 E_ 级错误（警告不影响）；false=至少一处错误。
    // 【副作用】仅通过 errors 收集诊断；不改动 profile / catalog。
    // 【错误模式】见文件头错误码表（E_PROFILE_*、E_HEADER_NOT_FOUND、E_EXPORT_*、
    //   W_TIME_ORDERBY_NONSORTABLE）。
    bool validate(const ProfileSpec& profile, const SchemaCatalog& catalog,
                  const QStringList& excelHeaders, ErrorCollector* errors, bool importMode = true);

    // validateForExport —— 导出方向的校验入口（H-03 fix）。
    // 【翻译保留原注释】H-03 fix：导出模式校验——跳过 discriminator.source 与 Excel 表头
    //   检查（这些是导入专属关切），但仍校验 columnOrder、原生 SQL、表/列存在性、
    //   冲突键，以及反向查找（reverse-lookup）引用。
    // 【实现策略】导出时没有真实 Excel 表头可比对，于是它「合成」一份表头清单
    //   （把 Profile 里声明过的所有列来源都塞进去），让表头存在性检查必然通过，
    //   再以 importMode=false 委托给 validate()，从而只保留导出真正在意的那些规则。
    // 【参数/返回/副作用】同 validate（无 excelHeaders 参数：内部合成）。
    bool validateForExport(const ProfileSpec& profile, const SchemaCatalog& catalog,
                           ErrorCollector* errors);

   private:
    // validateRoutes —— 校验「一组路由」（SingleTable/MultiTable 的顶层 routes，或
    //   Mixed 模式下某个 class 的 routes）。
    // 【做什么】逐条调用 validateRoute；再做「跨路由」检查：parent 字段必须指向本组内
    //   存在的另一条路由（否则 E_PROFILE_TABLE_NOT_FOUND）。
    // 【参数】routes=本组路由；其余同 validate。【返回】本组是否全部通过。
    // 【为什么 allRoutes 也传给 validateRoute】fkInject 需要按表名在「同组其它路由」里
    //   查父路由，故每条路由校验时都需看到整组。
    bool validateRoutes(const QVector<RouteSpec>& routes, const SchemaCatalog& catalog,
                        const QStringList& excelHeaders, const QString& sheet,
                        ErrorCollector* errors);

    // validateRoute —— 校验「单条路由」的全部自洽性（本类最核心、最长的方法）。
    // 【覆盖的规则（详见 .cpp 内逐条注释，编号对应 Profile 规格 §3.x）】
    //   · 路由目标表存在；冲突键非空、列存在且匹配 PK/UNIQUE；
    //   · columns：dbColumn 存在、source 表头存在、dbColumn 在本路由内不重复；
    //   · lookups：name 路由内唯一、fromTable 存在、match/select 的列与表头存在、
    //     select 目标在 lookup 内唯一、且与其它来源（excel/其它 lookup）不冲突、
    //     禁止级联（一个 lookup 的输出不能再做另一个 lookup 的匹配键）；
    //   · fkInject：父表必须是本组声明过的路由、父列存在于父路由的产出列、子列存在、
    //     子列跨注入组唯一、且与其它来源不冲突、同一注入组不可混用 lookup 派生与
    //     Excel 派生的父列。
    // 【参数】route=待校验路由；allRoutes=本组全部路由（供 fkInject 跨路由查父表）；
    //   其余同上。【返回】本路由是否通过。【副作用】问题写入 errors。
    bool validateRoute(const RouteSpec& route, const QVector<RouteSpec>& allRoutes,
                       const SchemaCatalog& catalog, const QStringList& excelHeaders,
                       const QString& sheet, ErrorCollector* errors);

    // isConflictValid —— 判断一组冲突列是否构成 SQLite 合法的 ON CONFLICT 目标。
    // 【为什么】UPSERT 的 ON CONFLICT 子句只接受「完整匹配某个非部分 UNIQUE 约束或
    //   PRIMARY KEY 的列集合」；否则数据库会拒绝。本函数据此判定。
    // 【判定逻辑】冲突列集合（去序，按集合比较）== 表的 PK 列集合，或 == 某个
    //   「非部分」UNIQUE 索引的列集合，即合法（H-02 fix：部分索引被排除）。
    // 【参数】conflict=Profile 的冲突键；table=目标表 schema。
    // 【返回】true=合法冲突目标；false=空或不匹配任何 PK/UNIQUE（调用方报
    //   E_PROFILE_NO_CONFLICT_KEY）。【副作用】无（纯查询）。【复杂度】O(列数+索引数)。
    bool isConflictValid(const ConflictSpec& conflict, const TableInfo& table);
};

}  // namespace dbridge::detail
