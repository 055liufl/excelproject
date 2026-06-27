// Unit + integration tests for export-direction reverse lookup (add-export-reverse-lookup).
//
// Tests 7.1-7.11 from tasks.md.
//
// Test strategy: Use a real SQLite in-memory (or temp-file) DB. Run ExportService.run()
// against a temp xlsx path, then read the xlsx back with QXlsx::Document to verify cell values.

#include "dbridge/Errors.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include "profile/ProfileLoader.h"
#include "profile/ProfileSpec.h"
#include "profile/ProfileValidator.h"
#include "schema/SchemaCatalog.h"
#include "schema/SchemaIntrospector.h"
#include "service/ErrorCollector.h"
#include "service/ExportService.h"
#include "service/ImportService.h"
#include <xlsxcellrange.h>
#include <xlsxdocument.h>

using namespace dbridge::detail;

// ── Shared helpers ───────────────────────────────────────────────────────────

static QString makeUuid() {
    return QUuid::createUuid().toString().remove('{').remove('}').remove('-');
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
            return sql + QStringLiteral(" failed: ") + q.lastError().text();
    db.close();
    QSqlDatabase::removeDatabase(conn);
    return {};
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

static ProfileSpec parseProfile(const QString& json) {
    ProfileLoader loader;
    ProfileSpec spec;
    QString err;
    if (!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err))
        qFatal("%s failed: %s", "parseProfile", err.toUtf8().constData());
    return spec;
}

struct ExportRunner {
    QString dbPath;
    QString xlsxPath;
    SchemaCatalog catalog;

    dbridge::ExportResult run(const ProfileSpec& spec, const QString& outXlsx = QString()) {
        QString out = outXlsx.isEmpty() ? xlsxPath : outXlsx;
        QString conn = QStringLiteral("run_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        db.open();

        SchemaIntrospector si;
        si.load(db, &catalog, nullptr);

        ExportService svc;
        dbridge::ExportOptions opts;
        auto res = svc.run(spec, catalog, out, opts, db);

        db.close();
        QSqlDatabase::removeDatabase(conn);
        return res;
    }
};

// Read a cell value from a saved xlsx file (1-based row/col).
static QVariant readXlsxCell(const QString& path, const QString& sheet, int row, int col) {
    QXlsx::Document doc(path);
    if (!doc.selectSheet(sheet))
        return QVariant();
    return doc.read(row, col);
}

// Read all rows (including header row 1) from a saved xlsx.
// Returns header list and data rows as strings.
static void readXlsxSheet(const QString& path, const QString& sheet, QStringList* headers,
                          QVector<QStringList>* rows) {
    QXlsx::Document doc(path);
    if (!doc.selectSheet(sheet))
        return;
    // Find dimensions
    int maxRow = 0, maxCol = 0;
    QXlsx::CellRange dim = doc.dimension();
    maxRow = dim.lastRow();
    maxCol = dim.lastColumn();

    for (int c = 1; c <= maxCol; ++c)
        headers->append(doc.read(1, c).toString());

    for (int r = 2; r <= maxRow; ++r) {
        QStringList dataRow;
        for (int c = 1; c <= maxCol; ++c)
            dataRow.append(doc.read(r, c).toString());
        rows->append(dataRow);
    }
}

static bool hasCode(const dbridge::ExportResult& r, const QString& code) {
    for (const auto& e : r.errors)
        if (e.code == code)
            return true;
    return false;
}

// ── Test class ───────────────────────────────────────────────────────────────

class TstReverseLookupExport : public QObject {
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

    // 7.1a: single-pair lookup → A column appears, H column absent in output
    void testSinglePairRoundtrip() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C2','Bob')"),
                        QStringLiteral("INSERT INTO orders VALUES ('SO-1','Alice')"),
                        QStringLiteral("INSERT INTO orders VALUES ('SO-2','Bob')"),
                    })
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers",
                        "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
            "columns":{"order_no":{"source":"OrderNo"}},
            "export":{"orderBy":["order_no"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 2);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        // H-col (customer_name) should NOT appear; A-col (CustNo) SHOULD appear
        QVERIFY2(headers.contains(QStringLiteral("CustNo")), "CustNo (A) not in headers");
        QVERIFY2(!headers.contains(QStringLiteral("customer_name")),
                 "customer_name (H) should be absent");

        // Row SO-1 → CustNo = C1, Row SO-2 → CustNo = C2
        int custNoCol = headers.indexOf(QStringLiteral("CustNo"));
        int orderNoCol = headers.indexOf(QStringLiteral("OrderNo"));
        QVERIFY(custNoCol >= 0 && orderNoCol >= 0);

        // Find rows by order_no
        QString r0CustNo, r1CustNo;
        for (const auto& row : rows) {
            if (row[orderNoCol] == QStringLiteral("SO-1"))
                r0CustNo = row[custNoCol];
            else if (row[orderNoCol] == QStringLiteral("SO-2"))
                r1CustNo = row[custNoCol];
        }
        QCOMPARE(r0CustNo, QStringLiteral("C1"));
        QCOMPARE(r1CustNo, QStringLiteral("C2"));
    }

    // 7.1b: composite-match (two match pairs) → both A-cols restored
    void testCompositePairRoundtrip() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {
                    QStringLiteral("CREATE TABLE items (id INTEGER PRIMARY KEY, tenant_id TEXT, "
                                   "line_no INTEGER)"),
                    QStringLiteral("CREATE TABLE ref_sku (t_code TEXT, sku_code TEXT, tenant_id "
                                   "TEXT, line_no INTEGER, PRIMARY KEY(t_code,sku_code))"),
                    QStringLiteral("INSERT INTO ref_sku VALUES ('ACME','SKU-A','T001',10)"),
                    QStringLiteral("INSERT INTO ref_sku VALUES ('ACME','SKU-B','T001',20)"),
                    QStringLiteral("INSERT INTO items VALUES (1,'T001',10)"),
                    QStringLiteral("INSERT INTO items VALUES (2,'T001',20)"),
                })
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"items","conflict":{"columns":["id"]},
            "lookups":[{"name":"sku","from":"ref_sku",
                        "match":[["t_code","TenantCode"],["sku_code","SkuCode"]],
                        "select":[["tenant_id","tenant_id"],["line_no","line_no"]]}],
            "columns":{"id":{"source":"Id"}},
            "export":{"orderBy":["id"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 2);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        QVERIFY(headers.contains(QStringLiteral("TenantCode")));
        QVERIFY(headers.contains(QStringLiteral("SkuCode")));
        QVERIFY(!headers.contains(QStringLiteral("tenant_id")));
        QVERIFY(!headers.contains(QStringLiteral("line_no")));

        int tcCol = headers.indexOf(QStringLiteral("TenantCode"));
        int skuCol = headers.indexOf(QStringLiteral("SkuCode"));
        QCOMPARE(rows[0][tcCol], QStringLiteral("ACME"));
        QCOMPARE(rows[0][skuCol], QStringLiteral("SKU-A"));
        QCOMPARE(rows[1][tcCol], QStringLiteral("ACME"));
        QCOMPARE(rows[1][skuCol], QStringLiteral("SKU-B"));
    }

    // 7.1c: identity merging — two routes with same (from,match,select) share one prefetch.
    // Verifies no duplicate errors and correct output.
    void testIdentityMerging() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral("CREATE TABLE items (order_no TEXT, line_no INTEGER, "
                                       "customer_name TEXT, PRIMARY KEY(order_no,line_no))"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
                        QStringLiteral("INSERT INTO orders VALUES ('SO-1','Alice')"),
                        QStringLiteral("INSERT INTO items VALUES ('SO-1',1,'Alice')"),
                    })
                .isEmpty(),
            "DB setup failed");

        // Two routes, same identity key on lookup
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"multiTable",
            "routes":[
                {
                    "table":"orders","conflict":{"columns":["order_no"]},
                    "lookups":[{"name":"c","from":"ref_customers",
                                "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
                    "columns":{"order_no":{"source":"OrderNo"}}
                },
                {
                    "table":"items","parent":"orders",
                    "conflict":{"columns":["order_no","line_no"]},
                    "fkInject":[{"from":"orders","pairs":[["order_no","order_no"]]}],
                    "lookups":[{"name":"c","from":"ref_customers",
                                "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
                    "columns":{"line_no":{"source":"LineNo"}}
                }
            ],
            "export":{"orderBy":["order_no","line_no"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        QVERIFY(headers.contains(QStringLiteral("CustNo")));
        QVERIFY(!headers.contains(QStringLiteral("customer_name")));
        int custNoCol = headers.indexOf(QStringLiteral("CustNo"));
        for (const auto& row : rows)
            QCOMPARE(row[custNoCol], QStringLiteral("C1"));
    }

    // 7.2: round-trip — import then export produces same A-column values.
    void testFullRoundtrip() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();
        QString importXlsx = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C2','Bob')"),
                    })
                .isEmpty(),
            "DB setup failed");

        // Write import xlsx: OrderNo + CustNo
        QVERIFY(writeXlsx(importXlsx, QStringLiteral("S"),
                          {QStringLiteral("OrderNo"), QStringLiteral("CustNo")},
                          {{QStringLiteral("SO-1"), QStringLiteral("C1")},
                           {QStringLiteral("SO-2"), QStringLiteral("C2")}}));

        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers",
                        "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
            "columns":{"order_no":{"source":"OrderNo"}},
            "export":{"orderBy":["order_no"]}
        })");

        // Import
        {
            QString conn = QStringLiteral("import_") + makeUuid();
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(runner.dbPath);
            db.open();
            SchemaCatalog cat;
            SchemaIntrospector si;
            si.load(db, &cat, nullptr);
            ImportService isvc;
            dbridge::ImportOptions iopts;
            auto ires = isvc.run(spec, cat, importXlsx, iopts, db);
            QVERIFY2(ires.errors.isEmpty(),
                     ires.errors.isEmpty() ? "" : ires.errors[0].message.toUtf8());
            db.close();
            QSqlDatabase::removeDatabase(conn);
        }

        // Export
        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 2);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        int custNoCol = headers.indexOf(QStringLiteral("CustNo"));
        int orderNoCol = headers.indexOf(QStringLiteral("OrderNo"));
        QVERIFY(custNoCol >= 0 && orderNoCol >= 0);

        QString c1Val, c2Val;
        for (const auto& row : rows) {
            if (row[orderNoCol] == QStringLiteral("SO-1"))
                c1Val = row[custNoCol];
            if (row[orderNoCol] == QStringLiteral("SO-2"))
                c2Val = row[custNoCol];
        }
        QCOMPARE(c1Val, QStringLiteral("C1"));
        QCOMPARE(c2Val, QStringLiteral("C2"));
    }

    // 7.3: exportRoundtrip: false → H cols kept as-is, A col absent
    void testRoundtripFalse() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
                        QStringLiteral("INSERT INTO orders VALUES ('SO-1','Alice')"),
                    })
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers",
                        "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]],
                        "exportRoundtrip":false}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        // With exportRoundtrip=false: H-col (customer_name) stays, A-col (CustNo) absent
        QVERIFY2(headers.contains(QStringLiteral("customer_name")),
                 "customer_name (H) should be present");
        QVERIFY2(!headers.contains(QStringLiteral("CustNo")), "CustNo (A) should be absent");
    }

    // 7.4a: exportOnMissing: "error" (default) → E_REVERSE_LOOKUP_NOT_FOUND, entire row skipped
    // H-03 fix: exportOnMissing="error" must skip the entire row (rowSkip=true), matching the
    // OpenSpec contract. The error is still reported. Previously (H-04 fix) only the A-column was
    // cleared, but that deviates from the spec which says "error" means skip-the-row.
    void testOnMissingError() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        // No rows in ref_customers → all lookups will miss
                        QStringLiteral("INSERT INTO orders VALUES ('SO-1','NoSuchCustomer')"),
                    })
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers",
                        "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]],
                        "exportOnMissing":"error"}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");

        auto res = runner.run(spec);
        QVERIFY(hasCode(res, QStringLiteral("E_REVERSE_LOOKUP_NOT_FOUND")));
        // H-03 fix: row is skipped entirely when exportOnMissing="error" and lookup misses.
        QCOMPARE(res.writtenRows, 0);
    }

    // 7.4b: exportOnMissing: "null" → A-col written as empty, no error, row continues
    void testOnMissingNull() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        QStringLiteral("INSERT INTO orders VALUES ('SO-1','NoSuchCustomer')"),
                    })
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers",
                        "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]],
                        "exportOnMissing":"null"}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 1);  // row written with NULL A-col

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);
        int custNoCol = headers.indexOf(QStringLiteral("CustNo"));
        QVERIFY(custNoCol >= 0);
        QVERIFY(rows[0][custNoCol].isEmpty());  // NULL → empty string in xlsx
    }

    // 7.4c: exportOnMissing: "skip" → row written (no error), A-col is empty
    void testOnMissingSkip() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        QStringLiteral("INSERT INTO orders VALUES ('SO-1','NoSuchCustomer')"),
                    })
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers",
                        "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]],
                        "exportOnMissing":"skip"}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 1);  // row written with empty A-col, no error
    }

    // 7.5: E_REVERSE_LOOKUP_AMBIGUOUS — G table has duplicate select-col values
    void testAmbiguous() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral("CREATE TABLE ref_customers (c_no TEXT, c_name TEXT)"),
                        // Two rows with same c_name → ambiguous reverse lookup
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C2','Alice')"),
                        QStringLiteral("INSERT INTO orders VALUES ('SO-1','Alice')"),
                    })
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers",
                        "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");

        auto res = runner.run(spec);
        QVERIFY(hasCode(res, QStringLiteral("E_REVERSE_LOOKUP_AMBIGUOUS")));
        QCOMPARE(res.writtenRows, 0);
    }

    // 7.6: prefetch failure (SQL error on G table) → E_REVERSE_LOOKUP_QUERY_FAILED, no rows
    void testPrefetchFailed() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        // ref_customers does NOT exist → SELECT from it will fail
                        QStringLiteral("INSERT INTO orders VALUES ('SO-1','Alice')"),
                    })
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers",
                        "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");

        auto res = runner.run(spec);
        QVERIFY(hasCode(res, QStringLiteral("E_REVERSE_LOOKUP_QUERY_FAILED")));
        QCOMPARE(res.writtenRows, 0);
    }

    // 7.11: NULL H-value in DB → treated as miss, governed by exportOnMissing
    void testNullHValue() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
                        // H-col (customer_name) is NULL in DB row
                        QStringLiteral("INSERT INTO orders VALUES ('SO-1',NULL)"),
                    })
                .isEmpty(),
            "DB setup failed");

        // exportOnMissing: "null" → row written, A-col empty
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers",
                        "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]],
                        "exportOnMissing":"null"}],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 1);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);
        int custNoCol = headers.indexOf(QStringLiteral("CustNo"));
        QVERIFY(custNoCol >= 0);
        QVERIFY(rows[0][custNoCol].isEmpty());
    }

    // 7.9: columnOrder with A-col in order → accepted; H-col in columnOrder →
    // E_EXPORT_UNKNOWN_HEADER
    void testColumnOrderWithAHeader() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
                        QStringLiteral("INSERT INTO orders VALUES ('SO-1','Alice')"),
                    })
                .isEmpty(),
            "DB setup failed");

        // A-col (CustNo) in columnOrder → valid
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers",
                        "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
            "columns":{"order_no":{"source":"OrderNo"}},
            "export":{"columnOrder":["CustNo","OrderNo"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);
        QVERIFY(headers.size() >= 2);
        QCOMPARE(headers[0], QStringLiteral("CustNo"));
        QCOMPARE(headers[1], QStringLiteral("OrderNo"));
    }

    // 7.8: Mixed mode — cross-class same identity merges prefetch; per-class resolution
    void testMixedModeRoundtrip() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        // Class A: orders
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        // Class B: invoices
                        QStringLiteral(
                            "CREATE TABLE invoices (inv_no TEXT PRIMARY KEY, customer_name TEXT)"),
                        // Shared G table (same identity key for both classes)
                        QStringLiteral(
                            "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
                        QStringLiteral("INSERT INTO ref_customers VALUES ('C2','Bob')"),
                        QStringLiteral("INSERT INTO orders VALUES ('SO-1','Alice')"),
                        QStringLiteral("INSERT INTO invoices VALUES ('INV-1','Bob')"),
                    })
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"mixed",
            "classColumn":"_class",
            "classes":[
                {
                    "id":"orders",
                    "routes":[{
                        "table":"orders","conflict":{"columns":["order_no"]},
                        "lookups":[{"name":"c","from":"ref_customers",
                                    "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
                        "columns":{"order_no":{"source":"OrderNo"}}
                    }]
                },
                {
                    "id":"invoices",
                    "routes":[{
                        "table":"invoices","conflict":{"columns":["inv_no"]},
                        "lookups":[{"name":"c","from":"ref_customers",
                                    "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
                        "columns":{"inv_no":{"source":"InvNo"}}
                    }]
                }
            ]
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 2);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        // CustNo (A) should appear; customer_name (H) should not
        QVERIFY(headers.contains(QStringLiteral("CustNo")));
        QVERIFY(!headers.contains(QStringLiteral("customer_name")));

        int custNoCol = headers.indexOf(QStringLiteral("CustNo"));
        QSet<QString> custNos;
        for (const auto& row : rows)
            custNos.insert(row[custNoCol]);
        // One row should have C1 (Alice→SO-1) and one C2 (Bob→INV-1)
        QVERIFY(custNos.contains(QStringLiteral("C1")));
        QVERIFY(custNos.contains(QStringLiteral("C2")));
    }

    // 7.10: A-column governed by dateFormat → temporal processing applied to reverse-lookup value
    void testAColumnWithTemporalFormat() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        // G table: period_id (FK code) → event_date (date in dbFormat "yyyy-MM-dd")
        // Main table: order_no + event_date (NULL direct column) + period_id (H-col FK)
        // The event_date column is NULL in DB so D5 falls through to reverse-lookup value.
        // temporal["OrderDate"] is built from the ColumnSpec (source="OrderDate", dateFormat).
        QVERIFY2(
            setupDb(
                runner.dbPath,
                {
                    QStringLiteral(
                        "CREATE TABLE ref_periods (period_id TEXT PRIMARY KEY, event_date TEXT)"),
                    QStringLiteral("INSERT INTO ref_periods VALUES ('P1','2025-03-14')"),
                    QStringLiteral("CREATE TABLE orders (order_no TEXT PRIMARY KEY, event_date "
                                   "TEXT, period_id TEXT)"),
                    QStringLiteral("INSERT INTO orders VALUES ('SO-1', NULL, 'P1')"),
                })
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"p","from":"ref_periods",
                        "match":[["event_date","OrderDate"]],
                        "select":[["period_id","period_id"]]}],
            "columns":{
                "order_no":{"source":"OrderNo"},
                "event_date":{
                    "source":"OrderDate",
                    "dateFormat":{"excelFormat":"yyyy/M/d","dbFormat":"yyyy-MM-dd"}
                }
            },
            "export":{"orderBy":["order_no"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 1);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        // H-col (period_id) must not appear; A-col (OrderDate) must appear
        QVERIFY(!headers.contains(QStringLiteral("period_id")));
        QVERIFY(headers.contains(QStringLiteral("OrderDate")));

        int orderDateCol = headers.indexOf(QStringLiteral("OrderDate"));
        QVERIFY(orderDateCol >= 0);
        // Reverse lookup returns "2025-03-14" (dbFormat); temporal converts to "2025/3/14"
        QCOMPARE(rows[0][orderDateCol], QStringLiteral("2025/3/14"));
    }

    // W2: same route with L1(roundtrip=true) and L2(roundtrip=false) — L1's H replaced, L2's H kept
    void testMixedRoundtripAcrossLookups() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {
                    QStringLiteral("CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer_name "
                                   "TEXT, category_id TEXT)"),
                    QStringLiteral(
                        "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                    QStringLiteral(
                        "CREATE TABLE ref_categories (cat_code TEXT PRIMARY KEY, cat_id TEXT)"),
                    QStringLiteral("INSERT INTO ref_customers VALUES ('C1','Alice')"),
                    QStringLiteral("INSERT INTO ref_categories VALUES ('CAT_A','category_id_1')"),
                    QStringLiteral("INSERT INTO orders VALUES ('SO-1','Alice','category_id_1')"),
                })
                .isEmpty(),
            "DB setup failed");

        // L1 roundtrip=true  → customer_name (H) removed, CustNo (A) added
        // L2 roundtrip=false → category_id (H) kept as-is, Category (A) NOT added
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[
                {"name":"l1","from":"ref_customers",
                 "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]],
                 "exportRoundtrip":true},
                {"name":"l2","from":"ref_categories",
                 "match":[["cat_code","Category"]],"select":[["cat_id","category_id"]],
                 "exportRoundtrip":false}
            ],
            "columns":{"order_no":{"source":"OrderNo"}}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        // L1: A-header appears, H-col absent
        QVERIFY2(headers.contains(QStringLiteral("CustNo")), "L1 A-header CustNo should appear");
        QVERIFY2(!headers.contains(QStringLiteral("customer_name")),
                 "L1 H-col customer_name should be absent");

        // L2: H-col appears, A-header absent
        QVERIFY2(headers.contains(QStringLiteral("category_id")),
                 "L2 H-col category_id should appear");
        QVERIFY2(!headers.contains(QStringLiteral("Category")),
                 "L2 A-header Category should be absent");

        QCOMPARE(res.writtenRows, 1);
        int custNoCol = headers.indexOf(QStringLiteral("CustNo"));
        QCOMPARE(rows[0][custNoCol], QStringLiteral("C1"));  // reverse-lookup result
        int catCol = headers.indexOf(QStringLiteral("category_id"));
        QCOMPARE(rows[0][catCol], QStringLiteral("category_id_1"));  // direct DB value
    }

    // W3: D5 — ColumnSpec.source value wins over reverse-lookup A value when non-NULL
    void testD5SourceWinsOverReverseLookup() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        // orders.client_no → source "ClientNo" (ColumnSpec.source)
        // The same "ClientNo" is ALSO the A-header for the lookup
        // DB row has client_no='DIRECT-99' (non-NULL) → D5: Excel shows 'DIRECT-99', not 'C1'
        QVERIFY2(
            setupDb(
                runner.dbPath,
                {
                    QStringLiteral("CREATE TABLE orders (order_no TEXT PRIMARY KEY, client_no "
                                   "TEXT, customer_name TEXT)"),
                    QStringLiteral(
                        "CREATE TABLE ref_customers (c_no TEXT PRIMARY KEY, c_name TEXT)"),
                    QStringLiteral("INSERT INTO ref_customers VALUES ('C1','DIRECT-99')"),
                    QStringLiteral("INSERT INTO orders VALUES ('SO-1','DIRECT-99','DIRECT-99')"),
                })
                .isEmpty(),
            "DB setup failed");

        // client_no has source="ClientNo" (ColumnSpec win candidate)
        // lookup: match=[["c_no","ClientNo"]], select=[["c_name","customer_name"]]
        // G row: c_name='DIRECT-99' → c_no='C1'
        // DB row: customer_name='DIRECT-99', client_no='DIRECT-99'
        // D5: rowData["ClientNo"] = 'DIRECT-99' (non-NULL) → wins over reverse-lookup c_no='C1'
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers",
                        "match":[["c_no","ClientNo"]],"select":[["c_name","customer_name"]]}],
            "columns":{
                "order_no":{"source":"OrderNo"},
                "client_no":{"source":"ClientNo"}
            }
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        int clientNoCol = headers.indexOf(QStringLiteral("ClientNo"));
        QVERIFY2(clientNoCol >= 0, "ClientNo column should be present");
        // ColumnSpec source value 'DIRECT-99' must win over reverse-lookup result 'C1'
        QCOMPARE(rows[0][clientNoCol], QStringLiteral("DIRECT-99"));
    }

    // 7.9b: H-col in columnOrder (with exportRoundtrip=true) → E_EXPORT_UNKNOWN_HEADER from
    // validator
    void testColumnOrderRejectsHHeader() {
        QString dbPath = newDb();

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

        ProfileSpec spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "lookups":[{"name":"c","from":"ref_customers",
                        "match":[["c_no","CustNo"]],"select":[["c_name","customer_name"]]}],
            "columns":{"order_no":{"source":"OrderNo"}},
            "export":{"columnOrder":["customer_name","OrderNo"]}
        })");

        // Validate against the real DB schema
        QString conn = QStringLiteral("val_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        db.open();
        SchemaCatalog catalog;
        SchemaIntrospector si;
        si.load(db, &catalog, nullptr);
        db.close();
        QSqlDatabase::removeDatabase(conn);

        // columnOrder includes "customer_name" which is an H-col for a roundtrip=true lookup.
        // ProfileValidator should reject it as E_EXPORT_UNKNOWN_HEADER.
        ErrorCollector errors;
        // Known Excel headers that would appear: "OrderNo" (source) + "CustNo" (A-header from
        // lookup). "customer_name" is NOT a valid output header when exportRoundtrip=true.
        QStringList knownExcelHeaders;
        knownExcelHeaders << QStringLiteral("OrderNo") << QStringLiteral("CustNo");
        ProfileValidator v;
        bool valid = v.validate(spec, catalog, knownExcelHeaders, &errors);
        QVERIFY(!valid);
        bool hasUnknownHeader = false;
        for (const auto& e : errors.list())
            if (e.code == QStringLiteral("E_EXPORT_UNKNOWN_HEADER"))
                hasUnknownHeader = true;
        QVERIFY2(hasUnknownHeader, "Expected E_EXPORT_UNKNOWN_HEADER for H-col in columnOrder");
    }
};

QTEST_MAIN(TstReverseLookupExport)
#include "tst_reverse_lookup_export.moc"
