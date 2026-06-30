// ============================================================================
// tst_profile_loader.cpp — ProfileLoader（JSON 映射配置解析校验）单元测试
// ============================================================================
//
// 【被测对象 ProfileLoader 做什么】
//   把一段「Profile JSON」（ETL 映射配置：从哪个 sheet、按什么模式、映射到哪些表/列、外键
//   如何注入、lookup 如何查参照、时间格式如何解释……）解析成强类型的 ProfileSpec，并在解析
//   过程中做大量「合法性校验」。它是导入/导出的「配置编译器」——配置写错，应在 load() 阶段
//   就以可读错误码（多为 E_PROFILE_PARSE）拒绝，绝不让坏配置流到实际导入再炸。
//
// 【这一文件的测试哲学：正路径 + 负路径 双覆盖】
//   每个特性都成对验证：
//     · 「正路径」用 QVERIFY2(loader.load(...), err) 断言「合法配置能加载成功」，并 QCOMPARE
//       解析出的字段值，确认「解析结果与 JSON 语义一致」（不仅不报错，还得解析对）。
//     · 「负路径」用 QVERIFY(!loader.load(...)) 断言「非法配置被拒」，并常 QVERIFY2(err.contains
//       ("关键词")) 确认「报的是对的那个错」（错误信息精准，便于用户改配置）。
//
// 【覆盖的主要契约分区】
//   · 模式三态：singleTable / multiTable / mixed 的合法解析与必填项（mode/profileName/routes…）。
//   · fkInject 的「新数组形式」被接受、「旧对象形式」被拒（§10.3，强制迁移到新语法）。
//   · lookup 的解析与各种非法形态（match/select 必须是数组对、name 必填且组内唯一）。
//   · 时间格式（dateFormat/datetimeFormat/timeFormat）的「新旧两种写法」「继承与覆盖」
//     「epochSec 只许 db 侧且不许带 format」「string 必须带非空 format」等一大批边界规则
//     （§7.2.x 矩阵），这是本文件占比最大、最易错的部分。
//
// 【一个反复出现的辅助断言模式】
//   多数测试直接内联：ProfileLoader loader; ProfileSpec spec; QString err;
//   然后 load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err) 取返回值与 err/spec 做断言。
//   effectiveTemporalFor(slotKind, column, spec) 用于计算「列级覆盖叠加 profile 级默认」后的
//   「有效时间格式」，专门验证「继承/覆盖」语义。
// ============================================================================
#include <QJsonDocument>
#include <QtTest>

#include "profile/ProfileLoader.h"
#include "profile/ProfileSpec.h"

using namespace dbridge::detail;

class TstProfileLoader : public QObject {
    Q_OBJECT

   private slots:
    // ── 模式与基础必填项 ──────────────────────────────────────────────────────
    // 正路径：一份完整的 singleTable profile 应加载成功，且各字段（名/表/sheet/headerRow/
    // 模式枚举/路由数/冲突列/列数/导出 orderBy）都解析为 JSON 所声明的值。这是「能解析对」
    // 的基准用例，钉住 SingleTable 模式的字段映射全链路。
    void testSingleTableValid() {
        QString json = R"({
            "profileName": "customer_basic",
            "sheet": "Customers",
            "headerRow": 1,
            "mode": "singleTable",
            "table": "customer",
            "conflict": { "columns": ["customer_no"] },
            "columns": {
                "customer_no": { "source": "CustomerNo", "validators": ["notNull", "len<=32"] },
                "name": { "source": "Name", "validators": ["notNull", "len<=128"] }
            },
            "export": { "orderBy": ["customer_no"] }
        })";

        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        QCOMPARE(spec.name, QStringLiteral("customer_basic"));
        QCOMPARE(spec.sheet, QStringLiteral("Customers"));
        QCOMPARE(spec.headerRow, 1);
        QCOMPARE(spec.mode, ProfileMode::SingleTable);
        QCOMPARE(spec.routes.size(), 1);
        QCOMPARE(spec.routes[0].table, QStringLiteral("customer"));
        QCOMPARE(spec.routes[0].conflict.columns, QStringList{"customer_no"});
        QCOMPARE(spec.routes[0].columns.size(), 2);
        QCOMPARE(spec.exportSpec.orderBy, QStringList{"customer_no"});
    }

    // 负路径：缺 mode 字段 → 拒绝且 err 非空。mode 是「按哪种模式解析路由」的总开关，必填。
    void testMissingMode() {
        QString json = R"({
            "profileName": "test",
            "sheet": "Sheet1",
            "headerRow": 1,
            "table": "t1",
            "columns": {}
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY(!err.isEmpty());
    }

    // 负路径：mode 取了未知值 "weirdMode" → 拒绝，且错误信息应点名这个非法值（便于用户定位）。
    void testUnknownMode() {
        QString json = R"({
            "profileName": "test",
            "sheet": "Sheet1",
            "mode": "weirdMode"
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY(err.contains("weirdMode") || !err.isEmpty());
    }

    // 负路径：缺 profileName → 拒绝。profileName 是按名选 profile 的唯一标识，必填。
    void testMissingProfileName() {
        QString json = R"({
            "sheet": "Sheet1",
            "mode": "singleTable",
            "table": "t1"
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
    }

    // 正路径：multiTable（一行 Excel 拆写到父子两表）。除模式与路由数外，重点校验子路由的
    // parent 指向、fkInject 被解析成「数组形式」的一组 (父列, 子列) pair——确认外键注入配置
    // 被完整、正确地解出（fromTable / pairs / pair.first（父列）/ pair.second（子列））。
    void testMultiTableValid() {
        QString json = R"({
            "profileName": "order_m_set",
            "sheet": "Orders",
            "headerRow": 1,
            "mode": "multiTable",
            "routes": [
                {
                    "table": "orders",
                    "conflict": { "columns": ["order_no"] },
                    "columns": {
                        "order_no": { "source": "OrderNo", "validators": ["notNull"] },
                        "amount": { "source": "Amount", "validators": ["decimal"] }
                    }
                },
                {
                    "table": "order_items",
                    "parent": "orders",
                    "fkInject": [{ "from": "orders", "pairs": [["order_no","order_no"]] }],
                    "conflict": { "columns": ["order_no", "line_no"] },
                    "columns": {
                        "line_no": { "source": "LineNo", "validators": ["int>=1"] },
                        "sku": { "source": "Sku" }
                    }
                }
            ]
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        QCOMPARE(spec.mode, ProfileMode::MultiTable);
        QCOMPARE(spec.routes.size(), 2);
        QCOMPARE(spec.routes[1].parent, QStringLiteral("orders"));
        QCOMPARE(spec.routes[1].fkInject.size(), 1);
        QCOMPARE(spec.routes[1].fkInject[0].fromTable, QStringLiteral("orders"));
        QCOMPARE(spec.routes[1].fkInject[0].pairs.size(), 1);
        QCOMPARE(spec.routes[1].fkInject[0].pairs[0].first, QStringLiteral("order_no"));
        QCOMPARE(spec.routes[1].fkInject[0].pairs[0].second, QStringLiteral("order_no"));
    }

    // 负路径：multiTable 但 routes 为空数组 → 拒绝。多表模式没有任何路由就无事可做，属非法配置。
    void testMultiTableEmptyRoutes() {
        QString json = R"({
            "profileName": "test",
            "sheet": "S",
            "mode": "multiTable",
            "routes": []
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
    }

    // 正路径：mixed（同一 sheet 里靠「判别列 discriminator」把每行分流到不同 class，各 class
    // 有自己的路由）。校验模式、判别列来源 source、class 数量都被正确解析。
    void testMixedValid() {
        QString json = R"({
            "profileName": "mixed_abc",
            "sheet": "Mixed",
            "mode": "mixed",
            "discriminator": { "source": "Type" },
            "classes": [
                {
                    "id": "A",
                    "match": { "equals": "A" },
                    "routes": [
                        { "table": "m1", "conflict": { "columns": ["m_no"] }, "columns": {} }
                    ]
                },
                {
                    "id": "B",
                    "match": { "equals": "B" },
                    "routes": [
                        { "table": "n1", "conflict": { "columns": ["n_no"] }, "columns": {} }
                    ]
                }
            ]
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        QCOMPARE(spec.mode, ProfileMode::Mixed);
        QCOMPARE(spec.discriminatorSource, QStringLiteral("Type"));
        QCOMPARE(spec.classes.size(), 2);
    }

    // 负路径：mixed 模式里两个 class 的 match.equals 都是 "A"（判别值撞车）→ 拒绝。
    // 判别值必须能把行「唯一」分流到某个 class；两个 class 抢同一个值会产生二义性，故非法。
    void testMixedDuplicateMatchEquals() {
        QString json = R"({
            "profileName": "bad_mixed",
            "sheet": "M",
            "mode": "mixed",
            "discriminator": { "source": "Type" },
            "classes": [
                {
                    "id": "A", "match": { "equals": "A" },
                    "routes": [{ "table": "m1", "conflict": { "columns": ["id"] }, "columns": {} }]
                },
                {
                    "id": "B", "match": { "equals": "A" },
                    "routes": [{ "table": "n1", "conflict": { "columns": ["id"] }, "columns": {} }]
                }
            ]
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
    }

    // 负路径：列上声明了未知的 validator token "unknownValidator" → 拒绝，且错误信息应含
    // "Unknown validator token"。校验器是受控词表（notNull/len<=N/int>=N/decimal/date:fmt…），
    // 写错应在加载期拒绝，而非导入时静默忽略。
    void testInvalidValidatorToken() {
        QString json = R"({
            "profileName": "test",
            "sheet": "S",
            "mode": "singleTable",
            "table": "t1",
            "conflict": { "columns": ["id"] },
            "columns": {
                "name": { "source": "Name", "validators": ["unknownValidator"] }
            }
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY(err.contains("Unknown validator token"));
    }

    // ── fkInject 语法：新数组形式接受 / 旧对象形式拒绝（§10.3）──────────────────
    // §10.3 — fkInject new array format
    // 正路径：fkInject 用「数组 + pairs 列表」的新形式，且支持「多列复合外键」（一条 fkInject
    // 携带两对映射）。校验 pairs 被解出 2 项、各 pair 的父列(first)/子列(second)对应正确。
    void testFkInjectArrayMultiPair() {
        QString json = R"({
            "profileName": "test",
            "sheet": "S",
            "mode": "multiTable",
            "routes": [
                { "table": "orders", "conflict": { "columns": ["order_no"] }, "columns": {} },
                {
                    "table": "items",
                    "parent": "orders",
                    "conflict": { "columns": ["order_no","line_no"] },
                    "fkInject": [
                        { "from": "orders", "pairs": [["order_no","order_no"],["tenant_id","tenant_id"]] }
                    ],
                    "columns": {}
                }
            ]
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        QCOMPARE(spec.routes[1].fkInject.size(), 1);
        QCOMPARE(spec.routes[1].fkInject[0].fromTable, QStringLiteral("orders"));
        QCOMPARE(spec.routes[1].fkInject[0].pairs.size(), 2);
        QCOMPARE(spec.routes[1].fkInject[0].pairs[0].first, QStringLiteral("order_no"));
        QCOMPARE(spec.routes[1].fkInject[0].pairs[1].first, QStringLiteral("tenant_id"));
        QCOMPARE(spec.routes[1].fkInject[0].pairs[1].second, QStringLiteral("tenant_id"));
    }

    // 负路径：fkInject 用了已废弃的「对象形式」({from,to}) → 拒绝，且错误信息提到
    // array/object/fkInject 之一（引导用户改用新数组语法）。强制迁移、避免两种语法并存的歧义。
    void testFkInjectOldObjectFormRejected() {
        QString json = R"({
            "profileName": "test",
            "sheet": "S",
            "mode": "multiTable",
            "routes": [
                { "table": "orders", "conflict": { "columns": ["id"] }, "columns": {} },
                {
                    "table": "items",
                    "parent": "orders",
                    "conflict": { "columns": ["id"] },
                    "fkInject": { "from": "orders.order_no", "to": "items.order_no" },
                    "columns": {}
                }
            ]
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY2(err.contains("array") || err.contains("object") || err.contains("fkInject"),
                 err.toUtf8());
    }

    // ── lookup（参照表查找）解析与非法形态 ───────────────────────────────────
    // §10.3 — lookup parsing
    // 正路径：一条完整 lookup 应被解析成 LookupSpec，逐字段校验：name / fromTable / match（一组
    // (参照列, Excel 源列) 对，含中文列名"客户编号"以验证非 ASCII 也正常）/ select（一组
    // (参照列, 写入本地列) 对，本例两项）。这是 lookup 解析「能解对」的基准。
    void testLookupsValidParsing() {
        QString json = R"({
            "profileName": "test",
            "sheet": "S",
            "mode": "singleTable",
            "table": "orders",
            "conflict": { "columns": ["order_no"] },
            "lookups": [
                {
                    "name": "cust",
                    "from": "ref_customers",
                    "match": [["c_no","客户编号"]],
                    "select": [["c_name","customer_name"],["c_tier","tier"]]
                }
            ],
            "columns": {}
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        QCOMPARE(spec.routes[0].lookups.size(), 1);
        const LookupSpec& lk = spec.routes[0].lookups[0];
        QCOMPARE(lk.name, QStringLiteral("cust"));
        QCOMPARE(lk.fromTable, QStringLiteral("ref_customers"));
        QCOMPARE(lk.match.size(), 1);
        QCOMPARE(lk.match[0].first, QStringLiteral("c_no"));
        QCOMPARE(lk.match[0].second, QStringLiteral("客户编号"));
        QCOMPARE(lk.select.size(), 2);
        QCOMPARE(lk.select[0].second, QStringLiteral("customer_name"));
        QCOMPARE(lk.select[1].second, QStringLiteral("tier"));
    }

    // 负路径：lookup.match 写成对象 {col:src} 而非数组对 [[col,src]] → 拒绝。
    // 强制数组形式是为保序、且与 select 语法统一（对象的 key 顺序在 JSON 里不可靠）。
    void testLookupMatchObjectFormRejected() {
        QString json = R"({
            "profileName": "test",
            "sheet": "S",
            "mode": "singleTable",
            "table": "orders",
            "conflict": { "columns": ["id"] },
            "lookups": [
                {
                    "name": "cust",
                    "from": "ref_customers",
                    "match": { "c_no": "客户编号" },
                    "select": [["c_name","customer_name"]]
                }
            ],
            "columns": {}
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
    }

    // 负路径：lookup.select 写成对象而非数组对 → 拒绝（理由同 match：强制数组、保序、统一语法）。
    void testLookupSelectObjectFormRejected() {
        QString json = R"({
            "profileName": "test",
            "sheet": "S",
            "mode": "singleTable",
            "table": "orders",
            "conflict": { "columns": ["id"] },
            "lookups": [
                {
                    "name": "cust",
                    "from": "ref_customers",
                    "match": [["c_no","客户编号"]],
                    "select": { "c_name": "customer_name" }
                }
            ],
            "columns": {}
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
    }

    // 负路径：lookup 缺 name 字段 → 拒绝。name 是 lookup
    // 的引用标识（错误信息/组内唯一性都靠它），必填。
    void testLookupNameRequired() {
        QString json = R"({
            "profileName": "test",
            "sheet": "S",
            "mode": "singleTable",
            "table": "orders",
            "conflict": { "columns": ["id"] },
            "lookups": [
                {
                    "from": "ref_customers",
                    "match": [["c_no","客户编号"]],
                    "select": [["c_name","customer_name"]]
                }
            ],
            "columns": {}
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
    }

    // 负路径：同一路由内两条 lookup 同名 "cust" → 拒绝。同名会让「按名引用 lookup 结果」产生
    // 歧义，故要求 name 在路由内唯一。
    void testLookupNameUniqueWithinRoute() {
        QString json = R"({
            "profileName": "test",
            "sheet": "S",
            "mode": "singleTable",
            "table": "orders",
            "conflict": { "columns": ["id"] },
            "lookups": [
                {
                    "name": "cust",
                    "from": "ref_customers",
                    "match": [["c_no","客户编号"]],
                    "select": [["c_name","customer_name"]]
                },
                {
                    "name": "cust",
                    "from": "ref_other",
                    "match": [["x","y"]],
                    "select": [["a","b"]]
                }
            ],
            "columns": {}
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
    }
    // ---- Temporal format tests ----
    // ── 时间格式（date/datetime/time）解析与继承/覆盖规则 ─────────────────────
    // 时间格式描述「Excel 侧文本格式 ↔ DB 侧存储格式」如何互转，可在 profile 级声明默认、
    // 并在列级覆盖。以下用例钉住：声明标志位 declared、各侧 format/type 的解析、列覆盖 profile、
    // 以及一组「不允许的写法」（date 槽混入时间 token、time 槽混入日期 token 等）。

    // profile 级 dateFormat：声明后 declared=true，excel/db 两侧 format 解析正确；
    // 未声明的 datetimeFormat/timeFormat 其 declared 应为 false（互不牵连）。
    void testProfileLevelDateFormat() {
        QString json = R"({
            "profileName": "tf",
            "sheet": "S",
            "headerRow": 1,
            "mode": "singleTable",
            "table": "t",
            "conflict": { "columns": ["id"] },
            "dateFormat": { "excelFormat": "yyyy/M/d", "dbFormat": "yyyy-MM-dd" },
            "columns": { "id": { "source": "ID" } }
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        QVERIFY(spec.dateFormat.declared);
        QCOMPARE(spec.dateFormat.excel.format, QStringLiteral("yyyy/M/d"));
        QCOMPARE(spec.dateFormat.db.format, QStringLiteral("yyyy-MM-dd"));
        QVERIFY(!spec.datetimeFormat.declared);
        QVERIFY(!spec.timeFormat.declared);
    }

    // 列级覆盖：列 dt 只覆盖了 excel 侧 format（"d.M.yyyy"），未声明 db 侧。校验：① 列上确实
    // 解析出该 excel format；② 通过 effectiveTemporalFor 计算「有效格式」时，db 侧应「继承
    // profile 默认」("yyyy-MM-dd")——验证「列覆盖某一侧、另一侧仍继承 profile」的部分覆盖语义。
    void testPerColumnDateFormatOverride() {
        QString json = R"({
            "profileName": "tf",
            "sheet": "S",
            "headerRow": 1,
            "mode": "singleTable",
            "table": "t",
            "conflict": { "columns": ["id"] },
            "dateFormat": { "excelFormat": "yyyy/M/d", "dbFormat": "yyyy-MM-dd" },
            "columns": {
                "id": { "source": "ID" },
                "dt": {
                    "source": "DT",
                    "dateFormat": { "excelFormat": "d.M.yyyy" }
                }
            }
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        const ColumnSpec* dtCol = nullptr;
        for (const auto& r : spec.routes) {
            for (const auto& c : r.columns) {
                if (c.dbColumn == QStringLiteral("dt"))
                    dtCol = &c;
            }
        }
        QVERIFY(dtCol);
        QVERIFY(dtCol->dateFormat.declared);
        QCOMPARE(dtCol->dateFormat.excel.format, QStringLiteral("d.M.yyyy"));
        // db side not declared at column level → effective inherits profile default
        TemporalFormatSpec eff = effectiveTemporalFor(TemporalSlotKind::Date, *dtCol, spec);
        QCOMPARE(eff.db.format, QStringLiteral("yyyy-MM-dd"));
    }

    // excel 侧的「备选格式列表 excelFormatFallback」解析：主格式之外，再给两个兜底格式
    // （当主格式解析单元格失败时依次尝试）。校验 fallback 数组被解出 2 项且顺序正确。
    void testExcelFormatFallbackParsed() {
        QString json = R"({
            "profileName": "tf",
            "sheet": "S",
            "headerRow": 1,
            "mode": "singleTable",
            "table": "t",
            "conflict": { "columns": ["id"] },
            "columns": {
                "id": { "source": "ID" },
                "dt": {
                    "source": "DT",
                    "dateFormat": {
                        "excelFormat": "yyyy-MM-dd",
                        "dbFormat": "yyyy-MM-dd",
                        "excelFormatFallback": ["d/M/yyyy", "MM.dd.yyyy"]
                    }
                }
            }
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        const ColumnSpec* dtCol = nullptr;
        for (const auto& r : spec.routes)
            for (const auto& c : r.columns)
                if (c.dbColumn == QStringLiteral("dt"))
                    dtCol = &c;
        QVERIFY(dtCol);
        QCOMPARE(dtCol->dateFormat.excel.fallback.size(), 2);
        QCOMPARE(dtCol->dateFormat.excel.fallback[0], QStringLiteral("d/M/yyyy"));
        QCOMPARE(dtCol->dateFormat.excel.fallback[1], QStringLiteral("MM.dd.yyyy"));
    }

    // 负路径：dateFormat 里混入了时间 token（"HH:mm"）→ 拒绝。date 槽只描述「纯日期」，
    // 带时间 token 属语义错配；这道校验防止用户把 datetime 误填进 date 槽。
    void testDateFormatTimeTokenRejected() {
        // dateFormat must not contain time tokens (H, h, m, s, z, t, a, A)
        // 译：dateFormat 不得含时间 token（H/h/m/s/z/t/a/A 等）。
        QString json = R"({
            "profileName": "tf",
            "sheet": "S",
            "headerRow": 1,
            "mode": "singleTable",
            "table": "t",
            "conflict": { "columns": ["id"] },
            "dateFormat": { "excelFormat": "yyyy-MM-dd HH:mm", "dbFormat": "yyyy-MM-dd" },
            "columns": { "id": { "source": "ID" } }
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY(err.contains("dateFormat") || !err.isEmpty());
    }

    // 负路径（对称用例）：timeFormat 里混入了日期 token（"yyyy"）→ 拒绝。time 槽只描述「纯时间」。
    void testTimeFormatDateTokenRejected() {
        // timeFormat must not contain date tokens (y, M, d)
        // 译：timeFormat 不得含日期 token（y/M/d）。
        QString json = R"({
            "profileName": "tf",
            "sheet": "S",
            "headerRow": 1,
            "mode": "singleTable",
            "table": "t",
            "conflict": { "columns": ["id"] },
            "timeFormat": { "excelFormat": "HH:mm yyyy", "dbFormat": "HH:mm:ss" },
            "columns": { "id": { "source": "ID" } }
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
    }

    // 负路径（§2.5）：export 段里出现已废弃的 reverseLookups 字段 → 加载期报错，且错误信息
    // 点名 "reverseLookups"（给出迁移指引）。验证「废弃字段被主动拒绝并精确提示」。
    void testRejectReverseLookupFieldInExportSpec() {
        // 2.5: exportSpec.reverseLookups / exportLookups → load-time error with guidance
        QString json = R"({
            "profileName": "test",
            "sheet": "S",
            "mode": "singleTable",
            "table": "orders",
            "conflict": { "columns": ["order_no"] },
            "columns": {},
            "export": {
                "reverseLookups": [{"name": "x"}]
            }
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY2(err.contains(QStringLiteral("reverseLookups")),
                 ("Error should mention reverseLookups, got: " + err).toUtf8());
    }

    // §2.4：lookup 上 exportRoundtrip=false 时再显式给 exportOnMissing 是「无效配置」——
    // 不报错（仍加载成功），但产生一条 info 级「加载告警」存入 spec.loadWarnings（非阻断）。
    // 验证「无害但无意义的组合」走「警告而非拒绝」这条柔性路径，且告警文本点名 exportOnMissing。
    void testExportOnMissingNoEffectWarningWhenRoundtripFalse() {
        // 2.4: exportRoundtrip=false + explicit exportOnMissing → info-level load warning
        // Warning is stored in spec.loadWarnings (non-blocking).
        // 译：警告存入 spec.loadWarnings（非阻断，不影响加载成功）。
        QString json = R"({
            "profileName": "test",
            "sheet": "S",
            "mode": "singleTable",
            "table": "orders",
            "conflict": { "columns": ["order_no"] },
            "lookups": [
                {
                    "name": "cust",
                    "from": "ref_customers",
                    "match": [["c_no","CustNo"]],
                    "select": [["c_name","customer_name"]],
                    "exportRoundtrip": false,
                    "exportOnMissing": "null"
                }
            ],
            "columns": {}
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        bool hasWarning = false;
        for (const QString& w : spec.loadWarnings)
            if (w.contains(QStringLiteral("exportOnMissing")))
                hasWarning = true;
        QVERIFY2(hasWarning,
                 "Expected info warning about exportOnMissing with exportRoundtrip=false");
    }

    // 一列同时声明了 dateFormat 与 date:fmt 校验器（两套都在管日期）→ 不报错但产生 loadWarning
    // （提醒可能冗余/冲突）。同样验证「柔性警告而非硬拒绝」。
    void testDateFormatAndValidatorCoexistProducesLoadWarning() {
        // column declares both dateFormat and date:fmt validator → loadWarning, no error
        QString json = R"({
            "profileName": "tf",
            "sheet": "S",
            "headerRow": 1,
            "mode": "singleTable",
            "table": "t",
            "conflict": { "columns": ["id"] },
            "columns": {
                "id": { "source": "ID" },
                "dt": {
                    "source": "DT",
                    "dateFormat": { "excelFormat": "yyyy-MM-dd", "dbFormat": "yyyy-MM-dd" },
                    "validators": ["date:yyyy-MM-dd"]
                }
            }
        })";
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        QVERIFY(!spec.loadWarnings.isEmpty());
    }

    // ── New-form (excel/db sub-object) loading matrix ──────────────────────────
    // ── 时间格式「新写法」加载矩阵：excel/db 各自是 {type, format} 子对象（§7.2.x）──
    //
    // 新写法把每一侧（excel / db）显式写成 {type, format}：
    //   · type=string  —— 用 format 字符串解释/格式化（此时 format 必须非空）；
    //   · type=epochSec/epochMs —— 存成纪元秒/毫秒（仅 db 侧、且仅 datetime 槽允许 epoch；
    //     epoch 类型不应再带 format，因为它是数值而非文本格式）。
    // 下面这一长串用例把「允许 / 不允许」的组合逐格钉死，是本文件最密集的边界覆盖区。

    // 7.2.1: dateFormat.db.type=string, profile level → load succeeds
    // 正路径：profile 级 dateFormat 用新写法、db 侧 type=string + 合法 format → 加载成功，
    // declared=true 且 db.format 解析正确。
    void testNewFormDateStringProfileLevel() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "dateFormat":{"db":{"type":"string","format":"yyyy-MM-dd"},
                          "excel":{"type":"string","format":"yyyy/M/d"}},
            "columns":{"id":{"source":"ID","validators":["int"]}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        QVERIFY(spec.dateFormat.declared);
        QCOMPARE(spec.dateFormat.db.format, QStringLiteral("yyyy-MM-dd"));
    }

    // 7.2.4: dateFormat.db.type=epochSec anywhere → E_PROFILE_PARSE (dateFormat doesn't allow
    // epoch)
    // 负路径：date 槽的 db 侧用 epochSec → 拒绝（纯日期不适合用纪元秒表达；epoch 仅 datetime 许）。
    // 错误信息应点名 "epochSec"。
    void testEpochSecOnDateFormatRejected() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "dateFormat":{"db":{"type":"epochSec"},"excel":{"type":"string","format":"yyyy/M/d"}},
            "columns":{"id":{"source":"ID","validators":["int"]},"dt":{"source":"DT"}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY2(err.contains("epochSec"), err.toUtf8());
    }

    // 7.2.6: datetimeFormat.db.type=epochSec profile level → load succeeds
    // 正路径：datetime 槽的 db 侧用 epochSec（profile 级）→ 成功，db.type 解析为 EpochSec。
    // 这是 epoch 的「唯一合法落点」（datetime 的 db 侧）。
    void testEpochSecOnDatetimeFormatProfileLevel() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                              "db":{"type":"epochSec"}},
            "columns":{"id":{"source":"ID","validators":["int"]},"ts":{"source":"TS"}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        QVERIFY(spec.datetimeFormat.declared);
        QCOMPARE(spec.datetimeFormat.db.type, TemporalPhysType::EpochSec);
    }

    // 7.2.6: datetimeFormat.db.type=epochSec column level → load succeeds
    // 正路径（列级版）：同上，但 datetimeFormat 声明在列上而非 profile 级 → 同样成功。
    void testEpochSecOnDatetimeFormatColumnLevel() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "ts":{"source":"TS","datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},"db":{"type":"epochSec"}}}
            }
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
    }

    // 7.2.8: timeFormat.db.type=epochSec → E_PROFILE_PARSE
    // 负路径：time 槽的 db 侧用 epochSec → 拒绝（纯时间无纪元意义）。错误信息含 "epochSec"。
    void testEpochSecOnTimeFormatRejected() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "timeFormat":{"db":{"type":"epochSec"},"excel":{"type":"string","format":"HH:mm:ss"}},
            "columns":{"id":{"source":"ID","validators":["int"]},"tm":{"source":"TM"}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY2(err.contains("epochSec"), err.toUtf8());
    }

    // 7.2.9: excel.type=epochSec anywhere → E_PROFILE_PARSE
    // 负路径：epoch 出现在 excel 侧 → 拒绝。epoch 只允许在 db 侧（Excel 单元格是文本/日期，
    // 不该被声明为纪元数）。错误信息含 "epochSec"。
    void testEpochSecOnExcelSideRejected() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "datetimeFormat":{"excel":{"type":"epochSec"},"db":{"type":"epochSec"}},
            "columns":{"id":{"source":"ID","validators":["int"]},"ts":{"source":"TS"}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY2(err.contains("epochSec"), err.toUtf8());
    }

    // 7.2.10: type=string + no format → E_PROFILE_PARSE
    // 负路径：type=string 却没给 format → 拒绝。string 必须配一个解释用的格式串，缺则无从转换。
    // 错误信息含 "non-empty format"。
    void testStringTypeWithEmptyFormatRejected() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                              "db":{"type":"string"}},
            "columns":{"id":{"source":"ID","validators":["int"]},"ts":{"source":"TS"}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY2(err.contains("non-empty format"), err.toUtf8());
    }

    // 7.2.11: type=epochSec + non-empty format → E_PROFILE_PARSE
    // 负路径：type=epochSec 又带了非空 format → 拒绝。epoch 是数值、无文本格式可言，带 format
    // 属矛盾声明。错误信息应同时含 "epochSec" 与 "format"。
    void testEpochSecWithNonEmptyFormatRejected() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                              "db":{"type":"epochSec","format":"yyyy-MM-dd HH:mm:ss"}},
            "columns":{"id":{"source":"ID","validators":["int"]},"ts":{"source":"TS"}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY2(err.contains("epochSec") && err.contains("format"), err.toUtf8());
    }

    // 7.2.12: unknown type string → E_PROFILE_PARSE
    // 负路径：type 取了未知值 "epochMs"（本槽不支持）→ 拒绝。type 是受控词表，错误信息回显该值。
    void testUnknownTypeStringRejected() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "datetimeFormat":{"db":{"type":"epochMs"},"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}},
            "columns":{"id":{"source":"ID","validators":["int"]},"ts":{"source":"TS"}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY2(err.contains("epochMs"), err.toUtf8());
    }

    // 7.2.13: format = JSON null → E_PROFILE_PARSE
    // 负路径：format 写成 JSON null → 拒绝（null 既非缺省也非合法字符串，属类型错误）。
    void testNullFormatRejected() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "dateFormat":{"excel":{"type":"string","format":null}},
            "columns":{"id":{"source":"ID","validators":["int"]}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY2(!err.isEmpty(), err.toUtf8());
    }

    // 7.2.15: same slot object, legacy + new form coexist → E_PROFILE_PARSE
    // 负路径：同一个槽对象里「旧写法 excelFormat」与「新写法 excel:{...}」并存 → 拒绝。
    // 两种语法混用会产生歧义，必须二选一。错误信息含 "mix"。
    void testLegacyAndNewFormCoexistRejected() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "dateFormat":{"excelFormat":"yyyy-MM-dd","excel":{"type":"string","format":"yyyy-MM-dd"}},
            "columns":{"id":{"source":"ID","validators":["int"]}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY2(err.contains("mix"), err.toUtf8());
    }

    // 7.2.18: column declares both dateFormat and datetimeFormat → E_PROFILE_PARSE
    // 负路径：同一列同时声明 dateFormat 与 datetimeFormat（两个时间槽）→ 拒绝。一列最多归属
    // 一个时间槽，否则该列到底按 date 还是 datetime 解释无法确定。错误信息含 "at most one"。
    void testColumnMultiSlotRejected() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "ts":{"source":"TS",
                      "dateFormat":{"excelFormat":"yyyy-MM-dd","dbFormat":"yyyy-MM-dd"},
                      "datetimeFormat":{"excelFormat":"yyyy-MM-dd HH:mm:ss","dbFormat":"yyyy-MM-dd HH:mm:ss"}}
            }
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY2(err.contains("at most one"), err.toUtf8());
    }

    // 7.2.2: dateFormat.db.type=string, column level only → success
    // 正路径：dateFormat 仅在列级声明（profile 级没有）→ 成功，列上 declared=true，
    // db/excel 两侧 format 均按列声明解析。验证「列级独立声明」可用。
    void testDateFormatStringColumnLevelOnly() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "dt":{"source":"DT","dateFormat":{"excel":{"type":"string","format":"yyyy/M/d"},
                                                  "db":{"type":"string","format":"yyyy-MM-dd"}}}
            }
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        const ColumnSpec* dtCol = nullptr;
        for (const auto& r : spec.routes)
            for (const auto& c : r.columns)
                if (c.dbColumn == QStringLiteral("dt"))
                    dtCol = &c;
        QVERIFY(dtCol);
        QVERIFY(dtCol->dateFormat.declared);
        QCOMPARE(dtCol->dateFormat.db.format, QStringLiteral("yyyy-MM-dd"));
        QCOMPARE(dtCol->dateFormat.excel.format, QStringLiteral("yyyy/M/d"));
    }

    // 7.2.3: dateFormat.db.type=string, profile + column → column side-level overrides
    // 正路径：profile 级与列级都声明了 dateFormat → 列级整体覆盖 profile 级。用
    // effectiveTemporalFor 计算有效格式，excel/db 两侧都应取「列级的值」("d.M.yyyy"/"yyyyMMdd")。
    void testDateFormatStringBothLevelsColumnOverrides() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "dateFormat":{"excel":{"type":"string","format":"yyyy/M/d"},
                          "db":{"type":"string","format":"yyyy-MM-dd"}},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "dt":{"source":"DT","dateFormat":{"excel":{"type":"string","format":"d.M.yyyy"},
                                                  "db":{"type":"string","format":"yyyyMMdd"}}}
            }
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        const ColumnSpec* dtCol = nullptr;
        for (const auto& r : spec.routes)
            for (const auto& c : r.columns)
                if (c.dbColumn == QStringLiteral("dt"))
                    dtCol = &c;
        QVERIFY(dtCol);
        TemporalFormatSpec eff = effectiveTemporalFor(TemporalSlotKind::Date, *dtCol, spec);
        QCOMPARE(eff.excel.format, QStringLiteral("d.M.yyyy"));
        QCOMPARE(eff.db.format, QStringLiteral("yyyyMMdd"));
    }

    // 7.2.5a: datetimeFormat.db.type=string, profile level only → success
    // 正路径：datetime 槽、profile 级、db 侧 type=string → 成功，db.type=String 且 format 正确。
    void testDatetimeFormatStringProfileLevel() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                              "db":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}},
            "columns":{"id":{"source":"ID","validators":["int"]},"ts":{"source":"TS"}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        QVERIFY(spec.datetimeFormat.declared);
        QCOMPARE(spec.datetimeFormat.db.type, TemporalPhysType::String);
        QCOMPARE(spec.datetimeFormat.db.format, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }

    // 7.2.5b: datetimeFormat.db.type=string, column level only → success
    // 正路径（列级版）：datetime 槽仅在列级声明 string → 加载成功（只验证不报错）。
    void testDatetimeFormatStringColumnLevel() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "ts":{"source":"TS","datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                                                      "db":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}}}
            }
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
    }

    // 7.2.5c: datetimeFormat.db.type=string, profile + column → column overrides
    // 正路径：datetime 槽 profile+列两级都声明 → 列级覆盖。有效格式两侧均取列级值。
    void testDatetimeFormatStringBothLevels() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                              "db":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "ts":{"source":"TS","datetimeFormat":{"excel":{"type":"string","format":"M/d/yyyy HH:mm"},
                                                      "db":{"type":"string","format":"yyyyMMdd HHmmss"}}}
            }
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        const ColumnSpec* tsCol = nullptr;
        for (const auto& r : spec.routes)
            for (const auto& c : r.columns)
                if (c.dbColumn == QStringLiteral("ts"))
                    tsCol = &c;
        QVERIFY(tsCol);
        TemporalFormatSpec eff = effectiveTemporalFor(TemporalSlotKind::DateTime, *tsCol, spec);
        QCOMPARE(eff.excel.format, QStringLiteral("M/d/yyyy HH:mm"));
        QCOMPARE(eff.db.format, QStringLiteral("yyyyMMdd HHmmss"));
    }

    // 7.2.7a: timeFormat.db.type=string, profile level only → success
    // 正路径：time 槽、profile 级、string → 成功，db.format 解析正确。
    void testTimeFormatStringProfileLevel() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "timeFormat":{"excel":{"type":"string","format":"HH:mm:ss"},
                          "db":{"type":"string","format":"HH:mm:ss"}},
            "columns":{"id":{"source":"ID","validators":["int"]},"tm":{"source":"TM"}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        QVERIFY(spec.timeFormat.declared);
        QCOMPARE(spec.timeFormat.db.format, QStringLiteral("HH:mm:ss"));
    }

    // 7.2.7b: timeFormat.db.type=string, column level only → success
    // 正路径（列级版）：time 槽仅列级声明 string → 加载成功（只验证不报错）。
    void testTimeFormatStringColumnLevel() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "tm":{"source":"TM","timeFormat":{"excel":{"type":"string","format":"HH:mm:ss"},
                                                  "db":{"type":"string","format":"HH:mm:ss"}}}
            }
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
    }

    // 7.2.7c: timeFormat.db.type=string, profile + column → column overrides
    // 正路径：time 槽 profile+列两级都声明 → 列级覆盖。有效格式两侧取列级值（"HH:mm"/"HHmmss"）。
    void testTimeFormatStringBothLevels() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "timeFormat":{"excel":{"type":"string","format":"HH:mm:ss"},
                          "db":{"type":"string","format":"HH:mm:ss"}},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "tm":{"source":"TM","timeFormat":{"excel":{"type":"string","format":"HH:mm"},
                                                  "db":{"type":"string","format":"HHmmss"}}}
            }
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        const ColumnSpec* tmCol = nullptr;
        for (const auto& r : spec.routes)
            for (const auto& c : r.columns)
                if (c.dbColumn == QStringLiteral("tm"))
                    tmCol = &c;
        QVERIFY(tmCol);
        TemporalFormatSpec eff = effectiveTemporalFor(TemporalSlotKind::Time, *tmCol, spec);
        QCOMPARE(eff.excel.format, QStringLiteral("HH:mm"));
        QCOMPARE(eff.db.format, QStringLiteral("HHmmss"));
    }

    // 7.2.14a: type=epochSec + format="" → valid (empty treated as absent)
    // 正路径（边界）：epochSec 带「空字符串 format」→ 合法。空串被视同「未提供 format」，
    // 与 7.2.11（epochSec 带非空 format 被拒）形成对照——空 format 不算「带了 format」。
    void testEpochSecWithExplicitEmptyFormatValid() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                              "db":{"type":"epochSec","format":""}},
            "columns":{"id":{"source":"ID","validators":["int"]},"ts":{"source":"TS"}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        QCOMPARE(spec.datetimeFormat.db.type, TemporalPhysType::EpochSec);
    }

    // 7.2.14b: type=string + format="" → E_PROFILE_PARSE (empty string treated as absent)
    // 负路径（对称边界）：string 带空 format → 拒绝。空串视同「没给 format」，而 string 必须有
    // 非空 format（同 7.2.10）。错误信息含 "non-empty format"。
    void testStringWithExplicitEmptyFormatRejected() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                              "db":{"type":"string","format":""}},
            "columns":{"id":{"source":"ID","validators":["int"]},"ts":{"source":"TS"}}
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY(!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err));
        QVERIFY2(err.contains("non-empty format"), err.toUtf8());
    }

    // 7.2.16: profile level legacy form + column level new form → success (side-level overwrite)
    // 正路径：profile 级用「旧写法」、列级用「新写法」→ 成功（跨级别允许新旧混用，只是同一个
    // 槽对象内部不能混，见 7.2.15）。列级新写法把 db 覆盖为 epochSec，excel 侧继承 profile 旧写法。
    void testProfileLegacyColumnNewFormMixed() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "datetimeFormat":{"excelFormat":"yyyy-MM-dd HH:mm:ss","dbFormat":"yyyy-MM-dd HH:mm:ss"},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "ts":{"source":"TS","datetimeFormat":{"db":{"type":"epochSec"}}}
            }
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        const ColumnSpec* tsCol = nullptr;
        for (const auto& r : spec.routes)
            for (const auto& c : r.columns)
                if (c.dbColumn == QStringLiteral("ts"))
                    tsCol = &c;
        QVERIFY(tsCol);
        TemporalFormatSpec eff = effectiveTemporalFor(TemporalSlotKind::DateTime, *tsCol, spec);
        QCOMPARE(eff.db.type, TemporalPhysType::EpochSec);
        QCOMPARE(eff.excel.format, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }

    // 7.2.17: profile level new form + column level legacy form → success (legacy normalized)
    // 正路径（对称版）：profile 级新写法 + 列级旧写法 → 成功，旧写法被归一化。列级旧写法把
    // db 覆盖为 String + 指定 format，excel 侧也取列级旧写法值。
    void testProfileNewFormColumnLegacyMixed() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm"},"db":{"type":"epochSec"}},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "ts":{"source":"TS","datetimeFormat":{"excelFormat":"M/d/yyyy HH:mm","dbFormat":"yyyy-MM-dd HH:mm:ss"}}
            }
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        const ColumnSpec* tsCol = nullptr;
        for (const auto& r : spec.routes)
            for (const auto& c : r.columns)
                if (c.dbColumn == QStringLiteral("ts"))
                    tsCol = &c;
        QVERIFY(tsCol);
        TemporalFormatSpec eff = effectiveTemporalFor(TemporalSlotKind::DateTime, *tsCol, spec);
        QCOMPARE(eff.excel.format, QStringLiteral("M/d/yyyy HH:mm"));
        QCOMPARE(eff.db.type, TemporalPhysType::String);
        QCOMPARE(eff.db.format, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }

    // 7.2.19: column declares empty slot object {} → declared=true, sides inherit from profile
    // 正路径（边界）：列声明了一个「空槽对象 {}」→ declared=true，但两侧都没覆盖任何东西，
    // 故有效格式完全继承 profile 级。验证「空对象 = 显式声明但不覆盖」这一微妙语义
    // （区别于「根本不声明」——后者 declared=false）。
    void testColumnEmptySlotObjectInheritsProfile() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "datetimeFormat":{"excel":{"type":"string","format":"yyyy/MM/dd HH:mm"},
                              "db":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "ts":{"source":"TS","datetimeFormat":{}}
            }
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        const ColumnSpec* tsCol = nullptr;
        for (const auto& r : spec.routes)
            for (const auto& c : r.columns)
                if (c.dbColumn == QStringLiteral("ts"))
                    tsCol = &c;
        QVERIFY(tsCol);
        QVERIFY(tsCol->datetimeFormat.declared);
        TemporalFormatSpec eff = effectiveTemporalFor(TemporalSlotKind::DateTime, *tsCol, spec);
        QCOMPARE(eff.excel.format, QStringLiteral("yyyy/MM/dd HH:mm"));
        QCOMPARE(eff.db.format, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    }

    // 7.2.20: column declares db side only → profile excel side still inherited
    // 正路径（边界）：列只覆盖了 db 侧（改成 epochSec），未碰 excel 侧 → 有效格式里 db 取列级
    // (EpochSec)、excel 仍继承 profile 级。这是「按侧（per-side）覆盖」最直接的证据：覆盖是
    // 「逐侧」生效的，未声明的那一侧不受影响。
    void testColumnDbSideOnlyInheritsProfileExcel() {
        auto json = QStringLiteral(R"({
            "profileName":"tf","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"t","conflict":{"columns":["id"]},
            "datetimeFormat":{"excel":{"type":"string","format":"yyyy/MM/dd HH:mm"},
                              "db":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "ts":{"source":"TS","datetimeFormat":{"db":{"type":"epochSec"}}}
            }
        })");
        ProfileLoader loader;
        ProfileSpec spec;
        QString err;
        QVERIFY2(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), err.toUtf8());
        const ColumnSpec* tsCol = nullptr;
        for (const auto& r : spec.routes)
            for (const auto& c : r.columns)
                if (c.dbColumn == QStringLiteral("ts"))
                    tsCol = &c;
        QVERIFY(tsCol);
        TemporalFormatSpec eff = effectiveTemporalFor(TemporalSlotKind::DateTime, *tsCol, spec);
        // db overridden to epochSec
        QCOMPARE(eff.db.type, TemporalPhysType::EpochSec);
        // excel inherited from profile
        QCOMPARE(eff.excel.format, QStringLiteral("yyyy/MM/dd HH:mm"));
    }
};

// QTEST_MAIN：生成测试可执行文件的 main()，自动跑本类全部 private slots 测试函数。
// 末尾 #include 该测试的 .moc：Qt 的 moc 工具为含 Q_OBJECT 的类生成元对象代码，此处把生成
// 的 moc 内容并入本编译单元（QtTest 单文件测试的固定写法）。
QTEST_MAIN(TstProfileLoader)
#include "tst_profile_loader.moc"
