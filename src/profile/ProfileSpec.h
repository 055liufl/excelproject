#pragma once
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

// ============================================================================
// ProfileSpec.h — Profile（Excel↔SQLite 映射配置）的「内存数据模型」
// ============================================================================
//
// 【这个文件是什么】
//   Profile 是一份声明式配置（来源是一段 JSON），描述「一张 Excel 如何映射到
//   一张/多张数据库表」。本文件就是这份配置被解析后在内存里的表示——一组纯数据
//   struct/enum（无业务行为，只装配置）。它由三方协作产出与消费：
//     · ProfileLoader.cpp   ：把 JSON 文本解析、校验、归一化 → 填充本文件各 struct。
//     · ProfileValidator.cpp：拿填好的 ProfileSpec 与数据库 schema 比对，确认自洽可执行。
//     · 导入/导出引擎（ImportService/ExportService/Mapper/Router）：按这些 struct
//       的指示去读 Excel、路由、校验、查外键、写库 / 反向查回业务键再写 Excel。
//
// 【一份 Profile 的核心要素（建立直觉）】
//   · sheet/headerRow ：读哪个工作表、表头在第几行。
//   · mode            ：单表 / 多表 / 混合（按某列值分类）。
//   · routes / classes：「路由」——一行 Excel 数据要写进哪张表、哪些列。混合模式
//                       下先按 discriminator 列把行分到某个 class，再走该 class 的 routes。
//   · columns         ：Excel 表头 ↔ DB 列 的对应，外加列级校验规则（validators）。
//   · conflict        ：UPSERT 的「冲突键」，即靠哪几列判定「插入还是更新」。
//   · fkInject        ：父表写完后把父表主键「注入」到子表对应列（多表关联写入）。
//   · lookups         ：用 Excel 里的业务键去参照表 G 查出代理主键等列（外键正向查找）；
//                       导出方向可反向用代理主键查回业务键（reverse lookup）。
//   · 时间格式槽       ：date/datetime/time 三类时间列的「Excel 显示格式 ↔ DB 存储格式」。
//   · exportSpec      ：导出排序、列顺序、原生 SQL、混合模式的类别列等。
//
// 【命名空间】dbridge::detail —— 这些都是库内部实现细节，不对外暴露为公共 API。
// 【错误码】解析/校验阶段产出的码集中在 include/dbridge/Errors.h（E_PROFILE_* 等）。
// ============================================================================

namespace dbridge::detail {

// Profile 的三种「映射模式」，决定 Loader 走哪条解析分支、引擎如何路由一行数据：
//   · SingleTable —— 一张 Excel → 一张表（最简单，配置顶层直接写 table/columns）。
//   · MultiTable  —— 一张 Excel → 多张表（顶层 routes[] 列出各目标表，可有父子依赖）。
//   · Mixed       —— 按某一列（discriminator）的值把每一行分派到不同「类别 class」，
//                    每个类别再各自拥有一组 routes（即「按列值路由到不同表集合」）。
enum class ProfileMode { SingleTable, MultiTable, Mixed };

// 时间列在「某一侧」（Excel 侧或 DB 侧）的物理存储类型。
// 时间值在内存里统一是 QDate/QDateTime/QTime；两侧各自决定它的「外部表示」：
enum class TemporalPhysType {
    String,  // 默认：用一段 Qt 日期时间格式串（如 "yyyy-MM-dd"）做文本表示
    EpochSec,  // Unix 纪元秒（存进 INTEGER 列）；仅允许出现在 DB 侧、且仅 datetime 槽
};

// 把 JSON 里 type 字段的字符串（"string"/"epochSec"）解析成枚举。
// 返回 std::nullopt 表示「无法识别」——调用方（Loader）据此报 E_PROFILE_PARSE。
inline std::optional<TemporalPhysType> temporalPhysTypeFromString(const QString& s) {
    if (s == QLatin1String("string"))
        return TemporalPhysType::String;
    if (s == QLatin1String("epochSec"))
        return TemporalPhysType::EpochSec;
    return std::nullopt;  // 未知 type → 让 Loader 走错误分支
}

// TemporalSideSpec —— 一个时间槽「单侧」（Excel 侧 或 DB 侧）的格式定义。
// 一个时间列有两侧：Excel 侧（人看的显示格式）与 DB 侧（库里的存储格式），
// 导入时按 Excel 侧解析、按 DB 侧写库；导出时反过来。
struct TemporalSideSpec {
    bool declared = false;  // 本侧是否被显式声明（决定是否覆盖 profile 级默认）
    TemporalPhysType type = TemporalPhysType::String;  // 物理类型：String 或 EpochSec
    QString format;  // 当 type=String 时的 Qt 格式串；EpochSec 时必须为空
    QStringList fallback;  // 仅 Excel 侧 + type=string 有效：解析失败时依次尝试的备选格式
};

// TemporalFormatSpec —— 一个完整的时间槽（含 Excel 侧 + DB 侧两侧规格）。
// 在 ProfileSpec 上有 profile 级默认槽，在 ColumnSpec 上有列级覆盖槽，二者按
// 「侧」整体合并（见本文件末 effectiveTemporalFor）。
struct TemporalFormatSpec {
    bool declared = false;   // 本槽是否被声明过（任一侧声明即视为已声明）
    TemporalSideSpec excel;  // Excel 侧规格
    TemporalSideSpec db;     // DB 侧规格
};

// 时间槽的「种类」：一个时间列至多归属其中一种（None 表示该列不是时间列）。
//   Date —— 仅日期；DateTime —— 日期+时间；Time —— 仅时间。
enum class TemporalSlotKind { None, Date, DateTime, Time };

// ColumnSpec —— 「一列」的映射：Excel 表头 ↔ DB 列，外加校验与时间格式。
struct ColumnSpec {
    QString dbColumn;  // SQLite 列名（写库目标）
    QString source;    // 对应的 Excel 表头名（读 Excel 来源；缺省回退为 dbColumn）
    QStringList validatorTokens;  // 校验规则原始 token，如
                                  // "notNull"/"int"/"len<=32"/"regex:^...$"/"date:..."

    // 列级时间格式覆盖：与 profile 级同名槽做「逐字段（逐侧）合并」。
    // 三个槽互斥——一列至多声明其中一个（Loader 会校验并拒绝多声明）。
    TemporalFormatSpec dateFormat;      // 日期槽覆盖
    TemporalFormatSpec datetimeFormat;  // 日期时间槽覆盖
    TemporalFormatSpec timeFormat;      // 时间槽覆盖
};

// ConflictSpec —— UPSERT 的「冲突键」：靠哪几列判定一行是「插入」还是「更新」。
struct ConflictSpec {
    QStringList columns;  // 这组列必须恰好匹配目标表的 PRIMARY KEY 或某个 UNIQUE 约束
};

// FkInjectSpec —— 「外键注入」：多列 / 多父表的外键复制规则。
// 【翻译保留原注释】多列 / 多父表的外键注入。一条路由的 fkInject 是
// QVector<FkInjectSpec>；每个条目指定「一个父表」以及一组 (父列, 子列) 对，
// 在导入时把父表那一行的指定列值复制到本（子）表对应列。旧的单对象形式
// {from:"t.c", to:"t.c"} 已不再接受（必须用下面的数组+pairs 形式）。
//
// 【为什么需要】父表（如 orders）插入后才知道它的代理主键；子表（如 order_items）
// 要引用该主键，于是「父写完→把父的主键值注入子表的外键列→再写子表」。
// 注意 fromTable 必须是本 Profile 里声明过的另一条路由（区别于 lookups 查的是
// 外部参照表 G）；这也是多表写入需要拓扑排序的根源。
struct FkInjectSpec {
    QString fromTable;  // 父表名（必须是本 profile 里的一条路由）
    QVector<QPair<QString, QString>> pairs;  // (父列名 parent_column, 子列名 child_column) 对
};

// ExportOnMissing —— 导出「反向查找未命中」时的处理策略取值常量 + 校验辅助。
// 【翻译保留原注释】exportOnMissing 的合法取值；isValid / allowedList 供
// loader 与 validator 生成错误信息时复用。
struct ExportOnMissing {
    static constexpr const char* kError = "error";  // 未命中视为错误（行级报错）
    static constexpr const char* kNull = "null";  // 未命中则该单元格写 NULL（行继续）
    static constexpr const char* kSkip = "skip";  // 未命中则跳过该行
    // 判断字符串是否为三个合法值之一。
    static bool isValid(const QString& v) {
        return v == QLatin1String(kError) || v == QLatin1String(kNull) || v == QLatin1String(kSkip);
    }
    // 生成「允许取值」的人类可读列表，用于拼错误信息。
    static QString allowedList() {
        return QStringLiteral("\"error\", \"null\", \"skip\"");
    }
};

// LookupSpec —— 「外键正向查找」：路由级 lookup 规则。
// 【翻译保留原注释】路由级查找：按 Excel 表头的值在同一 SQLite 库内的参照表 G
// 上做等值匹配，取回一组列。这些查回的列会成为本路由载荷上的「路由局部 dbColumn」。
//
// 【举例】Excel 里有「客户名」，但库里子表要存的是 customer_id。配置一个 lookup：
//   from=customers，match=[(name, 客户名)]，select=[(id, customer_id)]，
//   即「在 customers 表里按 name == Excel『客户名』找到那一行，取其 id 作为 customer_id」。
//   导入方向：业务键(name) → 代理主键(id)。
//   导出方向（reverse）：库里只有 id，需反查回 name 写进 Excel——由 exportRoundtrip 控制。
struct LookupSpec {
    QString name;       // 非空，且在本路由内唯一（用作诊断标识）
    QString fromTable;  // 参照表 G 的表名
    QVector<QPair<QString, QString>> match;  // 匹配条件：(G 的列, Excel 表头)，等值比对
    QVector<QPair<QString, QString>> select;  // 取出列：(G 的列, 写入本路由的局部 dbColumn)

    // 导出方向控制（不影响导入方向）。【翻译保留原注释】
    bool exportRoundtrip = true;  // false → 跳过反向查找，直接把 H（查出列）原样输出
    QString exportOnMissing;  // "error" | "null" | "skip"；默认 "error"（由 loader 设置）
};

// RouteSpec —— 「一条路由」：一行数据要写进的「一张目标表」及其全部映射材料。
struct RouteSpec {
    QString table;                   // 目标表名
    QString parent;                  // 父路由表名；空串表示「根路由」（无父）
    ConflictSpec conflict;           // 本表 UPSERT 的冲突键
    QVector<FkInjectSpec> fkInject;  // 外键注入组（数组，可为空）
    QVector<LookupSpec> lookups;     // 外键正向/反向查找（数组，可为空）
    QVector<ColumnSpec> columns;     // Excel↔DB 列映射 + 校验
};

// ClassSpec —— 混合模式下的「一个类别」：判别值 + 该类别独有的一组路由。
struct ClassSpec {
    QString id;  // 类别标识，如 "A" / "B" / "C"
    QString matchEquals;  // 判别规则（MVP 仅支持等值）：discriminator 列 == 此值 → 归入本类
    QVector<RouteSpec> routes;  // 本类别的路由集合（一行归到本类后走这些路由）
};

// ExportSpec —— 导出相关配置（与 export JSON 子对象对应）。
struct ExportSpec {
    QStringList orderBy;  // 导出排序列（可为 "col" 或 "table.col"）
    QString explicitSql;  // 原生 SELECT（仅 singleTable/multiTable 允许；与 columnOrder 互斥）
    QString classColumn;      // 混合模式导出：把类别 id 写进哪个表头（合成列）
    QStringList columnOrder;  // 可选的输出列顺序（按 Excel 表头名排列）
};

// ProfileSpec —— 「一份完整 Profile」的顶层内存表示（本子系统的中心数据结构）。
struct ProfileSpec {
    QString name;       // Profile 名（来自 JSON profileName，必填非空）
    QString sheet;      // 默认作用的工作表名（必填非空）
    int headerRow = 1;  // 表头所在行号（1 基，必须 >= 1）
    ProfileMode mode = ProfileMode::SingleTable;  // 映射模式（决定用 routes 还是 classes）
    QVector<RouteSpec> routes;  // SingleTable / MultiTable 模式下的路由集合
    QString discriminatorSource;  // Mixed 模式：用作判别的 Excel 表头（导入必填、导出可空）
    QVector<ClassSpec> classes;  // Mixed 模式下的类别集合
    ExportSpec exportSpec;       // 导出配置

    // profile 级时间格式默认值；列级同名槽会逐字段（逐侧）覆盖它。【翻译保留原注释】
    TemporalFormatSpec dateFormat;      // 日期槽默认
    TemporalFormatSpec datetimeFormat;  // 日期时间槽默认
    TemporalFormatSpec timeFormat;      // 时间槽默认

    // 加载期收集的「信息级诊断」（如「dateFormat 覆盖了 date:fmt 校验器」）。
    // 【翻译保留原注释】这些不是错误——非阻断，仅随结果携带供 CLI / 测试检视。
    QStringList loadWarnings;

    // 草稿 Profile 支持（M-03 fix）。【翻译保留原注释】当 AutoProfileBuilder 无法
    // 产出一份「可执行」的 Profile（例如目标表既无 PRIMARY KEY 也无 UNIQUE 约束、
    // 找不到冲突键）时，它会把 executable 置 false 并往 issues 写入问题描述，
    // 让调用方把问题反馈给用户，而不是直接失败。
    bool executable = true;  // false → 这是一份「草稿」，尚不可直接执行导入
    QStringList issues;      // 草稿的待解决问题清单（人类可读）
};

// effectiveTemporalFor —— 计算某列在某时间槽种类下「最终生效」的时间格式规格。
//
// 【做什么】把「列级槽」与「profile 级同名槽」按规则合并，返回有效规格。
// 【为什么这样合并——按「侧」整体覆盖，而非逐字段】【翻译保留原注释】
//   若某列声明了 excel/db 中的某一侧，则该侧「整体」替换 profile 同侧。这样可避免
//   出现无法求解的有效规格——例如列里写了 type=epochSec（必须无 format），却又从
//   profile 继承到一段 format，二者矛盾。整侧覆盖保证一侧的 type 与 format 始终自洽。
// 【参数】kind=要计算的槽种类；col=列规格；profile=所属 profile（提供默认槽）。
// 【返回】合并后的 TemporalFormatSpec；out.declared==false 表示「该列此槽未生效」。
// 【副作用】无（纯函数）。【复杂度】O(列的 validator 数)，仅遗留路径需扫一遍。
inline TemporalFormatSpec effectiveTemporalFor(TemporalSlotKind kind, const ColumnSpec& col,
                                               const ProfileSpec& profile) {
    // 按 kind 选出「列级槽指针, profile 级槽指针」这一对；None 返回 {nullptr,nullptr}。
    auto pick =
        [](const ColumnSpec& c, const ProfileSpec& p,
           TemporalSlotKind k) -> QPair<const TemporalFormatSpec*, const TemporalFormatSpec*> {
        switch (k) {
            case TemporalSlotKind::Date:
                return {&c.dateFormat, &p.dateFormat};
            case TemporalSlotKind::DateTime:
                return {&c.datetimeFormat, &p.datetimeFormat};
            case TemporalSlotKind::Time:
                return {&c.timeFormat, &p.timeFormat};
            case TemporalSlotKind::None:
            default:
                return {nullptr, nullptr};
        }
    };
    auto pair = pick(col, profile, kind);
    TemporalFormatSpec out;
    if (!pair.first || !pair.second)
        return out;                                   // kind==None：返回未声明的空规格
    const TemporalFormatSpec& colSlot = *pair.first;  // 列级槽
    const TemporalFormatSpec& profileSlot = *pair.second;  // profile 级槽
    // 遗留路径处理。【翻译保留原注释】列是通过 date:xxx 校验器（而非列级槽声明）
    // 走到这里的（即列自己没声明 date 槽）。此时 profile 级 dateFormat 不得覆盖——
    // 校验器会自行处理它的解析，profile 默认不该插手。命中即返回空规格。
    if (kind == TemporalSlotKind::Date && !colSlot.declared) {
        for (const auto& t : col.validatorTokens) {
            if (t.startsWith(QStringLiteral("date:")))
                return out;
        }
    }
    // 只要列级或 profile 级任一侧声明过，本槽即视为「已声明（生效）」。
    out.declared = colSlot.declared || profileSlot.declared;
    // 「按侧整体覆盖」：列声明了某侧 → 用列的该侧；否则继承 profile 的该侧。
    out.excel = colSlot.excel.declared ? colSlot.excel : profileSlot.excel;
    out.db = colSlot.db.declared ? colSlot.db : profileSlot.db;
    return out;
}

// temporalSlotKindFor —— 判定某列由「哪一类时间槽」治理（或根本不是时间列）。
//
// 【做什么】返回该列生效的 TemporalSlotKind；None 表示「非时间列」。
// 【为什么需要】列级与 profile 级、新式槽与遗留 date: 校验器、文本/数值列并存，
//   需要一个统一、确定的优先级来裁决「这一列到底按不按时间处理、按哪种时间」。
// 【解析优先级（顺序很重要）】【翻译保留原注释】
//   1. 列级槽若已 `declared`，由它决定种类（dateFormat→Date 等）。
//   2. 否则列若带 `date:fmt` 校验器 → 当作 Date（遗留兼容）。
//   3. 否则列若带「显式数值校验器」（int / int>=N / decimal 等任何非 date: 校验器）→
//      不回退到 profile 级时间槽——数值列永远不是时间列。
//   4. 否则若 profile 级某种类槽已 `declared` → 套用该种类。
//   5. 都不满足 → None。
//   当多个列级槽碰巧同时声明（这本身非法、加载时会被拒），按 (Date, DateTime, Time)
//   顺序取第一个匹配——这里保留防御性写法。
// 【参数】col=列；profile=所属 profile。【返回】槽种类。【副作用】无（纯函数）。
inline TemporalSlotKind temporalSlotKindFor(const ColumnSpec& col, const ProfileSpec& profile) {
    // (1) 列级槽优先：哪个槽声明了就用哪个（互斥，正常只会有一个）。
    if (col.dateFormat.declared)
        return TemporalSlotKind::Date;
    if (col.datetimeFormat.declared)
        return TemporalSlotKind::DateTime;
    if (col.timeFormat.declared)
        return TemporalSlotKind::Time;
    // (2) 遗留兼容：带 date:fmt 校验器视作 Date 列。
    for (const auto& t : col.validatorTokens) {
        if (t.startsWith(QStringLiteral("date:")))
            return TemporalSlotKind::Date;
    }
    // (3) 列带任何（上面已处理的 date: 之外的）校验器，意味着它有明确类型。
    // 【翻译保留原注释】对这类列要求「列级时间声明」才算时间列；此处跳过 profile 级
    // 回退，避免把文本/数值列误判为时间列。
    if (!col.validatorTokens.isEmpty())
        return TemporalSlotKind::None;
    // (4) 无任何列级线索 → 回退到 profile 级默认槽。
    if (profile.dateFormat.declared)
        return TemporalSlotKind::Date;
    if (profile.datetimeFormat.declared)
        return TemporalSlotKind::DateTime;
    if (profile.timeFormat.declared)
        return TemporalSlotKind::Time;
    // (5) 全无 → 非时间列。
    return TemporalSlotKind::None;
}

}  // namespace dbridge::detail
