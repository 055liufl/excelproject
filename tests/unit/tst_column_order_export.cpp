// Integration tests for export columnOrder (tasks.md 7.3-7.7).
//
// Test strategy: real SQLite temp-file DB + ExportService.run() + read xlsx back.
//
// ============================================================================
// tst_column_order_export.cpp — 导出列序（export.columnOrder）的集成测试
// ============================================================================
//
// 【columnOrder 是什么 / 验证什么】
//   导出时，列在 Excel 里的「默认顺序」是 SQL 自然顺序（建表/JOIN 出来的列序）。export.columnOrder
//   让用户显式指定「希望列以什么顺序出现」。本文件钉住它在四种导出模式下的排序契约：
//     · 7.3  SingleTable：完整 columnOrder → 列按声明顺序重排；
//     · 7.3b 部分 columnOrder：列出的列排前面（按声明序），未列出的追加在后（按 SQL 自然序）；
//     · 7.4  MultiTable：columnOrder 跨多张路由表的列也能统一重排；
//     · 7.5a Mixed 且 classColumn 不在 columnOrder → classColumn 自动「前置」为第 1 列；
//     · 7.5b Mixed 且 classColumn 在 columnOrder → 放在声明的那个位置（不前置）；
//     · 7.5c Mixed 跨类列缺失 → 某 class 没有的列，在该 class 的行里写空单元格；
//     · 7.6  orderBy 与 columnOrder 正交：行排序与列排序互不影响，可同时声明；
//     · 7.7  不给 columnOrder → 走流式导出路径，行为不变（列为 SQL 自然序）。
//
// 【测试手法：真库 + 真导出 + 回读 xlsx 校验】
//   每个用例都：建磁盘临时库并插入数据 → 用 ExportService.run() 真正导出成 .xlsx →
//   再用 QXlsx 把生成的文件「读回来」，对照 headers（列序）与 rows（行序/单元格值）断言。
//   这是「黑盒、端到端」的集成测试——直接验证用户最终拿到的 Excel 长什么样，而非内部中间态。
//
// 【夹具】initTestCase 建一次临时目录 tmp_；newDb()/newXlsx() 各用例生成唯一文件名。
//   ExportRunner 把「开库→反射 catalog→run→关库」的样板收成一个可复用的小结构体。
// ============================================================================

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
// ── 公共测试辅助 ──────────────────────────────────────────────────────────────

// makeUuid —— 生成去花括号/连字符的 UUID 片段，用作唯一连接名/文件名。
static QString makeUuid() {
    return QUuid::createUuid().toString().remove('{').remove('}').remove('-');
}

// setupDb —— 建库并按序执行一组 SQL（建表 + 插数据）。返回空串=成功，非空=出错的那条 SQL+原因。
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

// parseProfile —— 解析 profile JSON；失败即 qFatal（profile 是测试输入前提，写错应立即崩）。
static ProfileSpec parseProfile(const QString& json) {
    ProfileLoader loader;
    ProfileSpec spec;
    QString err;
    if (!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err))
        qFatal("%s failed: %s", "parseProfile", err.toUtf8().constData());
    return spec;
}

// readXlsxSheet —— 把导出的 .xlsx 回读成「表头列表 + 行数据列表」，供断言列序/行值。
//   做什么：用 QXlsx 打开文件、选中 sheet，按其有效区域（dimension）逐格读出。
//     · 第 1 行视为表头，逐列读进 *headers；
//     · 第 2 行起视为数据，每行读成一个 QStringList 追加进 *rows。
//   细节：QXlsx 行列从 1 起；read() 一律 toString()，故断言里比较的都是字符串形式
//     （数字 100 在 Excel 里回读为 "100"）。selectSheet 失败（无此表）则直接返回空结果。
static void readXlsxSheet(const QString& path, const QString& sheet, QStringList* headers,
                          QVector<QStringList>* rows) {
    QXlsx::Document doc(path);
    if (!doc.selectSheet(sheet))
        return;
    QXlsx::CellRange dim = doc.dimension();  // 有效数据区域（首末行列）
    int maxRow = dim.lastRow();
    int maxCol = dim.lastColumn();
    for (int c = 1; c <= maxCol; ++c)
        headers->append(doc.read(1, c).toString());  // 第 1 行 = 表头
    for (int r = 2; r <= maxRow; ++r) {              // 第 2 行起 = 数据
        QStringList dataRow;
        for (int c = 1; c <= maxCol; ++c)
            dataRow.append(doc.read(r, c).toString());
        rows->append(dataRow);
    }
}

// ExportRunner —— 把「开库 → 反射 catalog → ExportService.run → 关库」的样板收进一个结构体。
//   字段 dbPath/xlsxPath 由各用例先填好（指向 newDb()/newXlsx() 的唯一路径），catalog 由 run
//   内部填充。run(spec) 执行一次真实导出并返回 ExportResult，让用例只关注「给什么 spec、得到
//   什么文件」。注意此处用默认 ExportOptions（不指定 sheet 等）。
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
        si.load(db, &catalog, nullptr);  // 反射库结构（导出需要列信息）
        ExportService svc;
        dbridge::ExportOptions opts;
        auto res = svc.run(spec, catalog, xlsxPath, opts, db);  // 真正导出到 xlsxPath
        db.close();
        QSqlDatabase::removeDatabase(conn);
        return res;
    }
};

// ── Test class ────────────────────────────────────────────────────────────────

class TstColumnOrderExport : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;  // 全类共用临时目录（析构自动清理）

    QString newDb() const {
        return tmp_.path() + '/' + makeUuid() + ".db";
    }
    QString newXlsx() const {
        return tmp_.path() + '/' + makeUuid() + ".xlsx";
    }

   private slots:
    // 整类只跑一次：确认临时目录可用。
    void initTestCase() {
        QVERIFY(tmp_.isValid());
    }

    // 7.3: SingleTable — columnOrder reorders columns; unlisted columns appended in SQL order
    // GIVEN orders 表自然列序为 OrderNo,TenantId,Total，profile 给出完整 columnOrder
    //   ["Total","OrderNo","TenantId"]（即把列序反转），orderBy=order_no。
    // WHEN  导出并回读 xlsx，
    // THEN  ① 表头恰为 Total/OrderNo/TenantId（按声明序重排）；② 写出 2 行；
    //       ③ 行按 orderBy 排（SO-1 在前）、且各单元格值正确。
    //   验证：列序服从 columnOrder、行序服从 orderBy、数据不串位（用列名定位列下标后再取值）。
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
    // GIVEN items 自然列序 Id,Name,Qty,Price；columnOrder 只列了 ["Price","Name"]（部分）。
    // THEN 表头应为：Price,Name（列出的，按声明序在前），随后 Id,Qty（未列出的，按 SQL
    // 自然序追加）。
    //   验证「部分指定」的补齐规则：显式列在前、隐式列在后且保持自然序。
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
    // GIVEN multiTable：orders(父) + order_items(子) 经 JOIN 导出，自然列序
    //   OrderNo,Customer,LineNo,Product；columnOrder 跨两表重排为 Product,OrderNo,LineNo,Customer。
    // THEN 表头按声明序排列（跨表列也被统一重排），且行数据正确（Product=Widget,Customer=Alice）。
    //   验证 columnOrder 作用域不限单表，能横跨多路由的列。
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
    // GIVEN mixed 模式（orders 与 invoices 两 class，判别列 classColumn="_class"），
    //   columnOrder=["Amount","OrderNo"] 中「没有」_class。
    // THEN 由于 _class 未被显式排，系统把它「前置」成第 0 列；其后才是 Amount,OrderNo。
    //   验证 mixed 模式下「判别列默认置顶」的约定（让读者一眼看出每行属于哪个 class）。
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
    // 与 7.5a 对照：当 columnOrder 显式包含 _class（["Amount","_class","OrderNo"]）时，
    // _class 应放在声明的「中间」位置（index 1），而「不」被强制前置。
    //   验证「显式声明 > 默认前置」：用户一旦显式安排判别列位置，默认置顶规则让位。
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
    // GIVEN mixed：orders 类有 ShipDate、invoices 类有 DueDate，两类「不共享」这两列；
    //   columnOrder 同时排了 ShipDate,DueDate,OrderNo,InvNo（合并了两类各自的列）。
    // THEN 每一行只在「本类拥有的列」有值，「本类没有的列」写空单元格：
    //   orders 行的 DueDate/InvNo 为空、invoices 行的 ShipDate/OrderNo 为空，且各自专属列有值。
    //   验证「跨类合并导出」时缺失列以空格填充（而非串位/报错），并用 classCol 区分行归属逐行核对。
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
    // GIVEN products 乱序插入 (3,1,2)，orderBy=[id]（行排序）+
    // columnOrder=[Price,Name,Id]（列排序）。 THEN 列按 columnOrder 排（Price,Name,Id），行按
    // orderBy 升序排（id=1,2,3）。
    //   验证「行排序」与「列排序」是两个正交维度、可同时声明、互不干扰。
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
    // GIVEN 不声明 columnOrder（只给 orderBy）。
    // THEN 导出走「流式路径」（无需先把所有列缓存重排，可边查边写），列序为 SQL 自然序、
    //   行按 orderBy 排。验证「不指定列序」时回退到原有流式行为、结果不变（向后兼容/无回归）。
    //   注意这里用 contains 而非按下标精确比对表头——因为自然序由 SQL 决定，只需确认列都在即可。
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

// QTEST_MAIN：生成跑本测试类全部用例的 main()；末尾并入 moc 生成的元对象代码（固定写法）。
QTEST_MAIN(TstColumnOrderExport)
#include "tst_column_order_export.moc"
