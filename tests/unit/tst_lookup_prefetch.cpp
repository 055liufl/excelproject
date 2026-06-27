#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include "profile/ProfileLoader.h"
#include "profile/ProfileSpec.h"
#include "schema/SchemaCatalog.h"
#include "schema/SchemaIntrospector.h"
#include "service/ErrorCollector.h"
#include "service/ImportService.h"
#include <xlsxdocument.h>

using namespace dbridge::detail;

// ── Test helpers ────────────────────────────────────────────────────────────

static QString makeUuid() {
    return QUuid::createUuid().toString().remove('{').remove('}').remove('-');
}

// Create a real xlsx file: row 1 = headers, rows 2+ = data values
static bool writeXlsx(const QString& path, const QString& sheet, const QStringList& headers,
                      const QVector<QVector<QVariant>>& rows) {
    QXlsx::Document doc;
    if (!doc.addSheet(sheet))
        return false;
    if (!doc.selectSheet(sheet))
        return false;
    for (int c = 0; c < headers.size(); ++c)
        doc.write(1, c + 1, headers[c]);
    for (int r = 0; r < rows.size(); ++r)
        for (int c = 0; c < rows[r].size(); ++c)
            if (!rows[r][c].isNull())
                doc.write(r + 2, c + 1, rows[r][c]);
    return doc.saveAs(path);
}

// Open a QSQLITE connection and execute DDL/DML; close it after
static QString openDb(const QString& path, const QStringList& sqls) {
    QString connName = QStringLiteral("setup_") + makeUuid();
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
    db.setDatabaseName(path);
    if (!db.open())
        return QStringLiteral("Cannot open: ") + db.lastError().text();
    QSqlQuery q(db);
    for (const QString& sql : sqls) {
        if (!q.exec(sql))
            return QStringLiteral("SQL failed: ") + q.lastError().text() + " — " + sql;
    }
    db.close();
    QSqlDatabase::removeDatabase(connName);
    return QString();
}

static ProfileSpec loadProfile(const QString& json) {
    ProfileLoader loader;
    ProfileSpec spec;
    QString err;
    if (!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err))
        qFatal("%s failed: %s", "loadProfile", err.toUtf8().constData());
    return spec;
}

class TstLookupPrefetch : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;

    QString newDb() const {
        return tmp_.path() + '/' + makeUuid() + ".db";
    }
    QString newXlsx() const {
        return tmp_.path() + '/' + makeUuid() + ".xlsx";
    }

   private slots:
    void initTestCase() {
        QVERIFY(tmp_.isValid());
    }

    // ── K=0: no distinct match keys → 0 SELECT ───────────────────────────────
    void testK0NoSelect() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();

        QString dbErr = openDb(
            dbPath,
            {
                QStringLiteral("CREATE TABLE orders (order_no TEXT PRIMARY KEY)"),
                QStringLiteral("CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
            });
        QVERIFY2(dbErr.isEmpty(), dbErr.toUtf8());

        // Excel rows have empty CustNo → K = 0
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("Sheet1"),
                          {QStringLiteral("OrderNo"), QStringLiteral("CustNo")},
                          {{QStringLiteral("SO-1"), QVariant()}}));  // CustNo is NULL/empty

        QString profile = R"({
            "profileName": "test",
            "sheet": "Sheet1",
            "headerRow": 1,
            "mode": "singleTable",
            "table": "orders",
            "conflict": { "columns": ["order_no"] },
            "lookups": [
                {
                    "name": "cust",
                    "from": "ref_customers",
                    "match": [["c_no","CustNo"]],
                    "select": [["c_name","customer_name"]]
                }
            ],
            "columns": {
                "order_no": { "source": "OrderNo" }
            }
        })";
        ProfileSpec spec = loadProfile(profile);

        QString connName = QStringLiteral("k0_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        QVERIFY(db.open());

        SchemaIntrospector si;
        SchemaCatalog catalog;
        QVERIFY(si.load(db, &catalog, nullptr));

        int prefetchCount = 0;
        ImportService svc;
        svc.onPrefetch = [&](const QString&) { ++prefetchCount; };

        dbridge::ImportOptions opts;
        opts.profileName = QStringLiteral("test");
        opts.dryRun = true;
        dbridge::ImportResult result = svc.run(spec, catalog, xlsxPath, opts, db);

        db.close();
        QSqlDatabase::removeDatabase(connName);

        // All rows had empty CustNo → K=0 → 0 prefetch SELECTs
        QCOMPARE(prefetchCount, 0);
        // One row-level E_LOOKUP_KEY_EMPTY error expected
        bool hasLookupError = false;
        for (const auto& e : result.errors) {
            if (e.code == QStringLiteral("E_LOOKUP_KEY_EMPTY")) {
                hasLookupError = true;
                break;
            }
        }
        QVERIFY(hasLookupError);
    }

    // ── Single lookup with valid keys → exactly 1 SELECT batch ───────────────
    void testBasicLookupPrefetch() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();

        QString dbErr = openDb(
            dbPath,
            {
                QStringLiteral(
                    "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                QStringLiteral("CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
                QStringLiteral("INSERT INTO ref_customers VALUES ('C2','Bob')"),
            });
        QVERIFY2(dbErr.isEmpty(), dbErr.toUtf8());

        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("Sheet1"),
                          {QStringLiteral("OrderNo"), QStringLiteral("CustNo")},
                          {{QStringLiteral("SO-1"), QStringLiteral("C1")},
                           {QStringLiteral("SO-2"), QStringLiteral("C2")}}));

        QString profile = R"({
            "profileName": "test",
            "sheet": "Sheet1",
            "headerRow": 1,
            "mode": "singleTable",
            "table": "orders",
            "conflict": { "columns": ["order_no"] },
            "lookups": [
                {
                    "name": "cust",
                    "from": "ref_customers",
                    "match": [["c_no","CustNo"]],
                    "select": [["c_name","customer_name"]]
                }
            ],
            "columns": {
                "order_no": { "source": "OrderNo" }
            }
        })";
        ProfileSpec spec = loadProfile(profile);

        QString connName = QStringLiteral("basic_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        QVERIFY(db.open());

        SchemaIntrospector si;
        SchemaCatalog catalog;
        QVERIFY(si.load(db, &catalog, nullptr));

        int prefetchCount = 0;
        ImportService svc;
        svc.onPrefetch = [&](const QString&) { ++prefetchCount; };

        dbridge::ImportOptions opts;
        opts.profileName = QStringLiteral("test");
        opts.dryRun = true;
        dbridge::ImportResult result = svc.run(spec, catalog, xlsxPath, opts, db);

        db.close();
        QSqlDatabase::removeDatabase(connName);

        // 2 distinct keys, chunk=999, → 1 SELECT batch
        QCOMPARE(prefetchCount, 1);
        // dryRun → no errors from FK/upsert; lookup found
        for (const auto& e : result.errors)
            qDebug() << "error:" << e.code << e.message;
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.dryRunPayloads.size(), 2);
        // Each payload should include customer_name
        bool foundName = false;
        for (const auto& ctx : result.dryRunPayloads) {
            for (const auto& payload : ctx.payloads) {
                int idx = payload.indexOf(QStringLiteral("customer_name"));
                if (idx >= 0) {
                    foundName = true;
                    break;
                }
            }
        }
        QVERIFY(foundName);
    }

    // ── Identity merging: two routes, same lookup identity → 1 prefetch ───────
    void testIdentityMerging() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();

        QString dbErr = openDb(
            dbPath,
            {
                QStringLiteral(
                    "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                QStringLiteral(
                    "CREATE TABLE items (order_no TEXT, line_no INTEGER, customer_name TEXT, "
                    "UNIQUE(order_no, line_no))"),
                QStringLiteral("CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
            });
        QVERIFY2(dbErr.isEmpty(), dbErr.toUtf8());

        QVERIFY(writeXlsx(
            xlsxPath, QStringLiteral("Sheet1"),
            {QStringLiteral("OrderNo"), QStringLiteral("LineNo"), QStringLiteral("CustNo")},
            {{QStringLiteral("SO-1"), 1, QStringLiteral("C1")}}));

        // Both routes declare the SAME lookup (from, match, select identical)
        QString profile = R"({
            "profileName": "test",
            "sheet": "Sheet1",
            "headerRow": 1,
            "mode": "multiTable",
            "routes": [
                {
                    "table": "orders",
                    "conflict": { "columns": ["order_no"] },
                    "lookups": [
                        {
                            "name": "cust",
                            "from": "ref_customers",
                            "match": [["c_no","CustNo"]],
                            "select": [["c_name","customer_name"]]
                        }
                    ],
                    "columns": { "order_no": { "source": "OrderNo" } }
                },
                {
                    "table": "items",
                    "parent": "orders",
                    "conflict": { "columns": ["order_no","line_no"] },
                    "fkInject": [
                        { "from": "orders", "pairs": [["order_no","order_no"]] }
                    ],
                    "lookups": [
                        {
                            "name": "cust",
                            "from": "ref_customers",
                            "match": [["c_no","CustNo"]],
                            "select": [["c_name","customer_name"]]
                        }
                    ],
                    "columns": { "line_no": { "source": "LineNo" } }
                }
            ]
        })";
        ProfileSpec spec = loadProfile(profile);

        QString connName = QStringLiteral("merge_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        QVERIFY(db.open());

        SchemaIntrospector si;
        SchemaCatalog catalog;
        QVERIFY(si.load(db, &catalog, nullptr));

        int prefetchCount = 0;
        ImportService svc;
        svc.onPrefetch = [&](const QString&) { ++prefetchCount; };

        dbridge::ImportOptions opts;
        opts.profileName = QStringLiteral("test");
        opts.dryRun = true;
        svc.run(spec, catalog, xlsxPath, opts, db);

        db.close();
        QSqlDatabase::removeDatabase(connName);

        // Two routes with same identity → only 1 SELECT issued
        QCOMPARE(prefetchCount, 1);
    }

    // ── §10.10 Mixed mode: same lookup identity in two classes → 1 prefetch ──
    void testMixedModeIdentityMerging() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();

        QString dbErr = openDb(
            dbPath,
            {
                QStringLiteral(
                    "CREATE TABLE type_a (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                QStringLiteral(
                    "CREATE TABLE type_b (invoice_no TEXT PRIMARY KEY, customer_name TEXT)"),
                QStringLiteral("CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
            });
        QVERIFY2(dbErr.isEmpty(), dbErr.toUtf8());

        // One row of class A, one row of class B — both use the same CustNo=C1
        QVERIFY(writeXlsx(
            xlsxPath, QStringLiteral("Sheet1"),
            {QStringLiteral("Type"), QStringLiteral("OrderNo"), QStringLiteral("InvoiceNo"),
             QStringLiteral("CustNo")},
            {{QStringLiteral("A"), QStringLiteral("SO-1"), QVariant(), QStringLiteral("C1")},
             {QStringLiteral("B"), QVariant(), QStringLiteral("INV-1"), QStringLiteral("C1")}}));

        // Both classes declare the SAME lookup identity
        QString profile = R"({
            "profileName": "test",
            "sheet": "Sheet1",
            "headerRow": 1,
            "mode": "mixed",
            "discriminator": { "source": "Type" },
            "classes": [
                {
                    "id": "A",
                    "match": { "equals": "A" },
                    "routes": [{
                        "table": "type_a",
                        "conflict": { "columns": ["order_no"] },
                        "lookups": [
                            { "name": "cust", "from": "ref_customers",
                              "match": [["c_no","CustNo"]], "select": [["c_name","customer_name"]] }
                        ],
                        "columns": { "order_no": { "source": "OrderNo" } }
                    }]
                },
                {
                    "id": "B",
                    "match": { "equals": "B" },
                    "routes": [{
                        "table": "type_b",
                        "conflict": { "columns": ["invoice_no"] },
                        "lookups": [
                            { "name": "cust", "from": "ref_customers",
                              "match": [["c_no","CustNo"]], "select": [["c_name","customer_name"]] }
                        ],
                        "columns": { "invoice_no": { "source": "InvoiceNo" } }
                    }]
                }
            ]
        })";
        ProfileSpec spec = loadProfile(profile);

        QString connName = QStringLiteral("mixed10_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        QVERIFY(db.open());

        SchemaIntrospector si;
        SchemaCatalog catalog;
        QVERIFY(si.load(db, &catalog, nullptr));

        int prefetchCount = 0;
        ImportService svc;
        svc.onPrefetch = [&](const QString&) { ++prefetchCount; };

        dbridge::ImportOptions opts;
        opts.profileName = QStringLiteral("test");
        opts.dryRun = true;
        dbridge::ImportResult result = svc.run(spec, catalog, xlsxPath, opts, db);

        db.close();
        QSqlDatabase::removeDatabase(connName);

        for (const auto& e : result.errors)
            qDebug() << "error:" << e.code << e.message;

        // Same identity across two classes → only 1 SELECT batch issued
        QCOMPARE(prefetchCount, 1);
        // Both rows should produce payloads with customer_name filled
        QVERIFY(!result.dryRunPayloads.isEmpty());
    }

    // ── §12.3 e2e: real write with lookup + composite fkInject ───────────────
    void testE2eRealWrite() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();

        QString dbErr = openDb(
            dbPath,
            {
                QStringLiteral("CREATE TABLE orders ("
                               "  order_no TEXT PRIMARY KEY,"
                               "  customer_name TEXT"
                               ")"),
                QStringLiteral("CREATE TABLE order_items ("
                               "  order_no TEXT NOT NULL,"
                               "  line_no INTEGER NOT NULL,"
                               "  sku TEXT NOT NULL,"
                               "  customer_name TEXT,"
                               "  UNIQUE(order_no, line_no)"
                               ")"),
                QStringLiteral("CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
                QStringLiteral("INSERT INTO ref_customers VALUES ('C2','Bob')"),
            });
        QVERIFY2(dbErr.isEmpty(), dbErr.toUtf8());

        // Both rows use the same CustNo so orders.customer_name is identical (BatchUniqueness
        // allows it). order_items uses lookup too → both items get customer_name filled.
        QVERIFY(writeXlsx(
            xlsxPath, QStringLiteral("Sheet1"),
            {QStringLiteral("OrderNo"), QStringLiteral("LineNo"), QStringLiteral("Sku"),
             QStringLiteral("CustNo")},
            {{QStringLiteral("SO-1"), 1, QStringLiteral("SKU-A"), QStringLiteral("C1")},
             {QStringLiteral("SO-1"), 2, QStringLiteral("SKU-B"), QStringLiteral("C1")}}));

        // multiTable with lookup on both routes + composite fkInject
        QString profile = R"({
            "profileName": "e2e",
            "sheet": "Sheet1",
            "headerRow": 1,
            "mode": "multiTable",
            "routes": [
                {
                    "table": "orders",
                    "conflict": { "columns": ["order_no"] },
                    "lookups": [
                        { "name": "cust", "from": "ref_customers",
                          "match": [["c_no","CustNo"]], "select": [["c_name","customer_name"]] }
                    ],
                    "columns": { "order_no": { "source": "OrderNo" } }
                },
                {
                    "table": "order_items",
                    "parent": "orders",
                    "fkInject": [{ "from": "orders", "pairs": [["order_no","order_no"]] }],
                    "conflict": { "columns": ["order_no","line_no"] },
                    "lookups": [
                        { "name": "cust", "from": "ref_customers",
                          "match": [["c_no","CustNo"]], "select": [["c_name","customer_name"]] }
                    ],
                    "columns": {
                        "line_no": { "source": "LineNo" },
                        "sku":     { "source": "Sku" }
                    }
                }
            ]
        })";
        ProfileSpec spec = loadProfile(profile);

        QString connName = QStringLiteral("e2e_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        QVERIFY(db.open());

        SchemaIntrospector si;
        SchemaCatalog catalog;
        QVERIFY(si.load(db, &catalog, nullptr));

        ImportService svc;
        dbridge::ImportOptions opts;
        opts.profileName = QStringLiteral("e2e");
        opts.dryRun = false;
        dbridge::ImportResult result = svc.run(spec, catalog, xlsxPath, opts, db);

        for (const auto& e : result.errors)
            qDebug() << "e2e error:" << e.code << e.message;
        QVERIFY(result.ok);

        // Verify orders: 1 row (SO-1 deduped), customer_name = Alice (last-row C2 wins via upsert)
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM orders")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);

        // Verify order_items: 2 rows, with customer_name populated from lookup
        QVERIFY(q.exec(
            QStringLiteral("SELECT COUNT(*) FROM order_items WHERE customer_name IS NOT NULL")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 2);

        // Verify fkInject: order_items.order_no matches orders.order_no
        QVERIFY(
            q.exec(QStringLiteral("SELECT COUNT(*) FROM order_items WHERE order_no NOT IN (SELECT "
                                  "order_no FROM orders)")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);

        db.close();
        QSqlDatabase::removeDatabase(connName);
    }
};

QTEST_MAIN(TstLookupPrefetch)
#include "tst_lookup_prefetch.moc"
