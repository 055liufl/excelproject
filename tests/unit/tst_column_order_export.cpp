// Integration tests for export columnOrder (tasks.md 7.3-7.7).
//
// Test strategy: real SQLite temp-file DB + ExportService.run() + read xlsx back.

#include "dbridge/Errors.h"

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
#include "service/ExportService.h"
#include <xlsxcellrange.h>
#include <xlsxdocument.h>

using namespace dbridge::detail;

// ── Shared helpers ────────────────────────────────────────────────────────────

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

static ProfileSpec parseProfile(const QString& json) {
    ProfileLoader loader;
    ProfileSpec spec;
    QString err;
    if (!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err))
        qFatal("%s failed: %s", "parseProfile", err.toUtf8().constData());
    return spec;
}

static void readXlsxSheet(const QString& path, const QString& sheet, QStringList* headers,
                          QVector<QStringList>* rows) {
    QXlsx::Document doc(path);
    if (!doc.selectSheet(sheet))
        return;
    QXlsx::CellRange dim = doc.dimension();
    int maxRow = dim.lastRow();
    int maxCol = dim.lastColumn();
    for (int c = 1; c <= maxCol; ++c)
        headers->append(doc.read(1, c).toString());
    for (int r = 2; r <= maxRow; ++r) {
        QStringList dataRow;
        for (int c = 1; c <= maxCol; ++c)
            dataRow.append(doc.read(r, c).toString());
        rows->append(dataRow);
    }
}

struct ExportRunner {
    QString dbPath;
    QString xlsxPath;
    SchemaCatalog catalog;

    dbridge::ExportResult run(const ProfileSpec& spec) {
        QString conn = QStringLiteral("run_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        db.open();
        SchemaIntrospector si;
        si.load(db, &catalog, nullptr);
        ExportService svc;
        dbridge::ExportOptions opts;
        auto res = svc.run(spec, catalog, xlsxPath, opts, db);
        db.close();
        QSqlDatabase::removeDatabase(conn);
        return res;
    }
};

// ── Test class ────────────────────────────────────────────────────────────────

class TstColumnOrderExport : public QObject {
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

    // 7.3: SingleTable — columnOrder reorders columns; unlisted columns appended in SQL order
    void testSingleTableColumnOrder() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(setupDb(runner.dbPath,
                         {
                             QStringLiteral("CREATE TABLE orders (order_no TEXT PRIMARY KEY, "
                                            "tenant_id TEXT, total TEXT)"),
                             QStringLiteral("INSERT INTO orders VALUES ('SO-1','T001','100')"),
                             QStringLiteral("INSERT INTO orders VALUES ('SO-2','T002','200')"),
                         })
                     .isEmpty(),
                 "DB setup failed");

        // Natural SQL order: OrderNo, TenantId, Total
        // columnOrder reverses to: Total, OrderNo, TenantId
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "columns":{
                "order_no":  {"source":"OrderNo"},
                "tenant_id": {"source":"TenantId"},
                "total":     {"source":"Total"}
            },
            "export":{"orderBy":["order_no"],"columnOrder":["Total","OrderNo","TenantId"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 2);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        // Columns in declared order
        QCOMPARE(headers[0], QStringLiteral("Total"));
        QCOMPARE(headers[1], QStringLiteral("OrderNo"));
        QCOMPARE(headers[2], QStringLiteral("TenantId"));

        // Data is correct (orderBy=order_no → SO-1 row first)
        int totalCol = headers.indexOf(QStringLiteral("Total"));
        int orderNoCol = headers.indexOf(QStringLiteral("OrderNo"));
        QCOMPARE(rows[0][orderNoCol], QStringLiteral("SO-1"));
        QCOMPARE(rows[0][totalCol], QStringLiteral("100"));
        QCOMPARE(rows[1][orderNoCol], QStringLiteral("SO-2"));
        QCOMPARE(rows[1][totalCol], QStringLiteral("200"));
    }

    // 7.3b: Partial columnOrder — listed columns come first, unlisted appended in natural SQL order
    void testSingleTablePartialColumnOrder() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(setupDb(runner.dbPath,
                         {
                             QStringLiteral("CREATE TABLE items (id INTEGER PRIMARY KEY, name "
                                            "TEXT, qty INTEGER, price TEXT)"),
                             QStringLiteral("INSERT INTO items VALUES (1,'Widget',5,'9.99')"),
                         })
                     .isEmpty(),
                 "DB setup failed");

        // Natural order: Id, Name, Qty, Price
        // columnOrder lists only: Price, Name
        // Expected headers: Price, Name, [Id, Qty in SQL order]
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"items","conflict":{"columns":["id"]},
            "columns":{
                "id":    {"source":"Id"},
                "name":  {"source":"Name"},
                "qty":   {"source":"Qty"},
                "price": {"source":"Price"}
            },
            "export":{"columnOrder":["Price","Name"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        // Listed headers come first in declared order
        QCOMPARE(headers[0], QStringLiteral("Price"));
        QCOMPARE(headers[1], QStringLiteral("Name"));
        // Unlisted headers appended in natural SQL order (Id, Qty)
        QCOMPARE(headers[2], QStringLiteral("Id"));
        QCOMPARE(headers[3], QStringLiteral("Qty"));
    }

    // 7.4: MultiTable — columnOrder spanning multiple route columns
    void testMultiTableColumnOrder() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE orders (order_no TEXT PRIMARY KEY, customer TEXT)"),
                        QStringLiteral("CREATE TABLE order_items (order_no TEXT, line_no INTEGER, "
                                       "product TEXT, PRIMARY KEY(order_no,line_no))"),
                        QStringLiteral("INSERT INTO orders VALUES ('SO-1','Alice')"),
                        QStringLiteral("INSERT INTO order_items VALUES ('SO-1',1,'Widget')"),
                    })
                .isEmpty(),
            "DB setup failed");

        // MultiTable: natural headers come from SQL JOIN: OrderNo, Customer, LineNo, Product
        // columnOrder: Product, OrderNo, LineNo, Customer
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"multiTable",
            "routes":[
                {
                    "table":"orders","conflict":{"columns":["order_no"]},
                    "columns":{
                        "order_no": {"source":"OrderNo"},
                        "customer": {"source":"Customer"}
                    }
                },
                {
                    "table":"order_items","parent":"orders",
                    "conflict":{"columns":["order_no","line_no"]},
                    "fkInject":[{"from":"orders","pairs":[["order_no","order_no"]]}],
                    "columns":{
                        "line_no": {"source":"LineNo"},
                        "product": {"source":"Product"}
                    }
                }
            ],
            "export":{"orderBy":["order_no","line_no"],"columnOrder":["Product","OrderNo","LineNo","Customer"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        QCOMPARE(headers[0], QStringLiteral("Product"));
        QCOMPARE(headers[1], QStringLiteral("OrderNo"));
        QCOMPARE(headers[2], QStringLiteral("LineNo"));
        QCOMPARE(headers[3], QStringLiteral("Customer"));

        // Data is correct
        int productCol = headers.indexOf(QStringLiteral("Product"));
        int customerCol = headers.indexOf(QStringLiteral("Customer"));
        QCOMPARE(rows[0][productCol], QStringLiteral("Widget"));
        QCOMPARE(rows[0][customerCol], QStringLiteral("Alice"));
    }

    // 7.5a: Mixed — classColumn NOT in columnOrder → classColumn prepended as first column
    void testMixedClassColumnDefault() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {
                    QStringLiteral("CREATE TABLE orders (order_no TEXT PRIMARY KEY, amount TEXT)"),
                    QStringLiteral("CREATE TABLE invoices (inv_no TEXT PRIMARY KEY, amount TEXT)"),
                    QStringLiteral("INSERT INTO orders VALUES ('SO-1','100')"),
                    QStringLiteral("INSERT INTO invoices VALUES ('INV-1','200')"),
                })
                .isEmpty(),
            "DB setup failed");

        // columnOrder = ["Amount", "OrderNo"] — classColumn "_class" NOT in columnOrder
        // Expected: _class at position 0 (prepended), then Amount, then OrderNo
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"mixed",
            "classColumn":"_class",
            "classes":[
                {
                    "id":"orders",
                    "routes":[{"table":"orders","conflict":{"columns":["order_no"]},
                               "columns":{"order_no":{"source":"OrderNo"},"amount":{"source":"Amount"}}}]
                },
                {
                    "id":"invoices",
                    "routes":[{"table":"invoices","conflict":{"columns":["inv_no"]},
                               "columns":{"inv_no":{"source":"InvNo"},"amount":{"source":"Amount"}}}]
                }
            ],
            "export":{"columnOrder":["Amount","OrderNo"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        // classColumn prepended as first column
        QCOMPARE(headers[0], QStringLiteral("_class"));
        // columnOrder columns follow
        QCOMPARE(headers[1], QStringLiteral("Amount"));
        QCOMPARE(headers[2], QStringLiteral("OrderNo"));
    }

    // 7.5b: Mixed — classColumn IN columnOrder → placed at declared position
    void testMixedClassColumnInColumnOrder() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {
                    QStringLiteral("CREATE TABLE orders (order_no TEXT PRIMARY KEY, amount TEXT)"),
                    QStringLiteral("CREATE TABLE invoices (inv_no TEXT PRIMARY KEY, amount TEXT)"),
                    QStringLiteral("INSERT INTO orders VALUES ('SO-1','100')"),
                    QStringLiteral("INSERT INTO invoices VALUES ('INV-1','200')"),
                })
                .isEmpty(),
            "DB setup failed");

        // classColumn "_class" is placed in the middle by columnOrder
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"mixed",
            "classColumn":"_class",
            "classes":[
                {
                    "id":"orders",
                    "routes":[{"table":"orders","conflict":{"columns":["order_no"]},
                               "columns":{"order_no":{"source":"OrderNo"},"amount":{"source":"Amount"}}}]
                },
                {
                    "id":"invoices",
                    "routes":[{"table":"invoices","conflict":{"columns":["inv_no"]},
                               "columns":{"inv_no":{"source":"InvNo"},"amount":{"source":"Amount"}}}]
                }
            ],
            "export":{"columnOrder":["Amount","_class","OrderNo"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        // classColumn is at declared position (index 1), not prepended
        QCOMPARE(headers[0], QStringLiteral("Amount"));
        QCOMPARE(headers[1], QStringLiteral("_class"));
        QCOMPARE(headers[2], QStringLiteral("OrderNo"));
    }

    // 7.5c: Mixed — column absent in one class writes empty cell for that class's rows
    void testMixedCrossClassEmptyCell() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(setupDb(runner.dbPath,
                         {
                             QStringLiteral(
                                 "CREATE TABLE orders (order_no TEXT PRIMARY KEY, ship_date TEXT)"),
                             QStringLiteral(
                                 "CREATE TABLE invoices (inv_no TEXT PRIMARY KEY, due_date TEXT)"),
                             QStringLiteral("INSERT INTO orders VALUES ('SO-1','2025-03-01')"),
                             QStringLiteral("INSERT INTO invoices VALUES ('INV-1','2025-03-15')"),
                         })
                     .isEmpty(),
                 "DB setup failed");

        // orders has ShipDate; invoices has DueDate — neither class shares these columns.
        // columnOrder: [ShipDate, DueDate, ...] — each class row should have empty in the other's
        // column.
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"mixed",
            "classColumn":"_class",
            "classes":[
                {
                    "id":"orders",
                    "routes":[{"table":"orders","conflict":{"columns":["order_no"]},
                               "columns":{"order_no":{"source":"OrderNo"},"ship_date":{"source":"ShipDate"}}}]
                },
                {
                    "id":"invoices",
                    "routes":[{"table":"invoices","conflict":{"columns":["inv_no"]},
                               "columns":{"inv_no":{"source":"InvNo"},"due_date":{"source":"DueDate"}}}]
                }
            ],
            "export":{"columnOrder":["ShipDate","DueDate","OrderNo","InvNo"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 2);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        // _class prepended (not in columnOrder)
        QCOMPARE(headers[0], QStringLiteral("_class"));
        QCOMPARE(headers[1], QStringLiteral("ShipDate"));
        QCOMPARE(headers[2], QStringLiteral("DueDate"));

        int classCol = headers.indexOf(QStringLiteral("_class"));
        int shipCol = headers.indexOf(QStringLiteral("ShipDate"));
        int dueCol = headers.indexOf(QStringLiteral("DueDate"));
        int orderNoCol = headers.indexOf(QStringLiteral("OrderNo"));
        int invNoCol = headers.indexOf(QStringLiteral("InvNo"));

        // Find each row by class
        QString ordersShip, ordersDue, invoicesShip, invoicesDue;
        for (const auto& row : rows) {
            if (row[classCol] == QStringLiteral("orders")) {
                ordersShip = row[shipCol];
                ordersDue = row[dueCol];
                QVERIFY(row[orderNoCol] == QStringLiteral("SO-1"));
                QVERIFY(row[invNoCol].isEmpty());  // InvNo empty for orders row
            } else if (row[classCol] == QStringLiteral("invoices")) {
                invoicesShip = row[shipCol];
                invoicesDue = row[dueCol];
                QVERIFY(row[invNoCol] == QStringLiteral("INV-1"));
                QVERIFY(row[orderNoCol].isEmpty());  // OrderNo empty for invoices row
            }
        }
        QCOMPARE(ordersShip, QStringLiteral("2025-03-01"));
        QVERIFY(ordersDue.isEmpty());     // orders row has no DueDate
        QVERIFY(invoicesShip.isEmpty());  // invoices row has no ShipDate
        QCOMPARE(invoicesDue, QStringLiteral("2025-03-15"));
    }

    // 7.6: orderBy and columnOrder are orthogonal — both can be declared simultaneously
    void testOrderByAndColumnOrderCoexist() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {
                    QStringLiteral(
                        "CREATE TABLE products (id INTEGER PRIMARY KEY, name TEXT, price TEXT)"),
                    QStringLiteral("INSERT INTO products VALUES (3,'C','30')"),
                    QStringLiteral("INSERT INTO products VALUES (1,'A','10')"),
                    QStringLiteral("INSERT INTO products VALUES (2,'B','20')"),
                })
                .isEmpty(),
            "DB setup failed");

        // orderBy=[id] → rows sorted 1,2,3
        // columnOrder=[Price, Name, Id] → columns reordered
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"products","conflict":{"columns":["id"]},
            "columns":{
                "id":    {"source":"Id"},
                "name":  {"source":"Name"},
                "price": {"source":"Price"}
            },
            "export":{"orderBy":["id"],"columnOrder":["Price","Name","Id"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 3);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        // Columns in declared order
        QCOMPARE(headers[0], QStringLiteral("Price"));
        QCOMPARE(headers[1], QStringLiteral("Name"));
        QCOMPARE(headers[2], QStringLiteral("Id"));

        // Rows in orderBy order (id ascending: 1,2,3)
        int idCol = headers.indexOf(QStringLiteral("Id"));
        QCOMPARE(rows[0][idCol], QStringLiteral("1"));
        QCOMPARE(rows[1][idCol], QStringLiteral("2"));
        QCOMPARE(rows[2][idCol], QStringLiteral("3"));
    }

    // 7.7: No columnOrder → streaming path, behavior unchanged
    void testNoColumnOrderStreaming() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(setupDb(runner.dbPath,
                         {
                             QStringLiteral("CREATE TABLE orders (order_no TEXT PRIMARY KEY, "
                                            "customer TEXT, total TEXT)"),
                             QStringLiteral("INSERT INTO orders VALUES ('SO-1','Alice','100')"),
                             QStringLiteral("INSERT INTO orders VALUES ('SO-2','Bob','200')"),
                             QStringLiteral("INSERT INTO orders VALUES ('SO-3','Carol','300')"),
                         })
                     .isEmpty(),
                 "DB setup failed");

        // No columnOrder — SQL natural order should be used
        auto spec = parseProfile(R"({
            "profileName":"test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"orders","conflict":{"columns":["order_no"]},
            "columns":{
                "order_no": {"source":"OrderNo"},
                "customer": {"source":"Customer"},
                "total":    {"source":"Total"}
            },
            "export":{"orderBy":["order_no"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 3);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        // Headers in SQL natural order
        QVERIFY(headers.contains(QStringLiteral("OrderNo")));
        QVERIFY(headers.contains(QStringLiteral("Customer")));
        QVERIFY(headers.contains(QStringLiteral("Total")));

        // Rows present and correct (orderBy → sorted)
        int orderNoCol = headers.indexOf(QStringLiteral("OrderNo"));
        QCOMPARE(rows[0][orderNoCol], QStringLiteral("SO-1"));
        QCOMPARE(rows[1][orderNoCol], QStringLiteral("SO-2"));
        QCOMPARE(rows[2][orderNoCol], QStringLiteral("SO-3"));
    }
};

QTEST_MAIN(TstColumnOrderExport)
#include "tst_column_order_export.moc"
