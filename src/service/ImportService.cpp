#include "ImportService.h"

#include "dbridge/Errors.h"

#include <QSet>
#include <QSqlError>
#include <QSqlQuery>

#include "ErrorCollector.h"
#include "excel/ExcelReader.h"  // 读 .xlsx：选 sheet、读表头、按表头名取单元格
#include "mapping/BatchUniqueness.h"  // 批内唯一性检查（同一批数据里冲突键不得重复）
#include "mapping/FkInjector.h"  // 外键注入：把父表查得的主键写进子表 binds
#include "mapping/Mapper.h"      // 把一行 Excel 映射成多路由载荷 + 行级校验
#include "mapping/Router.h"      // Mixed 模式：按判别列把行分派到某个 class
#include "mapping/TopoSorter.h"  // 按外键依赖把各路由排成拓扑序（父先于子）
#include "profile/ProfileValidator.h"  // 载入期校验 Profile 与表头/表结构是否自洽
#include "schema/SchemaCatalog.h"      // 数据库表结构目录（列类型/约束）
#include "sql/SqlBuilder.h"            // 生成 UPSERT/标识符转义等 SQL 文本
#include "validation/ForeignKeyPreflight.h"  // 写库前的外键存在性预检
#include <functional>

// ============================================================================
// ImportService.cpp — 导入编排层实现（Excel → SQLite 的完整 ETL 流水线）
// ============================================================================
//
// 【全局流水线一图流（与 run() 中的 Phase 标注一一对应）】
//   Phase A   ExcelReader 打开文件 → 选 sheet → 读表头
//   Phase B   ProfileValidator 校验；Mapper 编译校验器；TopoSorter 拓扑排序路由
//   Phase A.5 buildLookupCache：把所有“正向查找”按身份去重，批量 SELECT 建缓存
//   Phase C   逐行：Router 分派(Mixed) → Mapper.map 生成载荷+校验 → applyLookups
//             套用查找结果 → FkInjector.inject 外键注入 → BatchUniqueness 批内查重
//             → 收齐后 ForeignKeyPreflight 做外键存在性预检
//   Phase D   单事务内按拓扑序对各路由 UPSERT；dryRun 则跳过本阶段只回报载荷
//
// 【贯穿全文的“部分成功”模型（务必先理解，否则读不懂跳过逻辑）】
//   一行 Excel = 一个 RowContext，内部含多个 RoutePayload（每个写一张表）。错误分两类：
//     · 路由局部错误 —— 只影响某条路由：记入 ctx.failedRouteIndices。
//       写阶段只跳过这条路由“及其子孙路由”（父没写成，子的外键就无从谈起），
//       兄弟路由若数据完好仍照常写入。
//     · 非路由错误（hasNonRouteError）—— 结构性/无法绑定，整行载荷已不可用：整行跳过。
//   这套区分是 RowPayload.h 里 H-01/H-04/M-04 等修复的核心，下文多处呼应。
//
// 【命名空间】实现细节均在 dbridge::detail；文件内匿名 namespace 里放只供本文件用的 helper。
// ============================================================================

namespace dbridge::detail {

namespace {

// ---- Lookup helpers --------------------------------------------------------
// 译：外键“正向查找”辅助函数（正向 = 用 Excel 里的业务键，查参照表 G 的代理主键等列）。

// Build a stable identity key for a LookupSpec: (from, match-pairs, select-pairs).
// Two lookups sharing the same identity key can reuse one prefetch result.
// 译：为一个 LookupSpec 生成稳定的“身份键”，由三部分拼成：参照表名 + 匹配对(match) +
//     选取对(select)。两个查找若身份键相同，说明它们“查同一张表、用同样的列匹配、取同样
//     的列”，于是可以共享同一次预取结果（见 Phase A.5），避免重复 SELECT。
//     形如：  G_table::gcol=hdr,gcol2=hdr2::gsel->dbcol
QString buildIdentityKey(const LookupSpec& lk) {
    QStringList matchParts, selectParts;
    for (const auto& p : lk.match)
        matchParts.append(p.first + QLatin1Char('=') + p.second);
    for (const auto& p : lk.select)
        selectParts.append(p.first + QStringLiteral("->") + p.second);
    return lk.fromTable + QStringLiteral("::") + matchParts.join(QLatin1Char(',')) +
           QStringLiteral("::") + selectParts.join(QLatin1Char(','));
}

// castToAffinity —— 把一个原始 QVariant 强制转成「参照表 G 某列的 SQLite 亲和性(affinity)」类型。
// Cast a raw QVariant to the affinity of a G-table column.
// Returns an invalid QVariant on type-mismatch (cast failure).
// Numeric zero is a valid (non-empty) value.
// 【译】按 G 表列的亲和性转换原始值；类型不符（转换失败）返回无效 QVariant；数值 0
// 是有效（非空）值。 【为什么必须做亲和性转换】Excel
// 单元格读出来的值类型往往是「文本」，而库里匹配列可能是 INTEGER。
//   若不转换，用文本 "100" 去和库里的整数 100 做相等匹配会失配（SQLite 严格相等区分类型）。这里
//   按列的声明类型把值「掰」成同一亲和性，使后续 makeTupleKey 生成的匹配键能与库侧结果对齐。
// 【返回】成功=转好的值；转换失败（如把 "abc" 当 INT）=无效 QVariant，调用方据此报
// E_LOOKUP_KEY_INVALID。
QVariant castToAffinity(const QVariant& raw, const ColumnInfo& gCol) {
    // Applies SQLite column affinity coercion per row-lookup spec §affinity-coercion table.
    // Priority order matches SQLite's own affinity determination rules.
    // 【译】按行查找规范的「亲和性强制表」做转换；判定优先级与 SQLite 自身的亲和性判定规则一致。
    // 取声明类型并归一化（去空白、转大写），下面用「子串包含」来判定亲和性——这正是 SQLite 的规则：
    //   含 INT → INTEGER 亲和；含 CHAR/CLOB/TEXT → TEXT；含 REAL/FLOA/DOUB → REAL；含 BLOB 或无类型
    //   → BLOB；其余 → NUMERIC。
    QString type = gCol.declaredType.trimmed().toUpper();
    if (type.contains(QStringLiteral("INT"))) {  // INTEGER 亲和：转 qlonglong，转不动即失败
        bool ok;
        qlonglong iv = raw.toLongLong(&ok);
        if (!ok)
            return QVariant();
        return QVariant(iv);
    }
    if (type.contains(QStringLiteral("REAL")) || type.contains(QStringLiteral("FLOA")) ||
        type.contains(QStringLiteral("DOUB"))) {  // REAL 亲和：转 double
        bool ok;
        double dv = raw.toDouble(&ok);
        if (!ok)
            return QVariant();
        return QVariant(dv);
    }
    if (type.contains(QStringLiteral("BLOB"))) {  // BLOB：保留字节数组；空字节视为无效
        if (raw.type() == QVariant::ByteArray)
            return raw;
        const QByteArray ba = raw.toByteArray();
        return ba.isEmpty() ? QVariant() : QVariant(ba);
    }
    // TEXT affinity: declared type contains CHAR, CLOB, or TEXT.
    // 【译】TEXT 亲和：声明类型含 CHAR / CLOB / TEXT → 一律按字符串处理。
    if (type.contains(QStringLiteral("CHAR")) || type.contains(QStringLiteral("CLOB")) ||
        type.contains(QStringLiteral("TEXT"))) {
        return QVariant(raw.toString());
    }
    // H-01 fix: empty type (BLOB/none affinity) and NUMERIC affinity (NUMERIC, DECIMAL,
    // BOOLEAN, DATE, DATETIME, etc.) must preserve the raw QVariant, not coerce to string.
    // Per row-lookup spec: "BLOB or no declared type → preserve raw QVariant".
    // 【H-01 修复｜译】空类型（BLOB/无亲和）与 NUMERIC 亲和（NUMERIC/DECIMAL/BOOLEAN/DATE/DATETIME
    // 等）
    //   必须【原样保留】raw，而不能强转成字符串——否则会改变值的真实类型、破坏后续严格相等匹配。
    //   行查找规范原话：「BLOB 或无声明类型 → 保留原始 QVariant」。
    return raw;
}

// makeTupleKey —— 把「匹配键元组」序列化成一个稳定、带类型标签的字符串，用作 QHash 的键。
// Serialize a match-key tuple to a stable, type-aware string for use as QHash key.
// H-5/H-6 fix: prefix each value with its type tag so that 1 (int), "1" (string) and
// 1.0 (double) produce different keys, preserving SQLite strict equality semantics.
// 【H-5/H-6 修复｜译】给每个值加上「类型标签前缀」，使 1（整数）、"1"（字符串）、1.0（浮点）生成
//   【不同】的键，从而保留 SQLite 的严格相等语义——三者在 SQLite 里并不相等，键也必须不同，
//   否则会把本不该匹配的行误判为命中。
// 【编码格式】各值前缀：i:整数 / d:浮点(17 位有效数字精确表示) / b:字节(hex) / s:字符串；null 用
//   特殊串 "\x00null"。各值之间用单元分隔符 0x1F 连接（业务数据几乎不含该控制符，避免歧义碰撞）。
QString makeTupleKey(const QVector<QVariant>& values) {
    QStringList parts;
    for (const auto& v : values) {
        if (v.isNull()) {
            parts.append(QStringLiteral("\x00null"));
        } else {
            switch (v.type()) {
                case QVariant::LongLong:
                case QVariant::Int:
                case QVariant::ULongLong:
                case QVariant::UInt:
                    parts.append(QStringLiteral("i:") + QString::number(v.toLongLong()));
                    break;
                case QVariant::Double:
                    parts.append(QStringLiteral("d:") + QString::number(v.toDouble(), 'g', 17));
                    break;
                case QVariant::ByteArray:
                    parts.append(QStringLiteral("b:") +
                                 QString::fromLatin1(v.toByteArray().toHex()));
                    break;
                default:
                    parts.append(QStringLiteral("s:") + v.toString());
                    break;
            }
        }
    }
    return parts.join(QLatin1Char('\x1F'));
}

// LookupHit —— 一次查找在缓存里的命中记录：选取列的值 + 命中行数（用于歧义检测）。
struct LookupHit {
    QVector<QVariant> values;  // select column values in lk.select order
                               // 【译】按 lk.select 的顺序排列的「选取列」值
    int hitCount = 0;  // 命中了几行——> 1 即为歧义（同一匹配键对应多条参照行）
};

// identityKey -> (tupleKey -> hit)
// 【译】两级缓存：身份键(哪张表/哪些匹配列/哪些选取列) → ( 匹配键元组 → 命中记录 )。
//   第一级按「查找身份」去重（同身份共享一次预取）；第二级按「具体匹配键」定位某一行结果。
using LookupCache = QHash<QString, QHash<QString, LookupHit>>;

}  // anonymous namespace

// ---- Lookup application (Phase B) -----------------------------------------
// 【译】查找应用（Phase B/C 行级阶段）：把 Phase A.5 预取好的缓存「套」到每一行的载荷上。

// applyLookups —— 对某一行的所有路由套用其 lookups：从预取缓存取结果、写进各路由载荷。
// Returns the set of route indices where any lookup failed (used to seed cascade suppression).
// 【译】返回「本行中发生过查找失败的路由下标集合」，用于播种「级联抑制」（父查找失败 → 子也跳过）。
// 做什么（逐路由、逐 lookup）：
//   1) 从本行 Excel 取匹配列原始值 → 判空（§5.2，空键报 E_LOOKUP_KEY_EMPTY）；
//   2) castToAffinity 强转匹配列亲和性（§5.3，失败报 E_LOOKUP_KEY_INVALID）；
//   3) makeTupleKey 拼匹配键 → 在缓存里查（§5.4，查无报 E_LOOKUP_NOT_FOUND）；
//   4) 命中行数>1 → 歧义（§5.5，报 E_LOOKUP_AMBIGUOUS）；
//   5) 命中唯一 → 把选取列的值写回载荷的 dbColumns/binds，并同步更新冲突键值（§5.6，NULL
//   透明传递）。
// 任一 lookup 出错即把该路由记入返回的 failedRoutes（§D11），该路由及其子孙在写阶段会被跳过。
// 参数：payloads 本行各路由载荷（出参，会被写入查得的列值）；routes 路由定义；cache 预取缓存；
//   catalog 表结构；reader/row/sheet 定位 Excel 单元格与报错位置；errors 错误收集器。
static QSet<int> applyLookups(QVector<RoutePayload>& payloads, const QVector<RouteSpec>& routes,
                              const LookupCache& cache, const SchemaCatalog& catalog,
                              const ExcelReader& reader, int row, const QString& sheet,
                              ErrorCollector* errors) {
    QSet<int> failedRoutes;
    for (int i = 0; i < routes.size(); ++i) {
        const RouteSpec& route = routes[i];
        if (route.lookups.isEmpty())
            continue;

        RoutePayload& payload = payloads[i];
        bool routeFailed = false;  // true if any lookup on this route erred this row

        for (const LookupSpec& lk : route.lookups) {
            const TableInfo* gTable = catalog.table(lk.fromTable);
            QString identityKey = buildIdentityKey(lk);

            // Build match key with affinity cast
            QVector<QVariant> matchVals;
            bool hasError = false;
            QStringList matchHeaders;

            for (const auto& mp : lk.match) {
                matchHeaders.append(mp.second);
                QVariant raw = reader.cellBySource(row, mp.second);

                // §5.2 empty: null or trimmed-empty; numeric zero is NOT empty
                bool isEmpty = raw.isNull();
                if (!isEmpty && raw.toString().trimmed().isEmpty())
                    isEmpty = true;

                if (isEmpty) {
                    errors->add(sheet, row, mp.second, raw.toString(),
                                QString::fromLatin1(err::E_LOOKUP_KEY_EMPTY),
                                QStringLiteral("route '") + route.table +
                                    QStringLiteral("' lookup '") + lk.name +
                                    QStringLiteral("': match key '") + mp.second +
                                    QStringLiteral("' is empty"));
                    hasError = true;
                    break;
                }

                // §5.3 cast to G column affinity
                const ColumnInfo* gCol = gTable ? gTable->column(mp.first) : nullptr;
                QVariant casted = gCol ? castToAffinity(raw, *gCol) : QVariant(raw.toString());
                if (!casted.isValid()) {
                    errors->add(sheet, row, mp.second, raw.toString(),
                                QString::fromLatin1(err::E_LOOKUP_KEY_INVALID),
                                QStringLiteral("route '") + route.table +
                                    QStringLiteral("' lookup '") + lk.name +
                                    QStringLiteral("': match key '") + mp.second +
                                    QStringLiteral("' type cast failed (expected ") +
                                    (gCol ? gCol->declaredType : QStringLiteral("?")) +
                                    QStringLiteral(")"));
                    hasError = true;
                    break;
                }
                matchVals.append(casted);
            }

            if (hasError) {
                routeFailed = true;
                continue;
            }

            // §5.4 look up in prefetch cache
            QString tkey = makeTupleKey(matchVals);
            const auto& idCache = cache.value(identityKey);
            auto it = idCache.find(tkey);

            if (it == idCache.end()) {
                QStringList keyParts;
                for (int ki = 0; ki < lk.match.size() && ki < matchVals.size(); ++ki)
                    keyParts.append(lk.match[ki].second + QLatin1Char('=') +
                                    matchVals[ki].toString());
                errors->add(sheet, row, matchHeaders.join(QLatin1Char(',')), tkey,
                            QString::fromLatin1(err::E_LOOKUP_NOT_FOUND),
                            QStringLiteral("route '") + route.table + QStringLiteral("' lookup '") +
                                lk.name + QStringLiteral("': no match for (") +
                                keyParts.join(QStringLiteral(", ")) + QStringLiteral(") in ") +
                                lk.fromTable);
                routeFailed = true;
                continue;
            }

            const LookupHit& hit = it.value();

            // §5.5 ambiguous
            if (hit.hitCount > 1) {
                errors->add(sheet, row, matchHeaders.join(QLatin1Char(',')), tkey,
                            QString::fromLatin1(err::E_LOOKUP_AMBIGUOUS),
                            QStringLiteral("route '") + route.table + QStringLiteral("' lookup '") +
                                lk.name + QStringLiteral("': found ") +
                                QString::number(hit.hitCount) + QStringLiteral(" rows in ") +
                                lk.fromTable + QStringLiteral("; consider deduplicating ") +
                                lk.fromTable + QStringLiteral(" on match columns"));
                routeFailed = true;
                continue;
            }

            // §5.6 append select results (NULL transparent)
            for (int si = 0; si < lk.select.size(); ++si) {
                const QString& targetCol = lk.select[si].second;
                const QVariant& val = hit.values[si];

                int existingIdx = payload.indexOf(targetCol);
                if (existingIdx >= 0) {
                    payload.binds[existingIdx] = val;
                } else {
                    payload.dbColumns.append(targetCol);
                    payload.binds.append(val);
                }

                // Update conflictVals if this target is a conflict column
                for (int ci = 0; ci < payload.conflictKey.size(); ++ci) {
                    if (payload.conflictKey[ci] == targetCol && ci < payload.conflictVals.size()) {
                        payload.conflictVals[ci] = val;
                    }
                }
            }
        }

        // §D11: any lookup failure on this route → seed cascade suppression
        if (routeFailed)
            failedRoutes.insert(i);
    }
    return failedRoutes;
}

// ---- Lookup prefetch (Phase A.5) ------------------------------------------
// 【译】查找预取（Phase A.5）：在逐行处理【之前】，把所有「不同身份」的查找一次性批量 SELECT
//   出来建好缓存，避免逐行去库里查（N 行 → N 次查询会非常慢）。这是导入性能的关键优化。

// buildLookupCache —— 扫描全 Excel、按查找身份去重、分批 SELECT 参照表，填满 LookupCache。
// §4.11 Prefetch query counter hook — called once per actual SELECT executed.
// Nullptr → noop (production). Inject a counting lambda in tests.
// 【§4.11｜译】onPrefetch 是「预取查询计数钩子」——每真正执行一次批量 SELECT
// 就回调一次（带身份键）。
//   生产传 nullptr（无操作）；测试注入计数 lambda 来断言「批次数 / 去重是否生效」。
// 做什么（对每个去重后的查找身份）：
//   1) 预扫全 Excel，收集该身份所有「去重后的匹配键元组」（跳过空/转换失败的行，留到行级再报错）；
//   2) §4.5 若一个键都没有 → 跳过 SELECT，缓存留空；
//   3) §4.8 按 SQLITE_MAX_VARIABLE_NUMBER=999 把键分批（单列用 IN(...)，多列用 (a=? AND b=?) OR
//   ...）； 4) §4.10 任一批 SELECT 失败 → 报 E_LOOKUP_QUERY_FAILED 并整体失败返回（表级致命错误）；
//   5) 把结果行按匹配键聚进 idCache，并累加 hitCount（供后续歧义检测）。
// 返回：true 全部预取成功；false 有批 SELECT 失败（errors 已记，导入应中止）。
static bool buildLookupCache(const ProfileSpec& profile, const SchemaCatalog& catalog,
                             const ExcelReader& reader, QSqlDatabase& db, const QString& sheetName,
                             ErrorCollector* errors, LookupCache* cache,
                             const std::function<void(const QString&)>& onPrefetch = nullptr) {
    // Collect all unique lookup identities across all routes (and all classes for Mixed mode)
    // 【译】收集所有路由（Mixed 模式下还含所有 class）里「去重后的查找身份」。同身份只保留一份
    //   代表性的 LookupSpec，确保「相同的查找只预取一次」。
    QHash<QString, LookupSpec>
        identitySpecs;  // identityKey -> representative LookupSpec（身份键→代表规格）

    // collectLookups：把一批路由里的 lookups 按身份键去重收进 identitySpecs（首次出现者为代表）。
    auto collectLookups = [&](const QVector<RouteSpec>& routes) {
        for (const RouteSpec& route : routes) {
            for (const LookupSpec& lk : route.lookups) {
                QString ikey = buildIdentityKey(lk);
                if (!identitySpecs.contains(ikey))
                    identitySpecs[ikey] = lk;
            }
        }
    };

    if (profile.mode == ProfileMode::Mixed) {
        for (const auto& cls : profile.classes)
            collectLookups(cls.routes);
    } else {
        collectLookups(profile.routes);
    }

    // For each identity: pre-scan Excel, batch SELECT, populate cache
    for (auto it = identitySpecs.begin(); it != identitySpecs.end(); ++it) {
        const QString& ikey = it.key();
        const LookupSpec& lk = it.value();

        const TableInfo* gTable = catalog.table(lk.fromTable);

        // Pre-scan: collect distinct match key tuples (skip empty/invalid at row-time)
        // 【译】预扫：收集去重后的匹配键元组。空键 / 转换失败的行在此【跳过】，留到行级
        // applyLookups
        //   时再就地报具体错误（这里只管「该去库里查哪些键」）。
        QHash<QString, QVector<QVariant>>
            keyMap;  // tupleKey -> casted values（匹配键 → 已转换的值）

        for (int r = reader.firstDataRow(); r <= reader.lastRow(); ++r) {
            QVector<QVariant> matchVals;
            bool skip = false;
            for (const auto& mp : lk.match) {
                QVariant raw = reader.cellBySource(r, mp.second);
                if (raw.isNull() || raw.toString().trimmed().isEmpty()) {
                    skip = true;
                    break;
                }
                const ColumnInfo* gCol = gTable ? gTable->column(mp.first) : nullptr;
                QVariant casted = gCol ? castToAffinity(raw, *gCol) : QVariant(raw.toString());
                if (!casted.isValid()) {
                    skip = true;
                    break;
                }
                matchVals.append(casted);
            }
            if (skip)
                continue;

            QString tkey = makeTupleKey(matchVals);
            if (!keyMap.contains(tkey))
                keyMap[tkey] = matchVals;
        }

        // §4.5 K == 0: skip SELECT entirely
        if (keyMap.isEmpty()) {
            (*cache)[ikey] = {};
            continue;
        }

        // Build batch SELECT: SELECT <match cols>, <select cols> FROM G WHERE ...
        QStringList selectColNames;
        for (const auto& mp : lk.match)
            selectColNames.append(mp.first);
        for (const auto& sp : lk.select)
            selectColNames.append(sp.first);

        int numMatchCols = lk.match.size();
        int numSelectCols = lk.select.size();

        // §4.8 chunk by SQLITE_MAX_VARIABLE_NUMBER = 999
        // 【§4.8｜译】按 SQLite 单条语句最多 999 个绑定变量的上限分批。每个匹配键占 numMatchCols 个
        //   占位符，故每批最多放 999/列数 个键（至少 1，防列数过大时除成 0）。超过就分多批 SELECT。
        const int maxVars = 999;
        int chunkSize = qMax(1, maxVars / numMatchCols);

        QHash<QString, LookupHit> idCache;
        QVector<QString> keyList = keyMap.keys().toVector();

        for (int start = 0; start < keyList.size(); start += chunkSize) {
            int end = qMin(start + chunkSize, keyList.size());
            int batchSize = end - start;

            // §4.6/4.7 Single column: use IN; multi-column: OR-join AND-clauses
            // H-04 fix: quote all identifiers using SqlBuilder::quoteIdent.
            // 【§4.6/4.7｜译】单匹配列用  col IN (?,?,...) ；多匹配列用  (a=? AND b=?) OR (...) OR
            // ...
            //   把整批键塞进一条 SELECT。【H-04 修复】所有标识符都过 SqlBuilder::quoteIdent
            //   防注入/关键字冲突。
            QStringList quotedSelectCols;
            for (const auto& c : selectColNames)
                quotedSelectCols.append(SqlBuilder::quoteIdent(c));
            QString sql = QStringLiteral("SELECT ") + quotedSelectCols.join(QStringLiteral(", ")) +
                          QStringLiteral(" FROM ") + SqlBuilder::quoteIdent(lk.fromTable) +
                          QStringLiteral(" WHERE ");

            if (numMatchCols == 1) {
                QStringList placeholders;
                for (int i = 0; i < batchSize; ++i)
                    placeholders.append(QStringLiteral("?"));
                sql += SqlBuilder::quoteIdent(lk.match[0].first) + QStringLiteral(" IN (") +
                       placeholders.join(QStringLiteral(", ")) + QStringLiteral(")");
            } else {
                QStringList orClauses;
                for (int i = 0; i < batchSize; ++i) {
                    QStringList andClauses;
                    for (const auto& mp : lk.match)
                        andClauses.append(SqlBuilder::quoteIdent(mp.first) +
                                          QStringLiteral(" = ?"));
                    orClauses.append(QStringLiteral("(") +
                                     andClauses.join(QStringLiteral(" AND ")) +
                                     QStringLiteral(")"));
                }
                sql += orClauses.join(QStringLiteral(" OR "));
            }

            if (onPrefetch)
                onPrefetch(ikey);

            QSqlQuery q(db);
            q.prepare(sql);
            for (int i = start; i < end; ++i) {
                const QVector<QVariant>& vals = keyMap[keyList[i]];
                for (const auto& v : vals)
                    q.addBindValue(v);
            }

            if (!q.exec()) {
                errors->addTable(sheetName, QString::fromLatin1(err::E_LOOKUP_QUERY_FAILED),
                                 QStringLiteral("Lookup prefetch failed for '") + lk.fromTable +
                                     QStringLiteral("': ") + q.lastError().text());
                return false;  // §4.10 fatal
            }

            while (q.next()) {
                // Re-build match key from result row
                // 【译】从结果行重建匹配键：SELECT 的列序是「先匹配列、后选取列」，故前
                // numMatchCols
                //   个值即匹配键。用同一个 makeTupleKey 重算键，保证与行级查询时的键编码完全一致。
                QVector<QVariant> resultMatchVals;
                for (int i = 0; i < numMatchCols; ++i)
                    resultMatchVals.append(q.value(i));
                QString tkey = makeTupleKey(resultMatchVals);

                // Select values follow match columns in the result
                // 【译】选取列的值紧跟在匹配列之后（下标 numMatchCols ..
                // numMatchCols+numSelectCols-1）。
                QVector<QVariant> selectVals;
                for (int i = numMatchCols; i < numMatchCols + numSelectCols; ++i)
                    selectVals.append(q.value(i));

                // 聚合：同一匹配键命中多行时，只保留首行的选取值，但 hitCount 持续累加——
                // 行级 applyLookups 据 hitCount>1 判定「歧义」(E_LOOKUP_AMBIGUOUS)。
                auto& hit = idCache[tkey];
                if (hit.hitCount == 0)
                    hit.values = selectVals;
                hit.hitCount++;
            }
        }

        (*cache)[ikey] = idCache;
    }

    return true;
}

// ---- routeHasChildren ------------------------------------------------------

// routeHasChildren —— 判断 table 是否是「某条路由的父表」（即有子路由以它为 parent）。
// 用途：批内唯一性检查（BatchUniqueness）需要知道一张表是不是「会被别的表当父引用」——若是父表，
//   其主键在同一批内重复会破坏子表的外键引用，校验需更严格。O(路由数) 线性扫描即可。
static bool routeHasChildren(const QString& table, const QVector<RouteSpec>& routes) {
    for (const auto& r : routes) {
        if (r.parent == table)
            return true;
    }
    return false;
}

// ---- ImportService::run ----------------------------------------------------
// run() 的整体五阶段流水线见文件头；下面按 Phase 标注，对每个阶段就近补充「这一步在干什么、
// 为什么这么处理错误」。Mixed 模式（按判别列分 class）与单/多表模式的分叉在每个阶段都成对出现。

ImportResult ImportService::run(const ProfileSpec& profile, const SchemaCatalog& catalog,
                                const QString& xlsxPath, const ImportOptions& options,
                                QSqlDatabase& db, bool manageTransaction) {
    ImportResult result;
    ErrorCollector errors;  // 全程收集表级/行级错误与警告，最后汇入 result

    // --- Phase A: Open and read xlsx header ---
    // 【Phase A｜译】打开 xlsx、选
    // sheet、读表头。这三步任一失败都是「表级致命错误」——文件都读不了，
    //   后续无从谈起，故各自立即把错误塞进 result 并 return（不写库）。
    ExcelReader reader;
    QString xlsxErr;
    if (!reader.open(xlsxPath, &xlsxErr)) {
        errors.addTable(profile.sheet, QString::fromLatin1(err::E_OPEN_XLSX), xlsxErr);
        result.errors = errors.list();
        return result;
    }

    QString sheetName = options.sheetName.isEmpty() ? profile.sheet : options.sheetName;
    if (!reader.selectSheet(sheetName, &xlsxErr)) {
        errors.addTable(profile.sheet, QString::fromLatin1(err::E_OPEN_XLSX), xlsxErr);
        result.errors = errors.list();
        return result;
    }

    if (!reader.readHeader(profile.headerRow, &xlsxErr)) {
        errors.addTable(profile.sheet, QString::fromLatin1(err::E_HEADER_NOT_FOUND), xlsxErr);
        result.errors = errors.list();
        return result;
    }

    // --- Phase B: Profile validation ---
    // 【Phase B｜译】校验 Profile 与「表头 + 表结构」是否自洽（列是否存在、类型是否相容等）。
    //   不通过即表级失败、立即返回。
    ProfileValidator validator;
    if (!validator.validate(profile, catalog, reader.headers(), &errors)) {
        result.errors = errors.list();
        return result;
    }

    // --- Prepare routes and validators ---
    // 【译】准备路由与校验器。三件事：①Mixed 模式下初始化 Router（按判别列分派 class）；
    //   ②为各路由编译校验器（compileValidators）；③把各路由按外键依赖做拓扑排序（父先于子）。
    //   每件都区分 Mixed（逐 class 处理，结果按 cls.id 存）与非 Mixed（用空串 key）两条分支。
    //   任一失败均为表级错误（配置解析/拓扑成环），立即返回。
    TopoSorter topoSorter;
    Mapper mapper;
    Router router;

    if (profile.mode == ProfileMode::Mixed) {
        QString routerErr;
        if (!router.init(profile, &routerErr)) {
            errors.addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE), routerErr);
            result.errors = errors.list();
            return result;
        }
    }

    ValidatorMap validatorMap;
    if (profile.mode == ProfileMode::Mixed) {
        for (const auto& cls : profile.classes) {
            QString vErr;
            if (!mapper.compileValidators(cls.routes, cls.id, profile, &validatorMap, &vErr)) {
                errors.addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE), vErr);
                result.errors = errors.list();
                return result;
            }
        }
    } else {
        QString vErr;
        if (!mapper.compileValidators(profile.routes, QString(), profile, &validatorMap, &vErr)) {
            errors.addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE), vErr);
            result.errors = errors.list();
            return result;
        }
    }

    QHash<QString, QVector<RouteSpec>> topoRoutes;
    if (profile.mode == ProfileMode::Mixed) {
        for (const auto& cls : profile.classes) {
            QVector<RouteSpec> sorted;
            QString topoErr;
            if (!topoSorter.sort(cls.routes, &sorted, &topoErr)) {
                errors.addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_TOPOLOGY_CYCLE),
                                topoErr);
                result.errors = errors.list();
                return result;
            }
            topoRoutes[cls.id] = sorted;
        }
    } else {
        QVector<RouteSpec> sorted;
        QString topoErr;
        if (!topoSorter.sort(profile.routes, &sorted, &topoErr)) {
            errors.addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_TOPOLOGY_CYCLE),
                            topoErr);
            result.errors = errors.list();
            return result;
        }
        topoRoutes[QString()] = sorted;
    }

    // --- Phase A.5: Lookup prefetch ---
    // 【Phase A.5｜译】外键正向查找的批量预取：在逐行处理前一次性把参照表查好建缓存（见
    //   buildLookupCache）。预取失败=表级致命（库查询出错），立即返回。
    LookupCache lookupCache;
    if (!buildLookupCache(profile, catalog, reader, db, sheetName, &errors, &lookupCache,
                          onPrefetch)) {
        result.errors = errors.list();
        return result;
    }

    // --- Phase C: Row mapping + validation ---
    // 【Phase C｜译】逐行处理：把每行 Excel 变成一个 RowContext（含多路由载荷）。每行依次做：
    //   ①Mixed 模式按判别列选 class（选不到报 E_ROUTE_UNMATCHED 并跳过该行）；
    //   ②Mapper.map 生成载荷并行级校验（失败标在 payload.hasError → 播种 failedRouteIndices）；
    //   ③applyLookups 套用预取结果；④FkInjector 注入外键（合并各失败来源到 failedRouteIndices）；
    //   ⑤对未失败的载荷做批内唯一性检查。处理后所有 ctx 收进 contexts，留待预检与写入。
    //   贯穿其中的是文件头讲的「部分成功」模型：路由局部失败只跳该路由及子孙，兄弟路由仍照常。
    QVector<RowContext> contexts;
    BatchUniqueness batchUniq;

    for (int r = reader.firstDataRow(); r <= reader.lastRow(); ++r) {
        result.readRows++;

        RowContext ctx;
        ctx.excelRow = r;

        // 选定本行要用哪套（拓扑排序后的）路由：Mixed 模式按「判别列」的值匹配到某个 class；
        // 非 Mixed 模式只有一套路由（用空串 key）。匹配不到 class 的行报 E_ROUTE_UNMATCHED 并跳过。
        QVector<RouteSpec>* routesPtr = nullptr;
        if (profile.mode == ProfileMode::Mixed) {
            QVariant discVal = reader.cellBySource(r, router.discriminatorSource());
            const ClassSpec* cls = router.match(discVal);
            if (!cls) {
                errors.add(sheetName, r, router.discriminatorSource(), discVal.toString(),
                           QString::fromLatin1(err::E_ROUTE_UNMATCHED),
                           QStringLiteral("Row does not match any class discriminator value '") +
                               discVal.toString() + '\'');
                continue;
            }
            ctx.classId = cls->id;
            routesPtr = &topoRoutes[cls->id];
        } else {
            routesPtr = &topoRoutes[QString()];
        }

        // Map Excel columns to payloads
        // M-06 fix: pass sheetName so Mapper error entries carry the correct sheet location.
        ctx.payloads = mapper.map(*routesPtr, r, ctx.classId, reader, validatorMap, profile,
                                  &errors, sheetName);
        // H-01 fix: validator and temporal-conversion failures are route-local (Mapper sets
        // payload.hasError for the affected route).  Seed failedRouteIndices from payload.hasError
        // so only the affected route (and its descendants) is skipped, while sibling routes whose
        // payloads are valid still proceed.  hasNonRouteError is reserved for structural errors
        // (missing columns, codec failures) that render the entire row unusable.
        for (int pi = 0; pi < ctx.payloads.size(); ++pi) {
            if (ctx.payloads[pi].hasError)
                ctx.failedRouteIndices.insert(pi);
        }

        // Apply lookups (Phase A.5 cache → row payload); failures seed cascade suppression (§D11)
        QSet<int> lookupFailed = applyLookups(ctx.payloads, *routesPtr, lookupCache, catalog,
                                              reader, r, sheetName, &errors);

        // H-01 fix: merge Mapper-level failures (ctx.failedRouteIndices) into the seed set
        // passed to FkInjector so both failure sources are preserved.  Previously the inject()
        // return value overwrote ctx.failedRouteIndices, discarding the Mapper failures.
        // H-04 fix: capture failedRouteIndices so write phase skips only affected payloads
        // (and their descendants) rather than the entire Excel row.
        QSet<int> initialFailed = ctx.failedRouteIndices;  // save Mapper failures
        initialFailed |= lookupFailed;
        FkInjector fkInjector;
        ctx.failedRouteIndices = fkInjector.inject(ctx.payloads, *routesPtr, r, sheetName, &errors,
                                                   std::move(initialFailed));

        // Batch uniqueness check — only on payloads that did not fail injection
        for (int pi = 0; pi < ctx.payloads.size(); ++pi) {
            if (ctx.failedRouteIndices.contains(pi))
                continue;
            const RoutePayload& payload = ctx.payloads[pi];
            bool hasChildren = routeHasChildren(payload.table, *routesPtr);
            batchUniq.check(payload, r, hasChildren, &errors, sheetName);
        }

        contexts.append(ctx);
    }

    // FK preflight check
    // 【译】外键预检：写库前先检查各行引用的外键在库里是否存在（避免写入时才撞约束失败）。
    //   非 Mixed 直接对全部 contexts 检查；Mixed 需逐 class 检查（不同 class 路由集不同），
    //   见下方 H-01 修复：把按 class 拷贝出的临时 ctx 里新增的 failedRouteIndices「回灌」原
    //   contexts， 因为写阶段只看原始 contexts，不回灌就会丢失这部分失败标记。
    if (profile.mode != ProfileMode::Mixed) {
        ForeignKeyPreflight fkPreflight;
        fkPreflight.check(contexts, topoRoutes.value(QString()), db, sheetName, &errors);
    } else {
        // H-01 fix: build an excelRow→index map before iterating by class, so failed
        // failedRouteIndices written into the per-class copy can be merged back into the
        // original contexts vector (the write phase only sees contexts, not clsContexts).
        QHash<int, int> excelRowToIdx;
        for (int i = 0; i < contexts.size(); ++i)
            excelRowToIdx[contexts[i].excelRow] = i;

        for (const auto& cls : profile.classes) {
            QVector<RowContext> clsContexts;
            for (const auto& ctx : contexts) {
                if (ctx.classId == cls.id)
                    clsContexts.append(ctx);
            }
            ForeignKeyPreflight fkPreflight;
            fkPreflight.check(clsContexts, topoRoutes.value(cls.id), db, sheetName, &errors);

            // Merge failedRouteIndices from the temporary copy back into original contexts.
            for (const auto& clsCtx : clsContexts) {
                if (!clsCtx.failedRouteIndices.isEmpty()) {
                    auto it = excelRowToIdx.find(clsCtx.excelRow);
                    if (it != excelRowToIdx.end())
                        contexts[it.value()].failedRouteIndices |= clsCtx.failedRouteIndices;
                }
            }
        }
    }

    // §8.4 dryRun: skip write, populate dryRunPayloads
    // 【§8.4｜译】dryRun（试跑）模式：不写库，把已生成的载荷原样放进 dryRunPayloads 返回，供调用方
    //   预览「会写入什么」。这正是 dryRun 的价值——校验/映射全做、唯独不落库。
    if (options.dryRun) {
        result.dryRunPayloads = contexts;
        result.errors = errors.list();
        return result;
    }

    // C-2 fix: implement ImportOptions::abortOnError.
    // 【C-2 修复｜译】实现 abortOnError 语义，决定有错误时是否还要写库：
    // - Table-level errors (row == 0) always abort (no partial write makes sense).
    // - Row-level errors (row > 0): abort when abortOnError=true (MVP all-or-nothing default);
    //   skip failing row and continue when abortOnError=false (time-format OpenSpec mode).
    // 【译】判别规则：
    //   · 表级错误（约定 row==0）→ 永远中止：连结构都不对，部分写入毫无意义。
    //   · 行级错误（row>0）→ abortOnError=true 时中止（默认「全有或全无」）；
    //     abortOnError=false 时跳过坏行、继续写好行（时间格式等「宽容」模式）。
    {
        bool hasTableErrors = false;
        bool hasRowErrors = false;
        for (const auto& e : errors.list()) {
            if (e.row == 0)  // row==0 是「表级错误」的约定标记
                hasTableErrors = true;
            else
                hasRowErrors = true;
        }
        if (hasTableErrors || (options.abortOnError && hasRowErrors)) {
            result.errors = errors.list();
            return result;  // 中止：不进入写阶段
        }
    }

    // --- Phase D: Write (single transaction) ---
    // 【Phase D｜译】写入：在【单个事务】内按拓扑序对各路由 UPSERT。事务保证「要么整体成功提交、
    //   要么任一处出错整体回滚」，不留半成品。manageTransaction=false
    //   时事务由外层（同步/批处理）管， 本处不 begin/commit/rollback，避免事务嵌套。
    if (manageTransaction && !db.transaction()) {
        errors.addTable(sheetName, QString::fromLatin1(err::E_DB_UPSERT),
                        QStringLiteral("Failed to start transaction: ") + db.lastError().text());
        result.errors = errors.list();
        return result;
    }

    // Collect rows that had mapping / validation / preflight errors; they are skipped.
    // 【译】收集「有过映射/校验/预检错误」的 Excel 行号集合——写入时这些行会被（按规则）跳过。
    QSet<int> failedExcelRows;
    for (const auto& e : errors.list())
        if (e.row > 0)
            failedExcelRows.insert(e.row);

    SqlBuilder sqlBuilder;
    // 预编译语句缓存：相同 SQL 文本只 prepare 一次、反复 bind 执行（导入常是同一表大量行，
    // 复用 prepared statement 能显著提速）。键=SQL 文本，值=已 prepare 的 QSqlQuery。
    QHash<QString, QSqlQuery> preparedQueries;

    // H-04 fix: compute descendant closure so that failing an FK-inject on a parent also skips
    // its children in the write phase.
    // 【H-04 修复｜译】计算「失败路由的子孙闭包」：若父路由的外键注入失败，它的所有子孙路由也必须
    //   在写阶段一并跳过——父都没写成，子的外键无所指，强写必撞约束。下面用「不动点迭代」：反复扫描
    //   路由，只要某路由的 parent 已在失败集里就把它也加入，直到一轮无新增（changed=false）为止。
    auto buildDescendantFailSet = [](const QVector<RoutePayload>& payloads,
                                     const QVector<RouteSpec>& routes,
                                     const QSet<int>& directFailed) -> QSet<int> {
        if (directFailed.isEmpty())
            return {};
        QHash<QString, int> tableToIdx;
        for (int i = 0; i < payloads.size(); ++i)
            tableToIdx[payloads[i].table] = i;
        QSet<int> failed = directFailed;
        bool changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < routes.size(); ++i) {
                if (failed.contains(i))
                    continue;
                if (!routes[i].parent.isEmpty()) {
                    const int parentIdx = tableToIdx.value(routes[i].parent, -1);
                    if (parentIdx >= 0 && failed.contains(parentIdx)) {
                        failed.insert(i);
                        changed = true;
                    }
                }
            }
        }
        return failed;
    };

    bool writeOk = true;  // 任一 prepare/exec 失败即置 false → 终止循环并回滚
    for (const auto& ctx : contexts) {
        // H-02 / M-04 fix: skip the entire Excel row only when the row has a non-route-local error
        // (structural/type/mapping failure that renders payload data unusable).  If the row only
        // has route-local errors (tracked in failedRouteIndices), proceed to route-level filtering
        // below so that sibling routes whose payloads are valid can still be written.
        // M-04: hasNonRouteError covers the case where both kinds of errors coexist — the
        // presence of non-route errors means the whole row must be skipped.
        if (failedExcelRows.contains(ctx.excelRow) &&
            (ctx.hasNonRouteError || ctx.failedRouteIndices.isEmpty()))
            continue;
        // Rows with only failedRouteIndices fall through; skipPayloadIndices below handles them.

        // H-04 fix: determine the routes for this context.
        const QVector<RouteSpec>* routesForCtx = nullptr;
        if (profile.mode == ProfileMode::Mixed) {
            auto it = topoRoutes.find(ctx.classId);
            if (it != topoRoutes.end())
                routesForCtx = &it.value();
        } else {
            auto it = topoRoutes.find(QString());
            if (it != topoRoutes.end())
                routesForCtx = &it.value();
        }

        QSet<int> skipPayloadIndices;
        if (routesForCtx && !ctx.failedRouteIndices.isEmpty())
            skipPayloadIndices =
                buildDescendantFailSet(ctx.payloads, *routesForCtx, ctx.failedRouteIndices);

        // H-01 fix: track whether any payload was actually upserted for this Excel row so that
        // writtenRows is only incremented when at least one route's data reaches the DB.  Rows
        // whose all payloads are suppressed by skipPayloadIndices (route-local failures) must not
        // count as written.
        bool anyUpserted = false;

        for (int pi = 0; pi < ctx.payloads.size(); ++pi) {
            if (skipPayloadIndices.contains(pi))
                continue;
            const auto& payload = ctx.payloads[pi];
            if (payload.dbColumns.isEmpty())
                continue;

            // 由载荷生成 UPSERT 文本（见 SqlBuilder::buildUpsert）；无可写列 → 空文本 →
            // 跳过该路由。
            UpsertSql upsert = sqlBuilder.buildUpsert(payload);
            if (upsert.sql.isEmpty())
                continue;

            // 取/建该 SQL 的预编译语句：缓存未命中则 prepare 一次并存入；prepare 失败=表级写错误、
            // 置 writeOk=false 跳出（最终回滚）。
            QSqlQuery* qptr = nullptr;
            auto it = preparedQueries.find(upsert.sql);
            if (it == preparedQueries.end()) {
                QSqlQuery q(db);
                if (!q.prepare(upsert.sql)) {
                    errors.add(sheetName, ctx.excelRow, QString(), QString(),
                               QString::fromLatin1(err::E_DB_UPSERT),
                               QStringLiteral("Failed to prepare SQL: ") + q.lastError().text() +
                                   QStringLiteral(" SQL: ") + upsert.sql);
                    writeOk = false;
                    break;
                }
                preparedQueries[upsert.sql] = std::move(q);
                qptr = &preparedQueries[upsert.sql];
            } else {
                qptr = &it.value();
            }

            // 按 binds 顺序绑定占位符的值（顺序与 buildUpsert 产出的 bindOrder 一致，见
            // SqlBuilder）。
            for (const auto& v : payload.binds)
                qptr->addBindValue(v);

            if (!qptr->exec()) {  // UPSERT 执行失败（如约束冲突）=写错误 → 记错、置
                                  // writeOk=false、跳出
                errors.add(sheetName, ctx.excelRow, QString(), QString(),
                           QString::fromLatin1(err::E_DB_UPSERT),
                           QStringLiteral("Upsert failed: ") + qptr->lastError().text());
                writeOk = false;
                break;
            }
            anyUpserted = true;
        }
        if (!writeOk)
            break;
        // M-05 fix: writtenRows counts Excel rows successfully written (i.e. rows where at least
        // one payload upsert succeeded). The previous semantics were identical — but the field name
        // previously suggested "number of DB rows inserted/updated", which is ctx.payloads.size()
        // times per Excel row (one upsert per route). The current semantics are Excel rows; callers
        // that need exact DB upsert counts should sum payloads.size() for non-empty payloads.
        // H-01 fix: only count the row when at least one payload was actually written.
        if (anyUpserted)
            result.writtenRows++;
    }

    // 收尾事务：
    //   · writeOk 且 commit 成功 → result.ok=true（整批落库）；
    //   · writeOk 但 commit 失败 → 回滚 + 记错 + writtenRows 清零；
    //   · writeOk=false（中途出错）→ 回滚 + writtenRows 清零（事务保证已写的也全部撤销）。
    if (writeOk) {
        if (manageTransaction && !db.commit()) {
            if (manageTransaction)
                db.rollback();
            errors.addTable(sheetName, QString::fromLatin1(err::E_DB_UPSERT),
                            QStringLiteral("Commit failed: ") + db.lastError().text());
            result.writtenRows = 0;
        } else {
            result.ok = true;
        }
    } else {
        if (manageTransaction)
            db.rollback();
        result.writtenRows = 0;
    }

    result.errors = errors.list();
    // Merge runtime warnings with profile load-time diagnostics
    // 【译】把「运行期警告」与「Profile 载入期诊断（loadWarnings）」合并进 result.warnings
    // 一并返回。
    result.warnings = errors.warnings();
    for (const QString& w : profile.loadWarnings) {
        RowError re;
        re.sheet = profile.sheet;
        re.code = QStringLiteral("W_PROFILE_LOAD");
        re.message = w;
        result.warnings.append(re);
    }
    return result;
}

}  // namespace dbridge::detail
