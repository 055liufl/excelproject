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

// ── Shared helpers ──────────────────────────────────────────────────────────

static QString makeUuid() {
    return QUuid::createUuid().toString().remove('{').remove('}').remove('-');
}

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

static QString setupDb(const QString& path, const QStringList& sqls) {
    QString conn = QStringLiteral("setup_") + makeUuid();
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(path);
    if (!db.open())
        return QStringLiteral("open failed: ") + db.lastError().text();
    QSqlQuery q(db);
    for (const QString& sql : sqls)
        if (!q.exec(sql))
            return sql + " failed: " + q.lastError().text();
    db.close();
    QSqlDatabase::removeDatabase(conn);
    return {};
}

static ProfileSpec parseProfile(const QString& json) {
    ProfileLoader loader;
    ProfileSpec spec;
    QString err;
    Q_ASSERT_X(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), "parseProfile",
               err.toUtf8());
    return spec;
}

struct RunResult {
    dbridge::ImportResult result;
    SchemaCatalog catalog;
};

static RunResult runDryRun(const QString& dbPath, const QString& xlsxPath,
                           const ProfileSpec& spec) {
    QString conn = QStringLiteral("run_") + makeUuid();
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(dbPath);
    db.open();

    SchemaIntrospector si;
    RunResult res;
    si.load(db, &res.catalog, nullptr);

    ImportService svc;
    dbridge::ImportOptions opts;
    opts.profileName = spec.name;
    opts.dryRun = true;
    res.result = svc.run(spec, res.catalog, xlsxPath, opts, db);

    db.close();
    QSqlDatabase::removeDatabase(conn);
    return res;
}

static bool hasCode(const dbridge::ImportResult& r, const QString& code) {
    for (const auto& e : r.errors)
        if (e.code == code)
            return true;
    return false;
}

// ── Test class ──────────────────────────────────────────────────────────────

class TstLookupSemantics : public QObject {
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

    // §5.4 zero hits → E_LOOKUP_NOT_FOUND
    void testZeroHit() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();
        QVERIFY2(
            setupDb(dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        // No rows in ref_customers
                    })
                .isEmpty(),
            "DB setup failed");
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("OrderNo"), QStringLiteral("CustNo")},
                          {{QStringLiteral("SO-1"), QStringLiteral("X999")}}));
        auto spec = parseProfile(R"({
            "profileName": "test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers","match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");
        auto res = runDryRun(dbPath, xlsxPath, spec);
        QVERIFY(hasCode(res.result, QStringLiteral("E_LOOKUP_NOT_FOUND")));
    }

    // §5.6 one hit → value propagated into dryRunPayloads
    void testOneHit() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();
        QVERIFY2(
            setupDb(dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
                    })
                .isEmpty(),
            "DB setup failed");
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("OrderNo"), QStringLiteral("CustNo")},
                          {{QStringLiteral("SO-1"), QStringLiteral("C1")}}));
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers","match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");
        auto res = runDryRun(dbPath, xlsxPath, spec);
        QVERIFY2(res.result.errors.isEmpty(),
                 res.result.errors.isEmpty() ? "" : res.result.errors[0].message.toUtf8());
        QCOMPARE(res.result.dryRunPayloads.size(), 1);
        const auto& payload = res.result.dryRunPayloads[0].payloads[0];
        int idx = payload.indexOf(QStringLiteral("customer_name"));
        QVERIFY(idx >= 0);
        QCOMPARE(payload.binds[idx].toString(), QStringLiteral("Alice"));
    }

    // §5.5 N hits → E_LOOKUP_AMBIGUOUS
    void testNHits() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();
        QVERIFY2(
            setupDb(dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral("CREATE TABLE ref_customers (c_no TEXT, c_name TEXT)"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C1','AliceDup')"),
                    })
                .isEmpty(),
            "DB setup failed");
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("OrderNo"), QStringLiteral("CustNo")},
                          {{QStringLiteral("SO-1"), QStringLiteral("C1")}}));
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers","match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");
        auto res = runDryRun(dbPath, xlsxPath, spec);
        QVERIFY(hasCode(res.result, QStringLiteral("E_LOOKUP_AMBIGUOUS")));
        // Error message should mention deduplication
        for (const auto& e : res.result.errors)
            if (e.code == QStringLiteral("E_LOOKUP_AMBIGUOUS"))
                QVERIFY2(e.message.contains(QStringLiteral("deduplicat")), e.message.toUtf8());
    }

    // §5.2 empty match keys: null / "" / " " → E_LOOKUP_KEY_EMPTY
    void testEmptyKeyNull() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();
        QVERIFY2(
            setupDb(dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                    })
                .isEmpty(),
            "DB setup failed");
        // null CustNo
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("OrderNo"), QStringLiteral("CustNo")},
                          {{QStringLiteral("SO-1"), QVariant()}}));
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers","match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");
        auto res = runDryRun(dbPath, xlsxPath, spec);
        QVERIFY(hasCode(res.result, QStringLiteral("E_LOOKUP_KEY_EMPTY")));
    }

    void testEmptyKeyWhitespace() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();
        QVERIFY2(
            setupDb(dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                    })
                .isEmpty(),
            "DB setup failed");
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("OrderNo"), QStringLiteral("CustNo")},
                          {{QStringLiteral("SO-1"), QStringLiteral("  ")}}));  // whitespace only
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers","match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");
        auto res = runDryRun(dbPath, xlsxPath, spec);
        QVERIFY(hasCode(res.result, QStringLiteral("E_LOOKUP_KEY_EMPTY")));
    }

    // §5.2 numeric zero is NOT empty
    void testNumericZeroNotEmpty() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();
        QVERIFY2(
            setupDb(dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no INTEGER PRIMARY KEY, c_name TEXT)"),
                        QStringLiteral("INSERT INTO ref_customers VALUES (0,'Zero Corp')"),
                    })
                .isEmpty(),
            "DB setup failed");
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("OrderNo"), QStringLiteral("CustId")},
                          {{QStringLiteral("SO-1"), 0}}));  // numeric zero
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers","match":[["c_no","CustId"]],"select":[["c_name","customer_name"]]}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");
        auto res = runDryRun(dbPath, xlsxPath, spec);
        // Zero should NOT produce E_LOOKUP_KEY_EMPTY
        QVERIFY(!hasCode(res.result, QStringLiteral("E_LOOKUP_KEY_EMPTY")));
        // Should find the row
        QVERIFY(!hasCode(res.result, QStringLiteral("E_LOOKUP_NOT_FOUND")));
    }

    // §5.6 NULL passthrough: G select col is NULL → no error, NULL in payload
    void testNullPassthrough() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();
        QVERIFY2(
            setupDb(dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        QStringLiteral(
                            "INSERT INTO ref_customers VALUES ('C1', NULL)"),  // c_name is NULL
                    })
                .isEmpty(),
            "DB setup failed");
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("OrderNo"), QStringLiteral("CustNo")},
                          {{QStringLiteral("SO-1"), QStringLiteral("C1")}}));
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers","match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");
        auto res = runDryRun(dbPath, xlsxPath, spec);
        // No lookup error — NULL is transparently passed
        QVERIFY2(res.result.errors.isEmpty(),
                 res.result.errors.isEmpty() ? "" : res.result.errors[0].message.toUtf8());
        QCOMPARE(res.result.dryRunPayloads.size(), 1);
        const auto& payload = res.result.dryRunPayloads[0].payloads[0];
        int idx = payload.indexOf(QStringLiteral("customer_name"));
        QVERIFY(idx >= 0);
        QVERIFY(payload.binds[idx].isNull());
    }

    // §10.9 Lookup output participates in conflict.columns
    void testLookupOutputInConflict() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();
        QVERIFY2(
            setupDb(
                dbPath,
                {
                    QStringLiteral("CREATE TABLE items (tenant_id TEXT, line_no INTEGER, PRIMARY "
                                   "KEY(tenant_id,line_no))"),
                    QStringLiteral("CREATE TABLE ref_tenants (t_code TEXT PRIMARY KEY, t_id TEXT)"),
                    QStringLiteral("INSERT INTO ref_tenants VALUES ('ACME','T001')"),
                })
                .isEmpty(),
            "DB setup failed");
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("TenantCode"), QStringLiteral("LineNo")},
                          {{QStringLiteral("ACME"), 1}}));
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"items",
            "conflict":{"columns":["tenant_id","line_no"]},
            "lookups":[{"name":"t","from":"ref_tenants","match":[["t_code","TenantCode"]],"select":[["t_id","tenant_id"]]}],
            "columns":{"line_no":{"source":"LineNo"}}
        })");
        auto res = runDryRun(dbPath, xlsxPath, spec);
        QVERIFY2(res.result.errors.isEmpty(),
                 res.result.errors.isEmpty() ? "" : res.result.errors[0].message.toUtf8());
        QCOMPARE(res.result.dryRunPayloads.size(), 1);
        const auto& payload = res.result.dryRunPayloads[0].payloads[0];
        // conflict key should include tenant_id with value T001
        bool tenantInConflict = false;
        for (int i = 0; i < payload.conflictKey.size(); ++i) {
            if (payload.conflictKey[i] == QStringLiteral("tenant_id") &&
                payload.conflictVals[i].toString() == QStringLiteral("T001")) {
                tenantInConflict = true;
            }
        }
        QVERIFY2(tenantInConflict, "tenant_id not found in conflictVals after lookup");
    }

    // §10.8 cascade suppression: parent lookup fail → child no secondary error
    void testCascadeSuppression() {
        QString dbPath = newDb();
        QString xlsxPath = newXlsx();
        QVERIFY2(
            setupDb(dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral("CREATE TABLE items (order_no TEXT, line_no INTEGER, "
                                       "UNIQUE(order_no,line_no))"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        // No ref_customers rows → lookup will fail
                    })
                .isEmpty(),
            "DB setup failed");
        QVERIFY(writeXlsx(
            xlsxPath, QStringLiteral("S"),
            {QStringLiteral("OrderNo"), QStringLiteral("LineNo"), QStringLiteral("CustNo")},
            {{QStringLiteral("SO-1"), 1, QStringLiteral("X999")}}));
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"multiTable",
            "routes":[
                {
                    "table":"orders",
                    "conflict":{"columns":["order_no"]},
                    "lookups":[{"name":"c","from":"ref_customers","match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
                    "columns":{"order_no":{"source":"OrderNo"}}
                },
                {
                    "table":"items","parent":"orders",
                    "conflict":{"columns":["order_no","line_no"]},
                    "fkInject":[{"from":"orders","pairs":[["order_no","order_no"]]}],
                    "columns":{"line_no":{"source":"LineNo"}}
                }
            ]
        })");
        auto res = runDryRun(dbPath, xlsxPath, spec);
        // Exactly 1 error: E_LOOKUP_NOT_FOUND on orders; cascade suppression silently drops items
        QCOMPARE(res.result.errors.size(), 1);
        QCOMPARE(res.result.errors.first().code, QStringLiteral("E_LOOKUP_NOT_FOUND"));
    }
};

QTEST_MAIN(TstLookupSemantics)
#include "tst_lookup_semantics.moc"
