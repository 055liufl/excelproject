// ============================================================================
// ExportService.cpp — SQLite→Excel「导出编排层」的实现（与 ImportService 镜像）
// ============================================================================
//
// 【这个文件是什么】
//   ExportService::run 是导出方向的总入口（声明见 ExportService.h）。它把「库里
//   的数据」按 Profile 配置变回「人能看懂的 Excel」，是 ETL 管线导出方向的“最后一公里”。
//
// 【它在 ETL 管线中的位置】
//   导入方向：Excel → 校验/查找/转换 → 写库（ImportService 负责）。
//   导出方向（本文件）：查库 SELECT → 反向查找（代理主键→业务键）→ 时间格式转换
//                       （DB 存储格式 → Excel 显示格式）→ 按 columnOrder 写出 .xlsx。
//   二者互为逆过程，许多辅助函数（makeTupleKey/castToAffinity 等）刻意与导入侧
//   保持“逐字节同构”，以保证「导入后再导出」能完整回到原始 Excel（round-trip）。
//
// 【一次 run() 的总体流程（建立直觉）】
//   1) 打开 ExcelWriter（失败即表级错误 E_WRITE_XLSX，立即返回）。
//   2) 预扫 Profile，构建“时间列 → 转换规格”的速查表（buildTemporalExportMap）。
//   3) 按 mode 分流：
//        · Mixed（混合多类）：逐 class 各查一次、合并表头，统一写出。
//        · Single/MultiTable：再按「是否需要列重排 / 反向查找」细分为三条写出路径：
//            (a) 流式路径   ：无 columnOrder 且无反向查找 → 边查边写，零额外内存。
//            (b) 仅列重排   ：有 columnOrder 但无反向查找 → 全量载入、重排列、写出。
//            (c) 反向查找   ：扩展 SELECT 取 H 列 → 预取反向缓存 → 投影替换 → 写出。
//   4) writer.save()，汇总行级/表级错误与警告，填 ExportResult 返回。
//
// 【协作者一览】
//   · SqlBuilder        ：根据排序后的 routes 生成自动 JOIN 的 SELECT SQL。
//   · TopoSorter        ：把多表路由按外键依赖排出拓扑序（成环则 E_PROFILE_TOPOLOGY_CYCLE）。
//   · SchemaCatalog     ：表结构目录，供反向查找做列定位与 SQLite affinity 强转。
//   · TemporalConvert（tconv）：时间值的解析/格式化（V→U），见 mapping/TemporalConvert.h。
//   · ExcelWriter       ：把表头/行写进 .xlsx。
//   · ErrorCollector    ：按 行/表 粒度收集错误与警告（见 ErrorCollector.h）。
//   · reorderHeaders    ：列顺序合并（自然列序 + columnOrder），见 ExportHelpers.h。
//
// 【命名空间】dbridge::detail —— 全部为库内实现细节，不对外暴露。
// 【错误码】集中在 include/dbridge/Errors.h：导出相关有 E_EXPORT_QUERY / E_WRITE_XLSX /
//   E_TIME_PARSE_DB / E_REVERSE_LOOKUP_{NOT_FOUND,AMBIGUOUS,QUERY_FAILED} /
//   E_PROFILE_TOPOLOGY_CYCLE 等；逐处用到时会标注其行为（行级/表级、是否中止）。
//
// 【源码中的 fix 标记】文中形如 H-xx / M-xx 的标记是历史 bug 修复的“锚点”，逐条保留
//   并已翻译/补充说明，便于追溯当初为何这样写——不要删除。
// ============================================================================

#include "ExportService.h"

#include "dbridge/Errors.h"

#include <QHash>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

#include "ErrorCollector.h"
#include "ExportHelpers.h"
#include "excel/ExcelWriter.h"
#include "mapping/TemporalConvert.h"
#include "mapping/TopoSorter.h"
#include "schema/SchemaCatalog.h"
#include "sql/SqlBuilder.h"
#include <algorithm>
#include <functional>

namespace dbridge::detail {

// ── 时间列导出转换的“逐列规格” ──────────────────────────────────────────────
// TemporalColumnInfo —— 描述“某一个时间列在导出方向上如何转换”所需的全部信息。
// 导出方向是 DB → Excel：先按 DB 侧规格把库里的值“解析”成结构化时间（QDate/
// QDateTime/QTime），再按 Excel 侧规格“格式化”成给人看的显示文本/数值。
// 这里把习惯叫法也标出来：V=value（DB 侧、被解析的源）、U=显示（Excel 侧、序列化目标）。
struct TemporalColumnInfo {
    TemporalSlotKind kind;  // 时间槽种类：Date / DateTime / Time（None 不会进此表）
    TemporalSideSpec db;    // V — 解析 DB 值时使用的（DB 侧）格式规格
    TemporalSideSpec excel;  // U — 序列化成 Excel 显示时使用的（Excel 侧）格式规格
};

// buildTemporalExportMap —— 预扫整份 Profile，建立“列名 → 时间转换规格”的速查表。
//
// 【做什么】遍历 profile 里所有路由（Mixed 模式则遍历每个 class 的路由）的每一列，
//   把“需要做时间格式转换的列”收集成一张扁平 QHash，供后续逐行转换时 O(1) 查表。
//
// 【为什么键用 col.source（而不是 dbColumn）——这是关键且不直观处】
//   SqlBuilder 生成的 SELECT 形如  `t.dbCol AS source`，即给每列起了别名 source。
//   因此查询结果集里 QSqlRecord::fieldName(i) 拿到的字段名是这个别名 source，
//   而非底层 dbColumn。我们后续是用“结果集字段名”去这张表里查转换规格的，
//   所以这里也必须以 col.source 为键，两边才能对上。
//
// 【为什么“首次出现者胜”（first-occurrence wins）】
//   Mixed 模式下不同 class 可能映射到同名的 source 列；若它们时间规格不同，
//   无法两全，约定取“先遇到的那个”，保证结果确定、可复现（`map.contains` 提前跳过）。
//
// 【过滤逻辑】非时间列（kind==None）跳过；以及“遗留 date:fmt 校验器但未声明新式
//   格式槽”的列（eff.declared==false）也跳过——这类列在导出方向不套用格式层，原样输出。
//
// 【参数】profile —— 完整 Profile。【返回】col.source → TemporalColumnInfo 映射。
// 【副作用】无（纯函数）。【复杂度】O(所有路由的列总数)。
static QHash<QString, TemporalColumnInfo> buildTemporalExportMap(const ProfileSpec& profile) {
    QHash<QString, TemporalColumnInfo> map;

    // 处理一组路由：把其中所有“受时间槽治理且声明了格式”的列登记进 map。
    auto processRoutes = [&](const QVector<RouteSpec>& routes) {
        for (const auto& route : routes) {
            for (const auto& col : route.columns) {
                if (map.contains(col.source))
                    continue;  // 首次出现者胜：同名 source 已登记则不覆盖
                // 判定该列是否为时间列、属哪种时间槽（解析优先级见 ProfileSpec.h）。
                TemporalSlotKind kind = temporalSlotKindFor(col, profile);
                if (kind == TemporalSlotKind::None)
                    continue;  // 非时间列，无需转换
                // 合并列级与 profile 级格式槽，得到“最终生效”的规格。
                TemporalFormatSpec eff = effectiveTemporalFor(kind, col, profile);
                if (!eff.declared)
                    continue;  // legacy date:fmt-only columns — no format layer on export
                               // 译：仅靠遗留 date:fmt 校验器（未声明新式格式槽）的列，
                               //     导出时不加格式层，跳过登记。
                map[col.source] = {kind, eff.db, eff.excel};
            }
        }
    };

    // Mixed 模式的时间列分散在各 class 的 routes 里；其余模式集中在顶层 routes。
    if (profile.mode == ProfileMode::Mixed) {
        for (const auto& cls : profile.classes)
            processRoutes(cls.routes);
    } else {
        processRoutes(profile.routes);
    }
    return map;
}

// convertTemporalForExport —— 把“一个 DB 单元格值”转换为“Excel 显示值”（V→U）。
//
// 【做什么】两步走：(1) 把 DB 值解析成结构化时间（toStructured，按 info.db 规格）；
//   (2) 再把结构化时间格式化成 Excel 显示值（formatValue，按 info.excel 规格）。
//   若 DB 值本身已是结构化时间（驱动直接给出 QDate/QDateTime/QTime），跳过解析步。
//
// 【为什么解析可能失败、失败如何处置——错误模式】
//   库里的文本若不符合 DB 侧声明的格式（脏数据 / 格式与配置不符），解析会失败。
//   此时记录行级、非阻断的 E_TIME_PARSE_DB（见 Errors.h：出错单元格写 NULL、整行
//   照常继续），并返回无效 QVariant 让该单元格留空——不会中止整张表的导出。
//
// 【NULL 的处置（与失败区分开）】显式 SQL NULL → 直接返回空 QVariant（空单元格），
//   且不报错。spec 规定：只有 isNull() 是“静默留空”，其它解析不了的才报错。
//
// 【参数】
//   dbVal     —— 来自结果集的原始 DB 值。
//   info      —— 该列的时间转换规格（kind/db/excel）。
//   sheet     —— 工作表名，用于错误定位。
//   dbColumn  —— 出错时回报的列名。
//   errors    —— 错误收集器（出口参数）。
//   outputRow —— 该值将写到的 Excel 行号（1 基）。
// 【返回】成功为 Excel 显示值；NULL / 解析失败 → 无效 QVariant（写空单元格）。
// 【副作用】解析失败时向 errors 追加一条 E_TIME_PARSE_DB。【复杂度】O(尝试的格式数)。
//
// 【M-1 fix（保留）】新增 outputRow 参数，使时间解析错误携带“真实的 Excel 行号”，
//   而非早期硬编码的 0。知道行号的调用方传入真实行号，不知道的传 0。
static QVariant convertTemporalForExport(const QVariant& dbVal, const TemporalColumnInfo& info,
                                         const QString& sheet, const QString& dbColumn,
                                         ErrorCollector* errors, int outputRow = 0) {
    // Explicit SQL NULL → empty cell, no error (spec: only isNull() is silent).
    // 译：显式 SQL NULL → 写空单元格、不报错（spec：唯有 isNull() 是静默留空）。
    if (dbVal.isNull())
        return QVariant();

    QVariant structured;
    if (tconv::isStructuredTemporal(dbVal, info.kind)) {
        // 已是匹配 kind 的结构化时间（如驱动直接返回 QDateTime）→ 免去解析。
        structured = dbVal;
    } else {
        // 否则按 DB 侧规格把原始值解析成结构化时间；失败则 errCode/errMsg 被填写。
        QString errCode, errMsg;
        structured = tconv::toStructured(dbVal, info.kind, info.db, &errCode, &errMsg);
        if (!structured.isValid()) {
            // M-1 fix: use outputRow (actual row) instead of hardcoded 0.
            // 译：用 outputRow（真实行号）而非硬编码 0 来回报错误位置。
            // 行级、非阻断错误：该单元格留空，整行继续（见 Errors.h 对 E_TIME_PARSE_DB 的说明）。
            errors->add(sheet, outputRow, dbColumn, dbVal.toString(),
                        QString::fromLatin1(err::E_TIME_PARSE_DB),
                        QStringLiteral("Cannot parse DB value '") + dbVal.toString() +
                            QStringLiteral("' (") + errMsg + QStringLiteral(")"));
            return QVariant();
        }
    }

    // 第二步：把结构化时间按 Excel 侧规格序列化成显示值。
    QVariant result = tconv::formatValue(structured, info.kind, info.excel);
    // 序列化无效（理论上罕见）也按“写空单元格”处理，避免写出垃圾值。
    return (result.isValid() && !result.isNull()) ? result : QVariant();
}

// ── 反向查找（reverse-lookup）辅助函数集 ────────────────────────────────────
// 来源特性：add-export-reverse-lookup。本段是导出方向“代理主键 → 业务键”的核心。
//
// 【先建立术语直觉】沿用 LookupSpec（见 ProfileSpec.h）的字母约定：
//   · G 表    ：参照表（lk.fromTable），业务键与代理主键共存的那张“字典表”。
//   · H 列    ：lk.select 的“G 列”，即库里实际存的值（导入时查出、当外键存进子表）。
//               导出方向我们手里有 H（代理主键），要反查回 A。
//   · A 列/A-header：lk.match 的“Excel 表头”，即人认得的业务键（如客户名）。
//   · 反向查找 = 用 H（select 的 G 列值）去 G 表里反查，取回 A（match 的 G 列值）。
//
// 【整体策略：先批量预取、再逐行投影（两阶段）】
//   逐行去查库会产生海量小查询。这里改为：先把所有行用到的 H 值去重收集，分块
//   （受 SQLite 999 变量上限约束）批量 SELECT 出 (A, H) 对，建成内存缓存
//   ReverseCache；逐行时只在缓存里查表，O(1) 命中，并据命中条数判 NOT_FOUND/AMBIGUOUS。
//
// 【放在匿名 namespace】这些都是本翻译单元私有的实现细节，避免符号外泄/冲突。

namespace {

// buildIdentityKey —— 为一条 lookup 生成稳定的“身份键”，用作缓存分桶的标识。
//
// 【做什么/为什么】不同路由可能声明“语义完全相同”的 lookup（同一个 G 表、同一组
//   match/select），它们的反向缓存应当共享、合并预取，避免重复查库。身份键就是把
//   (fromTable, match 对, select 对) 拼成一个确定字符串作为合并依据。
// 【为什么格式与导入侧一致】导入/导出对“同一条 lookup”的判定必须一致，
//   故刻意采用与 import 端相同的拼接格式（same format as import side），保证可对账。
// 【格式】 fromTable::A=B,...::G->local,...  （match 用 '='，select 用 '->'，段间 '::'）。
// 【参数】lk —— 一条 lookup 规格。【返回】身份键字符串。【副作用】无（纯函数）。
QString buildIdentityKey(const LookupSpec& lk) {
    QStringList matchParts, selectParts;
    for (const auto& p : lk.match)
        matchParts.append(p.first + QLatin1Char('=') + p.second);  // (G 列=Excel 表头)
    for (const auto& p : lk.select)
        selectParts.append(p.first + QStringLiteral("->") + p.second);  // (G 列->本地 dbColumn)
    return lk.fromTable + QStringLiteral("::") + matchParts.join(QLatin1Char(',')) +
           QStringLiteral("::") + selectParts.join(QLatin1Char(','));
}

// makeTupleKey —— 把“一个值元组”序列化成稳定、带类型标签的 QHash 键。
//
// 【做什么】反向缓存按“H 值元组”做键来命中。多列 lookup 的键是若干值，必须拼成
//   单个字符串。各值前加“类型前缀”（i:/d:/b:/s:/null），字段间用 ASCII 单元分隔符
//   0x1F（Unit Separator）连接——0x1F 几乎不会出现在真实数据里，避免歧义碰撞。
//
// 【为什么要带类型标签——H-6 fix（保留）】
//   若只按字符串比较，1（整数）、"1"（文本）、1.0（浮点）会被视为相同键，
//   但 SQLite 的等值匹配是严格区分存储类的。带类型标签后三者键不同
//   （i:1 / s:1 / d:1），从而忠实复现 SQLite 的严格相等语义——这关系到
//   预取阶段建键与逐行阶段查键必须用同一套规则，否则会命中错误的行。
//   double 用 'g',17 保证往返精度；ByteArray（BLOB）转十六进制保证可比较。
//
// 【参数】values —— 值元组（顺序敏感）。【返回】可用作 QHash 键的字符串。
// 【副作用】无（纯函数）。【复杂度】O(元组长度)。
QString makeTupleKey(const QVector<QVariant>& values) {
    QStringList parts;
    for (const auto& v : values) {
        if (v.isNull()) {
            // NULL 单独成一类键（前导 \x00 避免与任何真实文本冲突）。
            parts.append(QStringLiteral("\x00null"));
        } else {
            switch (v.type()) {
                // 整型家族统一规整为 longlong，前缀 i:（避免 int/longlong 视作不同键）。
                case QVariant::LongLong:
                case QVariant::Int:
                case QVariant::ULongLong:
                case QVariant::UInt:
                    parts.append(QStringLiteral("i:") + QString::number(v.toLongLong()));
                    break;
                // 浮点：'g',17 位有效数字 → 保证 double 文本化后可无损还原、稳定比较。
                case QVariant::Double:
                    parts.append(QStringLiteral("d:") + QString::number(v.toDouble(), 'g', 17));
                    break;
                // BLOB：转十六进制文本，使二进制内容可作为稳定键。
                case QVariant::ByteArray:
                    parts.append(QStringLiteral("b:") +
                                 QString::fromLatin1(v.toByteArray().toHex()));
                    break;
                // 其余（文本等）：前缀 s:。
                default:
                    parts.append(QStringLiteral("s:") + v.toString());
                    break;
            }
        }
    }
    return parts.join(QLatin1Char('\x1F'));  // 0x1F 单元分隔符：极不可能与真实数据冲突
}

// castToAffinity —— 按 G 表某列的“声明类型”推断 SQLite 列亲和性（affinity），
//                    并把原始值强制转换到该亲和性对应的 QVariant 类型。
//
// 【为什么需要】反向查找要把“行里取到的 H 值”与“G 表里存的值”做相等比对。
//   两边来源不同（一个来自主 SELECT、一个来自 G 表预取 SELECT），可能存储类不同
//   （如 "123" 文本 vs 123 整数）。先各自按 G 列亲和性归一，再建 tuple key 比对，
//   才能与 SQLite 自身的比较口径一致。本函数与逐行 collectHValues 用同一规则建键，
//   预取侧 buildReverseCache 也调用它——三处必须一致，命中才不会漂移。
//
// 【SQLite 亲和性规则（按 declaredType 文本包含关系判定，与官方算法一致）】
//   含 "INT"  → INTEGER 亲和；含 REAL/FLOA/DOUB → REAL 亲和；含 BLOB → BLOB；
//   含 CHAR/CLOB/TEXT → TEXT；其余（空类型 / NUMERIC）→ 不强转，原样保留。
//
// 【失败语义】数值强转失败（toLongLong/toDouble 的 ok=false）返回无效 QVariant，
//   上层据此把该行此 lookup 当作“缺值/不可匹配”处理（见调用处）。
//
// 【H-7 fix（保留）】BLOB 列保留二进制载荷，不要把它强转成空字符串。
// 【H-01 fix（保留）】空类型（none / BLOB 亲和）与 NUMERIC 亲和原样保留 raw。
// 【参数】raw=原始值；gCol=G 表列的结构信息（提供 declaredType）。
// 【返回】归一后的 QVariant；不可转 → 无效 QVariant。【副作用】无（纯函数）。
QVariant castToAffinity(const QVariant& raw, const ColumnInfo& gCol) {
    // Mirrors the same function in ImportService.cpp — must stay in sync.
    // Applies SQLite column affinity coercion per export-reverse-lookup spec §coercion-semantics.
    // 译：与 ImportService.cpp 里的同名函数互为镜像——两者必须保持同步；
    //     按 export-reverse-lookup 规范 §coercion-semantics 施加 SQLite 列亲和性强转。
    QString type = gCol.declaredType.trimmed().toUpper();
    if (type.contains(QStringLiteral("INT"))) {  // INTEGER 亲和
        bool ok;
        qlonglong iv = raw.toLongLong(&ok);
        return ok ? QVariant(iv) : QVariant();  // 转不动 → 无效（视为不可匹配）
    }
    if (type.contains(QStringLiteral("REAL")) || type.contains(QStringLiteral("FLOA")) ||
        type.contains(QStringLiteral("DOUB"))) {  // REAL 亲和
        bool ok;
        double dv = raw.toDouble(&ok);
        return ok ? QVariant(dv) : QVariant();
    }
    if (type.contains(QStringLiteral("BLOB"))) {  // BLOB 亲和
        if (raw.type() == QVariant::ByteArray)
            return raw;  // 已是二进制 → 原样
        const QByteArray ba = raw.toByteArray();
        return ba.isEmpty() ? QVariant() : QVariant(ba);  // H-7：保留二进制，不退化为空串
    }
    // TEXT affinity: declared type contains CHAR, CLOB, or TEXT.
    // 译：TEXT 亲和——声明类型里含 CHAR/CLOB/TEXT。
    if (type.contains(QStringLiteral("CHAR")) || type.contains(QStringLiteral("CLOB")) ||
        type.contains(QStringLiteral("TEXT"))) {
        return QVariant(raw.toString());
    }
    // H-01 fix: empty type (none/BLOB affinity) and NUMERIC affinity preserve raw QVariant.
    // 译：空类型（none/BLOB 亲和）与 NUMERIC 亲和 → 原样保留 raw，不做强转。
    return raw;
}

// ReverseHit —— 反向缓存里“一个 H 值元组”对应的命中记录。
//   matchVals：从 G 表取回的 A 列（业务键）值，顺序与 lk.match 的 G 列一一对应。
//   hitCount ：该 H 元组在 G 表里命中了几行——用于判 NOT_FOUND(0) / 唯一(1) / AMBIGUOUS(>1)。
//   注意：matchVals 只保存“首次命中”的那一行；当 hitCount>1 时数据本就有歧义，
//   matchVals 的具体内容不再可信，上层会因 AMBIGUOUS 跳过整行。
struct ReverseHit {
    QVector<QVariant> matchVals;  // A 列（业务键）值，按 match[].G_column 顺序
    int hitCount = 0;             // 命中行数（基数）
};

// 反向缓存的两级结构：identityKey →（H 元组键 → ReverseHit）。
//   第一级按 lookup 身份分桶（相同语义的 lookup 共享一桶）；
//   第二级在桶内按具体 H 值元组定位命中。
using ReverseCache = QHash<QString, QHash<QString, ReverseHit>>;

// hasActiveReverseLookup —— routes 中是否存在“启用了反向替换”的 lookup。
// 即任意 lk.exportRoundtrip==true（roundtrip=true 表示要把 H 真正替换回业务键 A）。
// 注：当前实现的分流主要用下面的 hasAnyLookupHCols；本函数保留为语义判定工具。
bool hasActiveReverseLookup(const QVector<RouteSpec>& routes) {
    for (const auto& route : routes)
        for (const auto& lk : route.lookups)
            if (lk.exportRoundtrip)
                return true;
    return false;
}

// hasAnyLookupHCols —— routes 中是否存在“带 select 列（H 列）”的 lookup。
//
// 【为什么与上一个函数分开】这里不区分 roundtrip 真假：只要某 lookup 声明了 select，
//   导出就可能需要把这些 H 列取出来（roundtrip=true 时替换成 A；=false 时原样输出 H）。
//   它被用来决定“是否扩展主 SELECT 以多取 H 列”，比 hasActiveReverseLookup 更宽。
// 【参数】routes。【返回】存在即 true。【副作用】无。
bool hasAnyLookupHCols(const QVector<RouteSpec>& routes) {
    for (const auto& route : routes)
        for (const auto& lk : route.lookups)
            if (!lk.select.isEmpty())
                return true;
    return false;
}

// buildHColSelectSuffix —— 生成“追加到主 SELECT 上、用于额外取出 H 列”的 SQL 片段。
//
// 【做什么】把所有 lookup（不分 roundtrip 真假）的 select 目标列（H 列）拼成形如
//   `, "t"."h1" AS "h1", "t"."h2" AS "h2"` 的列清单，准备插在原 SQL 的 " FROM " 之前
//   （插入动作由 extendSqlWithHCols 完成）。
//
// 【H-14 fix（保留）：为什么每个 H 列都显式 `AS "dbColumn"`】
//   每个 H 列都带显式别名 `AS "dbColumn"`，使结果集字段名严格等于 sp.second（即
//   dbColumn），不受 Qt/SQLite 对 “带引号的 table.column 表达式” 默认命名方式的影响。
//   这很关键：后续我们全程以 dbColumn 作为字段名去取值/查时间表，别名不稳就会取不到。
//   去重以别名（sp.second）为键，确保结果集里不会出现两列同名（多 lookup 指向同一
//   H 列时只取一次）。
//
// 【参数】routes。【返回】可直接拼接的列清单片段（空则返回空串）。【副作用】无。
QString buildHColSelectSuffix(const QVector<RouteSpec>& routes) {
    QStringList parts;
    QSet<QString> seenAlias;  // 已登记的别名集合，用于去重
    for (const auto& route : routes) {
        for (const auto& lk : route.lookups) {
            for (const auto& sp : lk.select) {
                const QString& alias = sp.second;  // dbColumn = result-set field name
                                                   // 译：dbColumn 即结果集字段名
                if (seenAlias.contains(alias))
                    continue;  // 同名 H 列只取一次，避免结果集重复列
                seenAlias.insert(alias);
                // 生成  "route.table"."alias" AS "alias"  （三处都加引号，H-10 同理的防御）。
                QString qualified = SqlBuilder::quoteIdent(route.table) + QLatin1Char('.') +
                                    SqlBuilder::quoteIdent(alias) + QStringLiteral(" AS ") +
                                    SqlBuilder::quoteIdent(alias);
                parts.append(qualified);
            }
        }
    }
    // 注意前导 ", "：因为这段会接在已有 SELECT 列清单之后。
    return parts.isEmpty() ? QString() : QStringLiteral(", ") + parts.join(QStringLiteral(", "));
}

// extendSqlWithHCols —— 把 H 列清单片段插进一条已生成的 SELECT SQL（" FROM " 之前）。
//
// 【做什么】定位首个 " FROM "，在它之前插入 hColSuffix（即 “…原列, h1 AS h1 FROM …”）。
// 【为什么用字符串插入而非重新构造】主 SQL 由 SqlBuilder 生成、可能含复杂 JOIN，
//   这里只需“多 SELECT 几列”，在 FROM 前插入是最小侵入做法。
// 【健壮性】suffix 为空 → 原样返回；找不到 " FROM "（异常 SQL）→ 也原样返回，不破坏 SQL。
// 【参数】baseSql=原 SQL；hColSuffix=待插片段。【返回】扩展后的 SQL。【副作用】无。
QString extendSqlWithHCols(const QString& baseSql, const QString& hColSuffix) {
    if (hColSuffix.isEmpty())
        return baseSql;
    int fromPos = baseSql.indexOf(QStringLiteral(" FROM "));
    if (fromPos < 0)
        return baseSql;  // 找不到 FROM：保守返回原 SQL，不冒险改写
    return baseSql.left(fromPos) + hColSuffix + baseSql.mid(fromPos);
}

// buildHColReplaceSet —— 收集“需要被业务键替换掉、因而不应出现在输出里”的 H 列名集合。
//
// 【做什么/为什么】仅针对 roundtrip=true 的 lookup：这些 lookup 的 H 列（select 目标
//   dbColumn）将被反查到的业务键 A 取代，故它们本身不应作为列写进 Excel。逐行投影时
//   用这个集合把它们从输出表头/取值中剔除。（roundtrip=false 的 H 列不在此集合——它们
//   要原样输出。）
// 【参数】routes。【返回】被替换 H 列的 dbColumn 名集合。【副作用】无。
QSet<QString> buildHColReplaceSet(const QVector<RouteSpec>& routes) {
    QSet<QString> hcols;
    for (const auto& route : routes)
        for (const auto& lk : route.lookups)
            if (lk.exportRoundtrip)
                for (const auto& sp : lk.select)
                    hcols.insert(sp.second);
    return hcols;
}

// buildAHeaders —— 收集 roundtrip=true 的 lookup 要“写回 Excel 的业务键表头”（A-header）。
//
// 【做什么】A-header 即 lk.match 的“Excel 表头”（mp.second）。反查成功后，这些表头
//   将承载查回的业务键值，加入输出列。保留“首次出现顺序”（seen 去重 + 顺序追加），
//   使输出列序稳定可复现。
// 【参数】routes。【返回】A-header 列表（去重、保序）。【副作用】无。
QStringList buildAHeaders(const QVector<RouteSpec>& routes) {
    QStringList aHeaders;
    QSet<QString> seen;
    for (const auto& route : routes) {
        for (const auto& lk : route.lookups) {
            if (!lk.exportRoundtrip)
                continue;  // 仅 roundtrip=true 才把业务键写回 Excel
            for (const auto& mp : lk.match) {
                if (!seen.contains(mp.second)) {
                    seen.insert(mp.second);
                    aHeaders.append(mp.second);  // mp.second = Excel 表头（业务键列名）
                }
            }
        }
    }
    return aHeaders;
}

// buildReverseCache —— 反向查找的“批量预取”阶段：建好整张 ReverseCache。
//
// 【做什么】对每一个 lookup 身份（identity），拿主 SELECT 阶段收集到的所有 H 值，
//   分批去 G 表里 SELECT 出 (A 列, H 列) 对，按“H 元组键 → ReverseHit”建成缓存。
//   逐行投影时（resolveAHeaders）只需在这张缓存里查表，无需再访问数据库。
//
// 【为什么要“先预取”而不是逐行查（性能与正确性）】
//   逐行各发一条带 N 个参数的小查询，对成千上万行会产生海量往返。预取把同一身份
//   下所有去重后的 H 值合并，用 `IN (...)` 或 `OR (col=? AND ...)` 一次性批量取回，
//   再在内存里 O(1) 命中。同时统计每个 H 元组的命中行数，为后续 NOT_FOUND/AMBIGUOUS
//   判定提供依据。
//
// 【SQLite 999 变量上限——为什么要分块（chunk）】
//   预编译语句的绑定参数有上限（maxVars=999）。每行 H 元组占 numSelectCols 个参数，
//   故每批最多放 999/numSelectCols 个元组（chunkSize，至少 1 防除零空批）。超出就
//   再发一批。这是不直观但必须的工程约束。
//
// 【WHERE 子句两种形态】
//   · 单 select 列：用 `selCol IN (?, ?, …)`，每个占位符绑一个 H 值——最紧凑。
//   · 多 select 列：用 `(c1=? AND c2=?) OR (…) OR …`，每个 OR 子句对应一个 H 元组。
//
// 【参数】
//   routes      —— 路由集合（从中筛出 roundtrip=true 的 lookup）。
//   catalog     —— 表结构目录，供 H 值 affinity 强转。
//   db          —— 数据库连接（在其上发预取 SELECT）。
//   sheet       —— 工作表名，用于错误定位。
//   hValueSets  —— 主 SELECT 阶段收集的 H 值集合：identityKey →（元组键 → H 值元组）。
//   cache       —— 出口：建好的 ReverseCache。
//   errors      —— 错误收集器。
//   onPrefetch  —— 可选回调（每发一批预取触发一次，携带 identityKey）；用于进度/测试观测。
// 【返回】成功 true；任一预取 SELECT 失败 → 记 E_REVERSE_LOOKUP_QUERY_FAILED 并返回 false。
// 【错误模式】E_REVERSE_LOOKUP_QUERY_FAILED 是“表级”错误（见 Errors.h）：整张 sheet
//   的导出据此中止（调用方收到 false 后立即返回结果）。
// 【副作用】执行只读 SELECT；填充 *cache；可能向 *errors 追加表级错误。
// 【复杂度】O(Σ 每身份的去重 H 元组数 / chunkSize 批 × 每批结果行)。
bool buildReverseCache(const QVector<RouteSpec>& routes, const SchemaCatalog& catalog,
                       QSqlDatabase& db, const QString& sheet,
                       const QHash<QString, QHash<QString, QVector<QVariant>>>& hValueSets,
                       ReverseCache* cache, ErrorCollector* errors,
                       const std::function<void(const QString&)>& onPrefetch = nullptr) {
    // Collect unique identities (roundtrip=true only)
    // 译：收集唯一身份（仅 roundtrip=true 的 lookup）。语义相同的 lookup 合并为一个身份，
    //     只预取一次、共享缓存桶。
    QHash<QString, LookupSpec> identitySpecs;
    for (const auto& route : routes) {
        for (const LookupSpec& lk : route.lookups) {
            if (!lk.exportRoundtrip)
                continue;  // 非 roundtrip 不做反查（其 H 列原样输出，不需缓存）
            QString ikey = buildIdentityKey(lk);
            if (!identitySpecs.contains(ikey))
                identitySpecs[ikey] = lk;  // 同身份保留首个 spec 即可（语义一致）
        }
    }

    // 逐身份预取。
    for (auto it = identitySpecs.begin(); it != identitySpecs.end(); ++it) {
        const QString& ikey = it.key();
        const LookupSpec& lk = it.value();
        const TableInfo* gTable = catalog.table(lk.fromTable);  // G 表结构（可能为空，做空保护）

        // H-values for this identity collected from the main SELECT
        // 译：本身份在主 SELECT 阶段收集到的全部（去重）H 值元组。
        const QHash<QString, QVector<QVariant>>& keyMap = hValueSets.value(ikey);

        if (keyMap.isEmpty()) {
            (*cache)[ikey] = {};  // 没有任何 H 值要查 → 该身份缓存为空桶（逐行将判 NOT_FOUND）
            continue;
        }

        // Build SELECT: SELECT <match cols>, <select cols> FROM G WHERE <select cols IN ...>
        // 译：构造预取 SELECT——前段取 match 列（A 值），后段取 select 列（H 值），
        //     WHERE 以 select 列（H）作为查询条件。结果列顺序固定为 [match... , select...]，
        //     下面读取结果时依赖这个顺序切分 A 段与 H 段。
        QStringList selectColNames;
        for (const auto& mp : lk.match)
            selectColNames.append(mp.first);  // A 列（G 表里的业务键列）
        for (const auto& sp : lk.select)
            selectColNames.append(sp.first);  // H 列（G 表里的代理主键/查出列）

        int numMatchCols = lk.match.size();
        int numSelectCols = lk.select.size();

        const int maxVars = 999;                           // SQLite 预编译参数上限
        int chunkSize = qMax(1, maxVars / numSelectCols);  // 每批最多放多少个 H 元组（至少 1）

        QHash<QString, ReverseHit> idCache;  // 本身份的缓存桶（边查边填）
        QVector<QString> keyList = keyMap.keys().toVector();  // 待查的所有去重 H 元组键

        // 按 chunkSize 分批，逐批发预取查询。
        for (int start = 0; start < keyList.size(); start += chunkSize) {
            int end = qMin(start + chunkSize, keyList.size());
            int batchSize = end - start;

            // H-10 fix: quote all identifiers in reverse lookup prefetch SQL.
            // 译：H-10——反查预取 SQL 里所有标识符都加引号（防列名/表名含保留字或特殊字符）。
            QStringList quotedSelectCols;
            for (const auto& colName : selectColNames)
                quotedSelectCols.append(SqlBuilder::quoteIdent(colName));
            QString sql = QStringLiteral("SELECT ") + quotedSelectCols.join(QStringLiteral(", ")) +
                          QStringLiteral(" FROM ") + SqlBuilder::quoteIdent(lk.fromTable) +
                          QStringLiteral(" WHERE ");

            if (numSelectCols == 1) {
                // 单列条件：col IN (?, ?, …)，本批每个元组贡献 1 个占位符。
                QStringList placeholders;
                for (int i = 0; i < batchSize; ++i)
                    placeholders.append(QStringLiteral("?"));
                sql += SqlBuilder::quoteIdent(lk.select[0].first) + QStringLiteral(" IN (") +
                       placeholders.join(QStringLiteral(", ")) + QStringLiteral(")");
            } else {
                // 多列条件：每个元组展开成一个 (c1=? AND c2=? …) 子句，子句间用 OR 连接。
                QStringList orClauses;
                for (int i = 0; i < batchSize; ++i) {
                    QStringList andClauses;
                    for (const auto& sp : lk.select)
                        andClauses.append(SqlBuilder::quoteIdent(sp.first) +
                                          QStringLiteral(" = ?"));
                    orClauses.append(QStringLiteral("(") +
                                     andClauses.join(QStringLiteral(" AND ")) +
                                     QStringLiteral(")"));
                }
                sql += orClauses.join(QStringLiteral(" OR "));
            }

            if (onPrefetch)
                onPrefetch(ikey);  // 通知观察者“正在为该身份发起一批预取”

            QSqlQuery q(db);
            q.prepare(sql);
            // 按 keyList 的顺序绑定本批每个 H 元组的各分量值（顺序须与占位符一致）。
            for (int i = start; i < end; ++i) {
                const QVector<QVariant>& vals = keyMap[keyList[i]];
                for (const auto& v : vals)
                    q.addBindValue(v);
            }

            if (!q.exec()) {
                // 表级错误：预取失败 → 整张 sheet 导出中止（见 Errors.h 对该码的说明）。
                errors->addTable(sheet, QString::fromLatin1(err::E_REVERSE_LOOKUP_QUERY_FAILED),
                                 QStringLiteral("Reverse-lookup prefetch failed for '") +
                                     lk.fromTable + QStringLiteral("': ") + q.lastError().text());
                return false;
            }

            // 读取本批结果：每行前 numMatchCols 列是 A 值，紧接 numSelectCols 列是 H 值。
            while (q.next()) {
                // First numMatchCols columns = match (A-values); next numSelectCols = select
                // (H-values)
                // 译：前 numMatchCols 列 = match（A 值）；其后 numSelectCols 列 = select（H 值）。
                QVector<QVariant> matchVals;
                for (int i = 0; i < numMatchCols; ++i)
                    matchVals.append(q.value(i));
                QVector<QVariant> selectVals;
                for (int i = numMatchCols; i < numMatchCols + numSelectCols; ++i)
                    selectVals.append(q.value(i));

                // Apply affinity casting for H-values (used as the cache key)
                // 译：对 H 值做亲和性强转后再建键——必须与逐行侧 collectHValues 用同一规则，
                //     这样“预取建的键”和“逐行查的键”才能精确对上（否则命中漂移）。
                QVector<QVariant> castedHVals;
                bool castOk = true;
                for (int i = 0; i < numSelectCols; ++i) {
                    const ColumnInfo* gCol = gTable ? gTable->column(lk.select[i].first) : nullptr;
                    QVariant c = gCol ? castToAffinity(selectVals[i], *gCol)
                                      : QVariant(selectVals[i].toString());
                    if (!c.isValid()) {
                        castOk = false;  // 该 H 值无法归一 → 此命中作废（与逐行侧口径一致）
                        break;
                    }
                    castedHVals.append(c);
                }
                if (!castOk)
                    continue;

                // 把命中登记进缓存：首个命中记录 A 值；之后只累加命中计数（用于判 AMBIGUOUS）。
                QString tkey = makeTupleKey(castedHVals);
                auto& hit = idCache[tkey];
                if (hit.hitCount == 0)
                    hit.matchVals = matchVals;  // 仅保存首行 A 值；多命中时 A 值无意义（会判歧义）
                hit.hitCount++;
            }
        }

        (*cache)[ikey] = idCache;  // 本身份预取完成，写入总缓存
    }

    return true;
}

// collectHValues —— 从“已载入内存的一行数据”里抽取各 lookup 的 H 值元组，汇入预取集合。
//
// 【做什么/在流程中的位置】这是预取的“收集阶段”：主 SELECT 把每一行装进 rowData
//   （字段名 → 值）后，对每条 roundtrip=true 的 lookup，取出它的 select 目标列
//   （H 列，键名是 sp.second=dbColumn）组成 H 元组，做与缓存侧一致的 affinity 强转，
//   去重后存进 hValueSets。buildReverseCache 随后用这些去重 H 值批量查 G 表。
//
// 【为什么用 sp.second 取值】主 SELECT 经 buildHColSelectSuffix 用 `AS "dbColumn"` 取出
//   H 列，故结果集字段名正是 sp.second——这里据此从 rowData 里取值（与 H-14 fix 呼应）。
//
// 【H-8 fix（保留）：NULL 与空串的区别】只有 SQL NULL 才算“缺失的 H 值”——一旦某分量
//   为 NULL（或强转失败），整条元组作废、跳过本 lookup（hasNull=true）。空字符串 ""
//   是合法的查找键，不算缺失，照常参与。
//
// 【参数】routes/catalog/rowData 同上；hValueSets 为出口（identityKey →（元组键→H 元组））。
// 【返回】void（结果写进 *hValueSets）。【副作用】填充 *hValueSets。【复杂度】O(lookup×select)。
void collectHValues(const QVector<RouteSpec>& routes, const SchemaCatalog& catalog,
                    const QHash<QString, QVariant>& rowData,
                    QHash<QString, QHash<QString, QVector<QVariant>>>* hValueSets) {
    for (const auto& route : routes) {
        for (const LookupSpec& lk : route.lookups) {
            if (!lk.exportRoundtrip)
                continue;  // 仅 roundtrip=true 需要反查（才需收集 H 值）
            QString ikey = buildIdentityKey(lk);
            const TableInfo* gTable = catalog.table(lk.fromTable);

            QVector<QVariant> hVals;
            bool hasNull = false;
            for (const auto& sp : lk.select) {
                QVariant dbVal = rowData.value(sp.second);  // 用 dbColumn 字段名取 H 值
                // H-8 fix: only SQL NULL is a missing H-value; empty string is a valid lookup key.
                // 译：H-8——只有 SQL NULL 才算缺失的 H 值；空字符串是合法查找键。
                if (dbVal.isNull()) {
                    hasNull = true;
                    break;
                }
                const ColumnInfo* gCol = gTable ? gTable->column(sp.first) : nullptr;
                QVariant casted = gCol ? castToAffinity(dbVal, *gCol) : QVariant(dbVal.toString());
                if (!casted.isValid()) {
                    hasNull = true;  // 强转失败同样视作该元组不可用
                    break;
                }
                hVals.append(casted);
            }
            if (hasNull)
                continue;  // 元组含缺失/不可转分量 → 不收集（逐行阶段会按 exportOnMissing 处置）

            // 去重存入：同一身份下相同 H 元组只需查一次 G 表。
            QString tkey = makeTupleKey(hVals);
            auto& idMap = (*hValueSets)[ikey];
            if (!idMap.contains(tkey))
                idMap[tkey] = hVals;
        }
    }
}

// resolveAHeaders —— 逐行解析：用 ReverseCache 把“某一行的 H 值”反查成业务键 A 值。
//
// 【做什么】对本行每条 roundtrip=true 的 lookup：重建该行的 H 元组 → 在缓存里查命中
//   → 据“命中条数”分流为四种情形（NULL/NOT_FOUND/AMBIGUOUS/唯一命中），写出 A 值或
//   按策略报错/跳过。返回的 aVals 是“A-header → 业务键值”的映射，供投影时填进输出列。
//
// 【exportOnMissing 三种策略（来自 ProfileSpec.h 的 ExportOnMissing）如何决定处置】
//   情形①NULL H 值 / 情形②NOT_FOUND（零命中）：
//     · "error" → 记录行级 E_REVERSE_LOOKUP_NOT_FOUND，且整行被跳过（*rowSkip=true，
//                 立即返回）——spec：该行不写入 Excel。
//     · "null" / "skip" → 不报错；该 A-header 缺席（投影时写 NULL），本行其余列照常。
//   情形③AMBIGUOUS（命中>1 行）：
//     · 无论 exportOnMissing 取何值，一律记 E_REVERSE_LOOKUP_AMBIGUOUS 并跳过整行
//       —— 数据本身有歧义，无法在多条匹配里挑一条，只能放弃该行。
//   情形④唯一命中：把 hit.matchVals 按 lk.match 顺序写进 aVals 对应的 A-header。
//
// 【H-04 fix 与 failedAHeaders（重要的语义演进，见调用处）】
//   失败粒度有两套出口：*rowSkip（整行跳过，用于 error 策略下的 NOT_FOUND 和所有
//   AMBIGUOUS）；failedAHeaders（仅记录“哪些 A-header 这条 lookup 失败”，供调用方对
//   “某 lookup 失败”只把对应列写 NULL、而不牵连同行其它列）。本函数当前在 error 策略
//   下仍走整行 rowSkip 路径；failedAHeaders 作为可选出口参数贯通到调用方使用（见
//   run() 内 H-04 注释处）。
//
// 【ctx 与诊断信息】classId 非空时（Mixed 模式）在错误消息前缀 "class 'X' "，便于定位
//   是哪个类别的哪条路由/lookup 出问题；AMBIGUOUS 消息里把元组键的 0x1F 分隔符替换成
//   逗号以便阅读，并提示“去重该 G 表”。
//
// 【参数】
//   routes/catalog/rowData/cache —— 同前；sheet/rowIndex —— 错误定位（rowIndex 为输出行号）。
//   classId        —— Mixed 模式下本行所属类别（非 Mixed 传空串）。
//   errors         —— 错误收集器；rowSkip —— 出口：是否需跳过整行。
//   failedAHeaders —— 可选出口：本行中“查找失败”的 A-header 集合（H-04 细粒度处置用）。
// 【返回】A-header → 业务键值 的映射（仅含唯一命中成功的项）。
// 【副作用】可能向 errors 追加行级错误；写 *rowSkip / *failedAHeaders。
QHash<QString, QVariant> resolveAHeaders(const QVector<RouteSpec>& routes,
                                         const SchemaCatalog& catalog,
                                         const QHash<QString, QVariant>& rowData,
                                         const ReverseCache& cache, const QString& sheet,
                                         int rowIndex, const QString& classId,
                                         ErrorCollector* errors, bool* rowSkip,
                                         QSet<QString>* failedAHeaders = nullptr) {
    QHash<QString, QVariant> aVals;
    *rowSkip = false;

    for (const auto& route : routes) {
        for (const LookupSpec& lk : route.lookups) {
            if (!lk.exportRoundtrip)
                continue;  // 非 roundtrip：H 列原样输出，不在此解析

            QString ikey = buildIdentityKey(lk);
            const TableInfo* gTable = catalog.table(lk.fromTable);

            // Collect H-tuple from this row
            // 译：从本行重建 H 元组（规则与预取侧 collectHValues 完全一致，键才能对上）。
            QVector<QVariant> hVals;
            bool hasNull = false;
            for (const auto& sp : lk.select) {
                QVariant dbVal = rowData.value(sp.second);
                // H-8 fix: only SQL NULL is a missing H-value; empty string is a valid lookup key.
                // 译：H-8——只有 SQL NULL 算缺失；空字符串是合法查找键。
                if (dbVal.isNull()) {
                    hasNull = true;
                    break;
                }
                const ColumnInfo* gCol = gTable ? gTable->column(sp.first) : nullptr;
                QVariant casted = gCol ? castToAffinity(dbVal, *gCol) : QVariant(dbVal.toString());
                if (!casted.isValid()) {
                    hasNull = true;
                    break;
                }
                hVals.append(casted);
            }

            // NULL H-value: treat as miss (exportOnMissing applies)
            // 译：情形①——H 值含 NULL，视为“未命中”，按 exportOnMissing 处置。
            if (hasNull) {
                if (lk.exportOnMissing == QLatin1String(ExportOnMissing::kError)) {
                    QString ctx = classId.isEmpty()
                                      ? QString()
                                      : QStringLiteral("class '") + classId + QStringLiteral("' ");
                    errors->add(sheet, rowIndex, lk.name, QString(),
                                QString::fromLatin1(err::E_REVERSE_LOOKUP_NOT_FOUND),
                                ctx + QStringLiteral("route '") + route.table +
                                    QStringLiteral("' lookup '") + lk.name +
                                    QStringLiteral("': H column is NULL — treating as no match"));
                    // Per spec §exportOnMissing:"error": row SHALL be skipped (not written to
                    // Excel).
                    // 译：按 spec §exportOnMissing:"error"，该行必须被跳过（不写入 Excel）。
                    *rowSkip = true;
                    return aVals;  // 整行作废，无需再看后续 lookup
                }
                // null/skip: leave A-headers absent (will write NULL), no error
                // 译：null/skip 策略——让该 A-header 缺席（投影时写 NULL），不报错，继续下一
                // lookup。
                continue;
            }

            // 在本身份的缓存桶里按 H 元组键查命中。
            QString tkey = makeTupleKey(hVals);
            const auto& idCache = cache.value(ikey);
            auto it = idCache.find(tkey);

            if (it == idCache.end()) {
                // NOT_FOUND
                // 译：情形②——零命中（G 表里查不到该 H 元组），按 exportOnMissing 处置。
                if (lk.exportOnMissing == QLatin1String(ExportOnMissing::kError)) {
                    QString ctx = classId.isEmpty()
                                      ? QString()
                                      : QStringLiteral("class '") + classId + QStringLiteral("' ");
                    // 把 H 元组各分量拼成 "dbColumn=value, …" 的可读描述，便于排查。
                    QStringList hDesc;
                    for (int i = 0; i < lk.select.size() && i < hVals.size(); ++i)
                        hDesc.append(lk.select[i].second + QLatin1Char('=') + hVals[i].toString());
                    errors->add(sheet, rowIndex, lk.name, tkey,
                                QString::fromLatin1(err::E_REVERSE_LOOKUP_NOT_FOUND),
                                ctx + QStringLiteral("route '") + route.table +
                                    QStringLiteral("' lookup '") + lk.name +
                                    QStringLiteral("': no match in '") + lk.fromTable +
                                    QStringLiteral("' for (") + hDesc.join(QStringLiteral(", ")) +
                                    QStringLiteral(")"));
                    // Per spec §exportOnMissing:"error": row SHALL be skipped (not written to
                    // Excel).
                    // 译：按 spec，error 策略下零命中 → 整行跳过、不写入 Excel。
                    *rowSkip = true;
                    return aVals;
                }
                // null/skip: leave A-headers absent (will write NULL), no error
                // 译：null/skip——该 A-header 缺席（写 NULL），不报错，继续。
                continue;
            }

            const ReverseHit& hit = it.value();

            if (hit.hitCount > 1) {
                // AMBIGUOUS — always a whole-row error regardless of exportOnMissing.
                // The data is fundamentally ambiguous: we cannot pick one of multiple matches,
                // so we must skip the entire row (not just the lookup columns).
                // 译：情形③——多命中（AMBIGUOUS）。无论 exportOnMissing 取何值，都视为整行错误：
                //     多条匹配无法择一，数据本质上有歧义，必须跳过整行（而非仅该 lookup 的列）。
                QString ctx = classId.isEmpty()
                                  ? QString()
                                  : QStringLiteral("class '") + classId + QStringLiteral("' ");
                errors->add(sheet, rowIndex, lk.name, tkey,
                            QString::fromLatin1(err::E_REVERSE_LOOKUP_AMBIGUOUS),
                            ctx + QStringLiteral("route '") + route.table +
                                QStringLiteral("' lookup '") + lk.name + QStringLiteral("': ") +
                                QString::number(hit.hitCount) + QStringLiteral(" rows in '") +
                                lk.fromTable + QStringLiteral("' share the same H-value tuple (") +
                                // 显示用：把 0x1F 单元分隔符换成逗号，让元组键人类可读。
                                tkey.replace(QLatin1Char('\x1F'), QLatin1Char(',')) +
                                QStringLiteral("); deduplicate '") + lk.fromTable +
                                QStringLiteral("'"));
                *rowSkip = true;  // AMBIGUOUS: whole row must be skipped
                                  // 译：歧义 → 整行必须跳过。
                continue;
            }

            // Success: store A-values
            // 译：情形④——唯一命中。把缓存里取回的 A 值（业务键）按 match 顺序写进 aVals，
            //     键为对应的 Excel 表头（A-header = lk.match[i].second）。
            for (int i = 0; i < lk.match.size() && i < hit.matchVals.size(); ++i)
                aVals[lk.match[i].second] = hit.matchVals[i];
        }
    }

    return aVals;
}

}  // anonymous namespace

// execAndWrite —— 三条写出路径中的“流式路径”：边查边写，全程零额外内存。
//
// 【何时走这条路】无 columnOrder 且无反向查找时（最省内存、最快）。此时输出列序就是
//   SELECT 的自然列序，无需把全部行先缓存到内存里再处理——每查出一行就立刻写一行。
//
// 【做什么】执行 sql → 取结果集字段名作表头（可选写出）→ 逐行：对登记在 temporal 里的
//   列做 DB→Excel 时间转换，其余列原样 → 写一行 → 累加 rowCount。
//
// 【错误模式】SELECT 执行失败 → 记“表级” E_EXPORT_QUERY 并返回 false（导出中止）。
//   逐行的时间解析失败由 convertTemporalForExport 记“行级”非阻断 E_TIME_PARSE_DB，
//   不影响本函数继续。
//
// 【M-1（保留）】传 *rowCount+1 作为输出行号，使时间解析错误携带真实 Excel 行号。
// 【参数】
//   sql/sheet/db —— 查询语句/表名/连接；writer —— 输出器；
//   writeHeader  —— 是否在此写表头（混合分支可能由外部统一写，故可关）；
//   outHeaders   —— 可选出口：把表头回传给调用方；
//   temporal     —— 列名→时间转换规格速查表；errors —— 错误收集器；
//   rowCount     —— 进出口：已写行数（兼作行号基准，逐行 +1）。
// 【返回】成功 true；查询失败 false。【副作用】写 writer、累加 *rowCount、可能记错误。
// 【复杂度】O(结果行数 × 列数)，内存 O(1)（不缓存行）。
static bool execAndWrite(const QString& sql, const QString& sheet, QSqlDatabase& db,
                         ExcelWriter& writer, bool writeHeader, QStringList* outHeaders,
                         const QHash<QString, TemporalColumnInfo>& temporal, ErrorCollector* errors,
                         int* rowCount) {
    QSqlQuery q(db);
    if (!q.exec(sql)) {
        // 表级错误：SELECT 失败 → 整张表导出中止（消息里附上 SQL 便于排查）。
        errors->addTable(sheet, QString::fromLatin1(err::E_EXPORT_QUERY),
                         QStringLiteral("Query failed: ") + q.lastError().text() +
                             QStringLiteral(" SQL: ") + sql);
        return false;
    }

    // 表头 = 结果集各字段名（自然列序）。
    QSqlRecord rec = q.record();
    QStringList headers;
    for (int i = 0; i < rec.count(); ++i)
        headers.append(rec.fieldName(i));

    if (writeHeader) {
        writer.writeHeader(headers);
        if (outHeaders)
            *outHeaders = headers;  // 回传给调用方（混合分支可能需要）
    }

    // 逐行流式写出。
    while (q.next()) {
        QVector<QVariant> row;
        for (int i = 0; i < rec.count(); ++i) {
            QVariant val = q.value(i);
            const QString& fieldName = headers[i];
            if (temporal.contains(fieldName))
                // 时间列：DB→Excel 转换；解析失败该格写空、整行继续。
                val = convertTemporalForExport(val, temporal[fieldName], sheet, fieldName, errors,
                                               *rowCount + 1);  // M-1: pass output row number
                                                                // 译：M-1 传输出行号。
            row.append(val);
        }
        writer.writeRow(row);
        (*rowCount)++;
    }
    return true;
}

// ExportService::run —— 导出总入口。完整职责/参数/错误模式/线程语义见 ExportService.h，
// 此处按“流程顺序”逐段讲解实现。返回 ExportResult（ok / 写出行数 / errors / warnings）。
ExportResult ExportService::run(const ProfileSpec& profile, const SchemaCatalog& catalog,
                                const QString& xlsxPath, const ExportOptions& options,
                                QSqlDatabase& db) {
    ExportResult result;
    ErrorCollector errors;  // 全程累积行级/表级错误与警告，最后汇入 result

    // 工作表名：优先用调用方 options 指定的，否则回退 Profile 默认 sheet。
    QString sheetName = options.sheetName.isEmpty() ? profile.sheet : options.sheetName;

    // 第 1 步：打开输出 .xlsx。失败即表级错误 E_WRITE_XLSX，无法继续 → 立即返回。
    ExcelWriter writer;
    QString writerErr;
    if (!writer.open(xlsxPath, sheetName, &writerErr)) {
        errors.addTable(sheetName, QString::fromLatin1(err::E_WRITE_XLSX), writerErr);
        result.errors = errors.list();
        return result;
    }

    // 第 2 步：预扫 Profile，建“列名→时间转换规格”速查表（后续所有路径共用）。
    QHash<QString, TemporalColumnInfo> temporal = buildTemporalExportMap(profile);

    SqlBuilder sqlBuilder;  // 生成自动 JOIN 的 SELECT
    TopoSorter topoSorter;  // 多表路由按外键依赖排拓扑序
    int rowCount = 0;       // 累计写出行数，兼作行号基准

    // 第 3 步：按映射模式分流。Mixed 与 Single/MultiTable 走两套大分支。
    if (profile.mode == ProfileMode::Mixed) {
        // --- Mixed export ---
        // 译：混合模式导出。特点：行被分到不同 class，各 class 有各自的 routes；
        //     需逐 class 分别查询、把各 class 的自然列“并集”成统一表头，再合并写出。
        //     另可在输出里附一列“类别 id”（classColumn）。

        // MixedRow —— 暂存一行：它属于哪个 class（classId）+ 该行的字段名→值映射（data）。
        struct MixedRow {
            QString classId;
            QHash<QString, QVariant> data;
        };
        QVector<MixedRow> allRows;  // 所有 class 的行汇总（混合模式需全量载入内存）
        QStringList allHeaders;     // natural SQL field names in first-appearance order
                                 // 译：各 class 自然列名的并集，按“首次出现”顺序排列。
        QSet<QString> headerSet;  // 配合 allHeaders 做 O(1) 去重

        // Collect sorted routes per class (needed for reverse lookup too)
        // 译：先为每个 class 把它的 routes 做拓扑排序（反向查找也要用这份排序结果）。
        //     拓扑排序保证多表写入/查询时父表在子表之前；成环则无法定序。
        QHash<QString, QVector<RouteSpec>> classSortedRoutes;
        for (const auto& cls : profile.classes) {
            QVector<RouteSpec> sorted;
            QString topoErr;
            if (!topoSorter.sort(cls.routes, &sorted, &topoErr)) {
                // 表级错误：路由依赖成环（E_PROFILE_TOPOLOGY_CYCLE）→ 无法导出，立即返回。
                errors.addTable(sheetName, QString::fromLatin1(err::E_PROFILE_TOPOLOGY_CYCLE),
                                topoErr);
                result.errors = errors.list();
                return result;
            }
            classSortedRoutes[cls.id] = sorted;
        }

        // Check if any class has reverse lookup H-cols to fetch (roundtrip=true or false).
        // 译：判断是否有任一 class 含“带 select 的 lookup”——只要有，就需要扩展 SQL 多取 H 列
        //     （注意：roundtrip 真假都算，理由见 hasAnyLookupHCols 注释）。
        bool needReverseLookupMixed = false;
        for (const auto& cls : profile.classes) {
            if (hasAnyLookupHCols(classSortedRoutes[cls.id])) {
                needReverseLookupMixed = true;
                break;
            }
        }

        // Load rows per class; extend SQL with H-cols if reverse lookup is active
        // 译：逐 class 查询并把行载入内存；若需反向查找则扩展 SQL 以多取 H 列。
        QHash<QString, QHash<QString, QVector<QVariant>>>
            hValueSets;  // for prefetch
                         // 译：供后续预取的 H 值集合。
        for (const auto& cls : profile.classes) {
            const QVector<RouteSpec>& sorted = classSortedRoutes[cls.id];
            QString sql = sqlBuilder.buildAutoJoinSelect(sorted, profile.exportSpec);
            if (sql.isEmpty())
                continue;  // 该 class 无可查内容（无根路由等）→ 跳过

            // Extend SQL with H-cols if needed (6.1: sheet-level H-value collection)
            // 译：需要时把 H 列追加进 SELECT（步骤 6.1：在 sheet 级统一收集 H 值）。
            if (needReverseLookupMixed) {
                QString hSuffix = buildHColSelectSuffix(sorted);
                sql = extendSqlWithHCols(sql, hSuffix);
            }

            QSqlQuery q(db);
            if (!q.exec(sql)) {
                // 表级错误：该 class 的 SELECT 失败 → 整张表导出中止。
                errors.addTable(sheetName, QString::fromLatin1(err::E_EXPORT_QUERY),
                                QStringLiteral("Query failed: ") + q.lastError().text());
                result.errors = errors.list();
                return result;
            }

            QSqlRecord rec = q.record();
            QSet<QString> clsHCols = buildHColReplaceSet(sorted);  // 本 class 要被替换掉的 H 列

            // Collect natural headers (exclude H-cols-to-replace from allHeaders)
            // 译：收集“自然列”表头并入并集——但要排除 roundtrip=true 的 H 列（它们将被业务键
            //     替换，不应作为输出列；改由 A-header 承载）。
            for (int i = 0; i < rec.count(); ++i) {
                QString h = rec.fieldName(i);
                if (clsHCols.contains(h))
                    continue;  // H-cols tracked separately, not in allHeaders
                               // 译：被替换的 H 列单独追踪，不进 allHeaders。
                if (!headerSet.contains(h)) {
                    headerSet.insert(h);
                    allHeaders.append(h);  // 首次出现才追加，保证并集稳定有序
                }
            }

            // 把本 class 的每一行装进 allRows；若需反查则顺带收集其 H 值供预取。
            while (q.next()) {
                MixedRow row;
                row.classId = cls.id;
                for (int i = 0; i < rec.count(); ++i)
                    row.data[rec.fieldName(i)] = q.value(i);  // 字段名（含别名）→ 值
                if (needReverseLookupMixed)
                    collectHValues(sorted, catalog, row.data, &hValueSets);
                allRows.append(row);
            }
        }

        // 6.1: sheet-level reverse prefetch (merged across classes)
        // 译：步骤 6.1——sheet 级反向预取（跨 class 合并）。把所有 class 的路由汇到一起，
        //     一次性建好整张 ReverseCache（同身份的 lookup 自然合并，避免重复查 G 表）。
        ReverseCache revCache;
        if (needReverseLookupMixed) {
            // Collect all routes from all classes for prefetch
            // 译：汇总所有 class 的路由用于预取。
            QVector<RouteSpec> allClassRoutes;
            for (const auto& cls : profile.classes)
                for (const auto& r : classSortedRoutes[cls.id])
                    allClassRoutes.append(r);

            if (!buildReverseCache(allClassRoutes, catalog, db, sheetName, hValueSets, &revCache,
                                   &errors)) {
                // 预取失败是表级错误（E_REVERSE_LOOKUP_QUERY_FAILED）→ 中止导出。
                result.errors = errors.list();
                return result;
            }
        }

        // M-02 fix: removed in-memory string sort for orderBy.
        // Per export-column-order spec "Orthogonality with orderBy", orderBy only affects
        // SQL-level row ordering (applied per-class in each SQL query); a secondary
        // string-comparison sort across merged classes violates SQLite collation semantics
        // (numeric, date, NULL ordering differs from lexicographic string comparison).
        // 译：M-02——移除了曾经针对 orderBy 的“内存字符串排序”。
        //   依 export-column-order 规范“与 orderBy 正交”一节：orderBy 只影响 SQL 层的行序
        //   （已在每个 class 的 SQL 查询里各自施加）；若再跨 class 合并后做一次“按字符串比较”
        //   的二次排序，会违背 SQLite 的排序规则语义——数值、日期、NULL 的排序都与字典序不同。
        //   因此这里刻意“不排序”，保持各 class SQL 已定的次序合并即可。

        // Compute effective output headers for reverse-lookup mode
        // 译：为反向查找模式计算“有效输出表头”——把各 class 的 A-header 并集，以及“被替换的
        //     H 列”并集分别算出（后者用于投影时识别哪些列不应直接输出）。
        QStringList mixedAllAHeaders;       // 跨 class 的 A-header 并集（保序）
        QSet<QString> mixedAllHColReplace;  // 跨 class 被替换的 H 列并集
        if (needReverseLookupMixed) {
            for (const auto& cls : profile.classes) {
                for (const QString& ah : buildAHeaders(classSortedRoutes[cls.id]))
                    if (!mixedAllAHeaders.contains(ah))
                        mixedAllAHeaders.append(ah);
                for (const QString& h : buildHColReplaceSet(classSortedRoutes[cls.id]))
                    mixedAllHColReplace.insert(h);
            }
        }

        // Apply columnOrder to allHeaders + A-headers; then handle classColumn placement.
        // 译：把 columnOrder 应用到 (自然列并集 + A-header 并集) 上，再处理“类别列”的摆放。
        const QStringList& colOrder = profile.exportSpec.columnOrder;
        QString classCol = profile.exportSpec.classColumn;  // 写类别 id 的合成列名（可空）

        // 有效列 = 自然列并集 ∪ A-header（A-header 追加在后，去重）。
        QStringList effectiveHeaders = allHeaders;
        for (const QString& ah : mixedAllAHeaders)
            if (!effectiveHeaders.contains(ah))
                effectiveHeaders.append(ah);

        // Reorder according to columnOrder
        // 译：按 columnOrder 重排（点名者在前，其余按原相对序在后；见 reorderHeaders）。
        QStringList finalHeaders = reorderHeaders(effectiveHeaders, colOrder);

        // classColumn placement: prepend if not explicitly positioned
        // 译：类别列的摆放——若用户没在 columnOrder 里显式给它定位，则置于最前（prepend）；
        //     若 columnOrder 已点名它，则尊重用户给的位置（不重复前插）。
        bool classColInOrder = !classCol.isEmpty() && colOrder.contains(classCol);
        if (!classCol.isEmpty() && !classColInOrder)
            finalHeaders.prepend(classCol);

        writer.writeHeader(finalHeaders);  // 混合模式：表头在此统一写出（各 class 已合并）
        // 逐行投影写出：把每一行按 finalHeaders 的列序，逐列取值/转换/反查后写一行。
        for (const auto& mr : allRows) {
            // 6.2: per-row resolution uses class-specific routes
            // 译：步骤 6.2——逐行解析时用“该行所属 class 的路由”（不同 class 的 lookup 不同）。
            bool rowSkip = false;
            QSet<QString> failedAHeaders;
            QHash<QString, QVariant> aVals;
            if (needReverseLookupMixed) {
                const QVector<RouteSpec>& clsRoutes = classSortedRoutes[mr.classId];
                // H-04 fix: collect per-lookup failures in failedAHeaders instead of whole rowSkip.
                // 译：H-04——把“每条 lookup 的失败”收集进
                // failedAHeaders（细粒度），而非一律整行跳过；
                //     rowSkip 仅用于 error 策略的 NOT_FOUND 与所有 AMBIGUOUS（见
                //     resolveAHeaders）。
                aVals =
                    resolveAHeaders(clsRoutes, catalog, mr.data, revCache, sheetName, rowCount + 1,
                                    mr.classId, &errors, &rowSkip, &failedAHeaders);
            }
            if (rowSkip)
                continue;  // 整行作废（不写入 Excel）

            // 按最终列序逐列产出本行的值。
            QVector<QVariant> rowVals;
            for (const auto& h : finalHeaders) {
                if (h == classCol) {
                    // 合成的“类别列”：写本行所属 class 的 id。
                    rowVals.append(mr.classId);
                } else if (mixedAllHColReplace.contains(h)) {
                    rowVals.append(QVariant());  // should not appear in finalHeaders
                                                 // 译：被替换的 H 列本不该出现在 finalHeaders 里，
                                                 //     这里是防御性写 NULL（理论不可达）。
                } else if (needReverseLookupMixed && mixedAllAHeaders.contains(h)) {
                    // 该列是反查得到的 A-header（业务键列）。
                    if (failedAHeaders.contains(h)) {
                        // H-04 fix: only null out the columns whose lookup failed.
                        // 译：H-04——只把“查找失败的那一列”写 NULL，本行其它列不受牵连。
                        rowVals.append(QVariant());
                    } else {
                        // D5: ColumnSpec.source value wins if non-NULL
                        // 译：规则 D5——若该列在源数据 mr.data 里本就有非 NULL 值，则“源值优先”，
                        //     否则才用反查得到的业务键 aVals。
                        QVariant srcVal = mr.data.value(h);
                        QVariant val = (!srcVal.isNull()) ? srcVal : aVals.value(h, QVariant());
                        if (!val.isNull() && temporal.contains(h))
                            val = convertTemporalForExport(val, temporal[h], sheetName, h, &errors,
                                                           rowCount + 1);
                        rowVals.append(val);
                    }
                } else {
                    // 普通自然列：直接取源值，必要时做时间转换。
                    QVariant val = mr.data.value(h, QVariant());
                    if (!val.isNull() && temporal.contains(h))
                        val = convertTemporalForExport(val, temporal[h], sheetName, h, &errors,
                                                       rowCount + 1);
                    rowVals.append(val);
                }
            }
            writer.writeRow(rowVals);
            rowCount++;
        }
    } else {
        // --- SingleTable / MultiTable export ---
        // 译：单表 / 多表模式导出。
        // 第一步：拿到要执行的 SELECT。两种来源——
        //   (a) Profile 提供了原生 SQL（explicitSql）→ 直接用，不做拓扑排序、不重排、不反查；
        //   (b) 否则由 SqlBuilder 依拓扑排序后的 routes 自动生成 JOIN SELECT。
        QVector<RouteSpec> sorted;
        QString sql;
        if (!profile.exportSpec.explicitSql.isEmpty()) {
            sql = profile.exportSpec.explicitSql;
        } else {
            QString topoErr;
            if (!topoSorter.sort(profile.routes, &sorted, &topoErr)) {
                // 表级错误：路由成环，无法定序 → 立即返回。
                errors.addTable(sheetName, QString::fromLatin1(err::E_PROFILE_TOPOLOGY_CYCLE),
                                topoErr);
                result.errors = errors.list();
                return result;
            }
            sql = sqlBuilder.buildAutoJoinSelect(sorted, profile.exportSpec);
        }

        // needReverseLookup: true if any lookup has select columns (H-cols) to retrieve.
        // hasAnyLookupHCols() covers both roundtrip=true (replacement) and roundtrip=false (raw
        // H-col output). buildReverseCache() internally skips non-roundtrip lookups so the
        // expensive prefetch is a no-op when all lookups have exportRoundtrip=false.
        // M-07 note: the real performance win (skip buildReverseCache entirely for roundtrip=false)
        // could be applied but is out of scope here since buildReverseCache is already selective.
        // 译：needReverseLookup —— 是否“需要取 H 列”。只要有 lookup 声明了 select 列就为真，
        //   它同时覆盖 roundtrip=true（要把 H 替换成业务键）与 roundtrip=false（H 原样输出）。
        //   buildReverseCache 内部会跳过非 roundtrip 的 lookup，故当所有 lookup 都 roundtrip=false
        //   时，那次昂贵的预取实际是空操作。
        //   M-07 备注：进一步优化（roundtrip=false 时干脆完全跳过 buildReverseCache）是可行的，
        //   但因 buildReverseCache 已是“按需选择性”执行，此处不做，留待后续。
        //   另：使用原生 SQL 时不支持反向查找（列名/结构由用户自定，引擎无从扩展），故置 false。
        const bool needReverseLookup =
            !profile.exportSpec.explicitSql.isEmpty() ? false : hasAnyLookupHCols(sorted);

        // 第二步：在“是否有 columnOrder”× “是否需反查”二维上选 (a)/(b)/(c) 三条写出路径之一。
        if (profile.exportSpec.columnOrder.isEmpty() && !needReverseLookup) {
            // Streaming path: no columnOrder, no reverse lookup — zero extra memory.
            // 译：路径(a) 流式——无列重排、无反查，边查边写，零额外内存（最省，见 execAndWrite）。
            QStringList headers;
            if (!execAndWrite(sql, sheetName, db, writer, true, &headers, temporal, &errors,
                              &rowCount)) {
                result.errors = errors.list();
                return result;
            }
        } else if (!needReverseLookup) {
            // columnOrder-only path: load all rows into memory, reorder columns, then write.
            // 译：路径(b) 仅列重排——有 columnOrder 但无反查。必须先全量载入内存（因为重排需要
            //   先知道所有自然列），再按 columnOrder 重排列序后逐行写出。
            QSqlQuery q(db);
            if (!q.exec(sql)) {
                // 表级错误：SELECT 失败 → 中止。
                errors.addTable(sheetName, QString::fromLatin1(err::E_EXPORT_QUERY),
                                QStringLiteral("Query failed: ") + q.lastError().text() +
                                    QStringLiteral(" SQL: ") + sql);
                result.errors = errors.list();
                return result;
            }

            // 自然列序 = 结果集字段名顺序。
            QSqlRecord rec = q.record();
            QStringList naturalHeaders;
            for (int i = 0; i < rec.count(); ++i)
                naturalHeaders.append(rec.fieldName(i));

            // 全量载入：逐行取值并在“载入时”就做时间转换（这样后续重排只是搬运，不再触碰值）。
            // 注意时间转换需携带真实行号——这里用独立计数器 colOrderLoadRow（而非 rowCount，
            // 因为 rowCount 在“写出”阶段才递增，载入阶段尚未开始计数写出行）。
            QVector<QVector<QVariant>> rows;
            int colOrderLoadRow = 0;
            while (q.next()) {
                QVector<QVariant> row;
                ++colOrderLoadRow;
                for (int i = 0; i < rec.count(); ++i) {
                    QVariant val = q.value(i);
                    const QString& fieldName = naturalHeaders[i];
                    if (temporal.contains(fieldName))
                        val = convertTemporalForExport(
                            val, temporal[fieldName], sheetName, fieldName, &errors,
                            colOrderLoadRow);  // M-1: pass row
                                               // 译：M-1 传真实载入行号。
                    row.append(val);
                }
                rows.append(row);
            }

            // 计算最终列序（columnOrder 点名者在前），并建“列名→自然列下标”映射以便 O(1) 取列。
            QStringList finalHeaders =
                reorderHeaders(naturalHeaders, profile.exportSpec.columnOrder);
            QHash<QString, int> naturalIdx;
            for (int i = 0; i < naturalHeaders.size(); ++i)
                naturalIdx[naturalHeaders[i]] = i;

            // 写表头，再逐行“按最终列序搬运”各列值（找不到的列写 NULL，理论上不会发生）。
            writer.writeHeader(finalHeaders);
            for (const auto& row : rows) {
                QVector<QVariant> reordered;
                for (const QString& h : finalHeaders) {
                    int idx = naturalIdx.value(h, -1);
                    reordered.append(idx >= 0 ? row[idx] : QVariant());
                }
                writer.writeRow(reordered);
                rowCount++;
            }
        } else {
            // Reverse-lookup path: extend SQL with H-cols, full-load, prefetch, project.
            // 译：路径(c) 反向查找——四步走：扩展 SQL 取 H 列 → 全量载入 → 批量预取 → 逐行投影。
            // 5.2: Extend SQL to also retrieve H-col values from lookup.select targets.
            // 译：步骤 5.2——扩展 SELECT，使其额外取出各 lookup.select 目标（H 列）的值。
            QString hColSuffix = buildHColSelectSuffix(sorted);
            QString extSql = extendSqlWithHCols(sql, hColSuffix);

            QSqlQuery q(db);
            if (!q.exec(extSql)) {
                // 表级错误：扩展后的 SELECT 失败 → 中止。
                errors.addTable(sheetName, QString::fromLatin1(err::E_EXPORT_QUERY),
                                QStringLiteral("Query failed: ") + q.lastError().text() +
                                    QStringLiteral(" SQL: ") + extSql);
                result.errors = errors.list();
                return result;
            }

            // 结果集全部字段名（含自然列 + 额外取出的 H 列）。
            QSqlRecord rec = q.record();
            QStringList allSqlHeaders;
            for (int i = 0; i < rec.count(); ++i)
                allSqlHeaders.append(rec.fieldName(i));

            QSet<QString> hColReplaceSet = buildHColReplaceSet(sorted);  // 将被业务键替换的 H 列

            // 5.3: Load all rows; collect H-value sets for reverse prefetch.
            // 译：步骤 5.3——全量载入每一行（字段名→值），同时收集各行的 H 值供预取。
            //   注意此路径不在载入时做时间转换（留到投影阶段统一做），因为列要先经反查/重排。
            QVector<QHash<QString, QVariant>> rowDataList;
            QHash<QString, QHash<QString, QVector<QVariant>>> hValueSets;
            while (q.next()) {
                QHash<QString, QVariant> rowData;
                for (int i = 0; i < rec.count(); ++i)
                    rowData[allSqlHeaders[i]] = q.value(i);
                collectHValues(sorted, catalog, rowData, &hValueSets);
                rowDataList.append(rowData);
            }

            // 5.4: Run reverse prefetch.
            // 译：步骤 5.4——批量预取，建好 ReverseCache（失败为表级错误，中止）。
            ReverseCache revCache;
            if (!buildReverseCache(sorted, catalog, db, sheetName, hValueSets, &revCache,
                                   &errors)) {
                result.errors = errors.list();
                return result;
            }

            // Compute output headers: natural cols (excluding roundtrip=true H-cols) + A-headers.
            // Natural: SQL result headers NOT in hColReplaceSet.
            // 译：计算输出表头——自然列（剔除将被替换的 roundtrip=true H 列）并上 A-header。
            //   自然列 = 结果集字段中“不在 hColReplaceSet”的那些。
            QStringList naturalHeaders;
            for (const QString& h : allSqlHeaders) {
                if (!hColReplaceSet.contains(h))
                    naturalHeaders.append(h);
            }
            QStringList aHeaders = buildAHeaders(sorted);  // 要写回的业务键表头
            QStringList effectiveHeaders = naturalHeaders;
            for (const QString& ah : aHeaders) {
                if (!effectiveHeaders.contains(ah))
                    effectiveHeaders.append(ah);  // A-header 追加在自然列之后
            }
            // 按 columnOrder 重排，得到最终列序。
            QStringList finalHeaders =
                reorderHeaders(effectiveHeaders, profile.exportSpec.columnOrder);

            writer.writeHeader(finalHeaders);

            // 5.5-5.7: Project rows.
            // 译：步骤 5.5~5.7——逐行投影：先反查得到 A 值，再按最终列序产出每行各列。
            for (const auto& rowData : rowDataList) {
                bool rowSkip = false;
                QSet<QString> failedAHeaders;
                // H-04 fix: pass failedAHeaders to collect per-lookup failures instead of skipping
                // the whole row on NOT_FOUND.  rowSkip is only set for AMBIGUOUS results.
                // 译：H-04——传入 failedAHeaders 以“按列”收集查找失败，而不在 NOT_FOUND 时整行跳过；
                //   rowSkip 仅在 AMBIGUOUS（以及 error 策略下的 NOT_FOUND）时被置位。
                QHash<QString, QVariant> aVals =
                    resolveAHeaders(sorted, catalog, rowData, revCache, sheetName, rowCount + 1,
                                    QString(), &errors, &rowSkip, &failedAHeaders);
                if (rowSkip)
                    continue;  // 整行作废

                // 按最终列序逐列产出本行的值。
                QVector<QVariant> outRow;
                for (const QString& h : finalHeaders) {
                    QVariant val;
                    if (hColReplaceSet.contains(h)) {
                        // H-col-to-replace: should not appear in finalHeaders, skip
                        // 译：被替换的 H 列本不应出现在 finalHeaders（防御性写 NULL，理论不可达）。
                        val = QVariant();
                    } else if (aHeaders.contains(h)) {
                        // 该列是反查得到的业务键 A-header。
                        if (failedAHeaders.contains(h)) {
                            // H-04 fix: this A-header's lookup failed — write NULL for this
                            // column only; other columns in the same row are unaffected.
                            // 译：H-04——该 A-header 的查找失败，仅此列写 NULL，本行其它列不受影响。
                            val = QVariant();
                        } else {
                            // 5.6: D5 — ColumnSpec.source value wins if present and non-NULL
                            // 译：步骤 5.6 / 规则 D5——源数据该列若非 NULL
                            // 则“源值优先”，否则用反查值。
                            QVariant srcVal = rowData.value(h);
                            if (!srcVal.isNull())
                                val = srcVal;
                            else
                                val = aVals.value(h, QVariant());
                        }
                    } else {
                        // 普通自然列：取源值。
                        val = rowData.value(h, QVariant());
                    }
                    // 统一在投影阶段做时间转换（本路径未在载入时转换）。
                    if (!val.isNull() && temporal.contains(h))
                        val = convertTemporalForExport(val, temporal[h], sheetName, h, &errors,
                                                       rowCount + 1);  // M-1
                    outRow.append(val);
                }
                writer.writeRow(outRow);
                rowCount++;
            }
        }
    }

    // 第 4 步：把缓冲的内容落盘成 .xlsx。失败为表级错误 E_WRITE_XLSX → 返回。
    if (!writer.save(&writerErr)) {
        errors.addTable(sheetName, QString::fromLatin1(err::E_WRITE_XLSX), writerErr);
        result.errors = errors.list();
        return result;
    }

    // 第 5 步：组装结果。注意：即便有行级错误（NOT_FOUND/时间解析等），ok 仍为 true——
    //   这些是“非阻断”的部分失败，文件已成功写出；调用方应检查 errors 列表了解明细。
    //   （真正阻断的表级错误在前面的各分支里已提前 return，根本走不到这里。）
    result.ok = true;
    result.writtenRows = rowCount;  // 实际写出的行数（被跳过的行不计）
    result.errors = errors.list();  // 行级 + 表级错误清单
    // Merge runtime warnings with profile load-time diagnostics
    // 译：把“运行期警告”与“Profile 加载期诊断”合并，统一通过 warnings 回报给调用方。
    result.warnings = errors.warnings();
    for (const QString& w : profile.loadWarnings) {
        // 加载期诊断（如“dateFormat 覆盖了 date: 校验器”）包装成统一的 RowError 形态，
        // 用占位码 W_PROFILE_LOAD 标识其来源是 Profile 加载阶段。
        RowError re;
        re.sheet = profile.sheet;
        re.code = QStringLiteral("W_PROFILE_LOAD");
        re.message = w;
        result.warnings.append(re);
    }
    return result;
}

}  // namespace dbridge::detail
