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
                    "fkInject": { "from": "orders.order_no", "to": "order_items.order_no" },
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
        QVERIFY(spec.routes[1].fkInject.has_value());
        QCOMPARE(spec.routes[1].fkInject->fromTable, QStringLiteral("orders"));
        QCOMPARE(spec.routes[1].fkInject->toColumn, QStringLiteral("order_no"));
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
};

QTEST_MAIN(TstProfileLoader)
#include "tst_profile_loader.moc"
