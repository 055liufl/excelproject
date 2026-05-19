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
};

QTEST_MAIN(TstProfileLoader)
#include "tst_profile_loader.moc"
