#include <QJsonDocument>
#include <QtTest>

#include "profile/ProfileLoader.h"
#include "profile/ProfileSpec.h"

using namespace dbridge::detail;

class TstProfileLoader : public QObject {
    Q_OBJECT

   private slots:
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

    // §10.3 — fkInject new array format
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

    // §10.3 — lookup parsing
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

    void testDateFormatTimeTokenRejected() {
        // dateFormat must not contain time tokens (H, h, m, s, z, t, a, A)
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

    void testTimeFormatDateTokenRejected() {
        // timeFormat must not contain date tokens (y, M, d)
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

    void testExportOnMissingNoEffectWarningWhenRoundtripFalse() {
        // 2.4: exportRoundtrip=false + explicit exportOnMissing → info-level load warning
        // Warning is stored in spec.loadWarnings (non-blocking).
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

    // 7.2.1: dateFormat.db.type=string, profile level → load succeeds
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

QTEST_MAIN(TstProfileLoader)
#include "tst_profile_loader.moc"
