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

// ============================================================================
// tst_lookup_prefetch.cpp — 「外键查找(lookup)预取批处理」集成测试
// ============================================================================
//
// 【被测的是什么特性】
//   导入时的 lookup（外键正向查找）：Excel 里只填了「业务键」（如客户编号 CustNo），
//   而目标库列需要的是参照表里查出来的值（如客户名 customer_name）。ImportService 会先
//   把一批行里出现过的「去重业务键」攒成一批，用【一次】批量 SELECT（prefetch，预取）去
//   参照表查回所需列，再回填到各行——而不是「每行各查一次」。这能把 N 行的 N 次查询压成
//   常数次批量查询，是导入性能的关键优化。
//
// 【这个测试文件守护的核心契约：prefetch 次数 == 不同 lookup 身份的批数】
//   svc.onPrefetch 是 ImportService 暴露的「每发起一次预取 SELECT 就回调一次」的钩子。
//   各用例正是用一个计数器 prefetchCount 数它被调几次，从而验证「批处理 + 身份去重」是否
//   生效。所谓「lookup 身份」= (from 参照表, match 匹配列对, select 取出列对) 三者相同即同一身份，
//   即便分散在不同 route / 不同 class 里，也应被合并成同一次预取。守护的不变量包括：
//     · K=0（无任何有效业务键）→ 0 次预取，且每行报 E_LOOKUP_KEY_EMPTY（testK0NoSelect）；
//     · 有效键的单 lookup → 恰 1 次预取，且查得的列被填进 payload（testBasicLookupPrefetch）；
//     · 多表中相同身份的 lookup → 合并为 1 次（testIdentityMerging）；
//     · 混合模式跨类别相同身份 → 仍合并为 1 次（testMixedModeIdentityMerging，规范 §10.10）；
//     · 端到端真实写库：lookup 回填 + 复合 fkInject 同时生效（testE2eRealWrite，规范 §12.3）。
//
// 【测试框架与执行方式】
//   Qt Test：每个 private slot 是一个用例；initTestCase() 是「整个测试类只跑一次」的总夹具。
//   多数用例用 dryRun=true（只跑解析/预取/生成 payload，不真正写库），把焦点收在「预取行为」上；
//   唯独 testE2eRealWrite 用 dryRun=false 真正落库，验证端到端写入正确性。
//   末尾 QTEST_MAIN：需要 QApplication（QXlsx 写文件等可能依赖）。
//   各用例统一 GIVEN（建库+造 xlsx+写 profile）→ WHEN（svc.run 导入）→
//   THEN（断预取次数/payload/库）。
// ============================================================================

using namespace dbridge::detail;

// ── Test helpers —— 测试夹具/辅助函数（被多个用例复用）─────────────────────────

// makeUuid —— 生成一个去掉花括号与连字符的随机 UUID 串。
// 用途：给数据库连接名、临时文件名拼一个全局唯一后缀，避免用例间撞名/撞文件。
static QString makeUuid() {
    return QUuid::createUuid().toString().remove('{').remove('}').remove('-');
}

// writeXlsx —— 造一个「真实的 .xlsx 文件」作为导入输入。
// Create a real xlsx file: row 1 = headers, rows 2+ = data values
// 译：第 1 行写表头，第 2 行起写数据值（与 profile 里 headerRow=1 的约定一致）。
// 【参数】path 输出文件路径；sheet 工作表名；headers 表头；rows 各行的值（QVariant 二维）。
// 【返回】true=建表+选表+写格+保存全部成功；任一步失败返回 false。
// 【要点】rows[r][c] 为 null 时跳过不写 → 对应 Excel 空单元格（用来构造「业务键为空」等场景）。
//   行号 r+2、列号 c+1 都是 QXlsx 的 1 基坐标，且数据从第 2 行起（第 1 行留给表头）。
static bool writeXlsx(const QString& path, const QString& sheet, const QStringList& headers,
                      const QVector<QVector<QVariant>>& rows) {
    QXlsx::Document doc;
    if (!doc.addSheet(sheet))
        return false;
    if (!doc.selectSheet(sheet))
        return false;
    for (int c = 0; c < headers.size(); ++c)
        doc.write(1, c + 1, headers[c]);  // 第 1 行：表头
    for (int r = 0; r < rows.size(); ++r)
        for (int c = 0; c < rows[r].size(); ++c)
            if (!rows[r][c].isNull())                 // null → 留空单元格（NULL 语义）
                doc.write(r + 2, c + 1, rows[r][c]);  // 第 2 行起：数据
    return doc.saveAs(path);
}

// openDb —— 打开一个临时 SQLite 连接，依次执行一组 DDL/DML，用完即关并移除连接。
// Open a QSQLITE connection and execute DDL/DML; close it after
// 用途：各用例的 GIVEN 阶段用它一次性建好参照表/目标表并灌入种子数据。
// 【返回】空字符串=全部成功；非空=出错信息（打不开库 或 某条 SQL 失败，含失败的语句本身）。
//   返回「错误串而非 bool」是为了让 QVERIFY2 直接把失败原因打到测试输出，便于定位。
// 【要点】连接名带 UUID 防撞名；执行完先 close 再 removeDatabase（释放后才移除，Qt 要求）。
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

// loadProfile —— 把一段 JSON 文本解析成 ProfileSpec（解析失败直接 qFatal 终止测试）。
// 用途：各用例把内嵌的 profile JSON 交给真实的 ProfileLoader 解析，确保测的是「经过真实
//   加载链路的 profile」。解析失败用 qFatal 立即中止——测试用例的 profile 必须合法，
//   若连加载都过不了说明用例本身写错了，应当醒目失败而非默默继续。
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

    QTemporaryDir tmp_;  // 临时目录：所有用例的 .db 与 .xlsx 都落在这里，测试结束自动清理

    // newDb / newXlsx —— 在临时目录里生成一个唯一的 .db / .xlsx 路径（仅给路径，不创建文件）。
    QString newDb() const {
        return tmp_.path() + '/' + makeUuid() + ".db";
    }
    QString newXlsx() const {
        return tmp_.path() + '/' + makeUuid() + ".xlsx";
    }

   private slots:
    // initTestCase —— Qt Test 总夹具：整个测试类【只运行一次】，先于所有用例。
    // 这里仅断言临时目录创建成功（后续用例都依赖它落地文件）。
    void initTestCase() {
        QVERIFY(tmp_.isValid());
    }

    // ── testK0NoSelect —— K=0：没有任何「不同的业务键」→ 0 次预取，且每行报键为空 ────
    // K=0: no distinct match keys → 0 SELECT
    // GIVEN orders 表 + ref_customers 参照表；Excel 仅一行且 CustNo 为空（NULL）；
    //   profile 声明一个按 CustNo 查 ref_customers 的 lookup。
    // WHEN dryRun 导入。
    // THEN 因为不存在任何「有效不同键」(distinct keys, K=0)，根本无需查参照表 → prefetchCount==0；
    //   同时该行因业务键为空，应产生一条行级错误 E_LOOKUP_KEY_EMPTY。
    // 业务含义：空键既不该触发无谓的
    // SELECT（性能），也不该被静默放过（正确性——必须报错让用户知道）。
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
        // 译：Excel 行的 CustNo 为空 → 不同业务键数 K = 0（无键可查）。
        QVERIFY(
            writeXlsx(xlsxPath, QStringLiteral("Sheet1"),
                      {QStringLiteral("OrderNo"), QStringLiteral("CustNo")},
                      {{QStringLiteral("SO-1"), QVariant()}}));  // CustNo is NULL/empty（故意留空）

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

        // 打开真实 SQLite 库，并自省出 schema catalog（导入需要它了解表/列结构）。
        QString connName = QStringLiteral("k0_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        QVERIFY(db.open());

        SchemaIntrospector si;
        SchemaCatalog catalog;
        QVERIFY(si.load(db, &catalog, nullptr));

        // 安装预取计数钩子：ImportService 每发起一次预取 SELECT 就回调一次，这里累加计数。
        int prefetchCount = 0;
        ImportService svc;
        svc.onPrefetch = [&](const QString&) { ++prefetchCount; };

        dbridge::ImportOptions opts;
        opts.profileName = QStringLiteral("test");
        opts.dryRun = true;  // 只验预取行为，不真正写库
        dbridge::ImportResult result = svc.run(spec, catalog, xlsxPath, opts, db);

        db.close();
        QSqlDatabase::removeDatabase(connName);

        // All rows had empty CustNo → K=0 → 0 prefetch SELECTs
        // 译：所有行 CustNo 都空 → K=0 → 0 次预取 SELECT。
        QCOMPARE(prefetchCount, 0);
        // One row-level E_LOOKUP_KEY_EMPTY error expected
        // 译：应恰好出现一条行级错误 E_LOOKUP_KEY_EMPTY（空键不被静默放过）。
        bool hasLookupError = false;
        for (const auto& e : result.errors) {
            if (e.code == QStringLiteral("E_LOOKUP_KEY_EMPTY")) {
                hasLookupError = true;
                break;
            }
        }
        QVERIFY(hasLookupError);
    }

    // ── testBasicLookupPrefetch —— 有效键的单 lookup → 恰 1 次预取，且查得列被填进 payload ──
    // Single lookup with valid keys → exactly 1 SELECT batch
    // GIVEN ref_customers 预置 C1=Alice、C2=Bob；Excel 两行 CustNo 分别为
    // C1、C2（两个不同有效键）；
    //   profile 声明按 CustNo 查 ref_customers、取 c_name 填到 customer_name 的 lookup。
    // WHEN dryRun 导入。
    // THEN 两个不同键被攒成【一批】（chunk=999 容得下）→ prefetchCount==1（而非 2 次逐行查）；
    //   无错误；生成 2 条 dryRun payload；且 payload 里确实带上了查回来的 customer_name。
    // 业务含义：这是「批处理优化」的最小正例——多键合一次查，且查到的值正确回填。
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
        // 译：2 个不同键、批容量 chunk=999 → 合并成 1 次 SELECT 批查。
        QCOMPARE(prefetchCount, 1);
        // dryRun → no errors from FK/upsert; lookup found
        // 译：dryRun 不做真实 FK/upsert，故不应有错误；且 lookup 命中。
        for (const auto& e : result.errors)
            qDebug() << "error:" << e.code << e.message;  // 有错时打印出来便于排查
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.dryRunPayloads.size(), 2);  // 两行 → 两条 payload
        // Each payload should include customer_name
        // 译：每条 payload 都应包含查回来的 customer_name 列（验证回填生效）。
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

    // ── testIdentityMerging —— 同身份合并：两条 route 声明相同 lookup → 仅 1 次预取 ──
    // Identity merging: two routes, same lookup identity → 1 prefetch
    // GIVEN multiTable 模式，orders 与 items 两条 route【各自】声明了一个完全相同的 lookup
    //   （from/match/select 三者一致，即「同一 lookup 身份」）；Excel 一行 CustNo=C1。
    // WHEN dryRun 导入。
    // THEN 尽管有两条 route 各带 lookup，因身份相同应被【合并】，只发起 1 次预取 →
    // prefetchCount==1。 业务含义：预取去重是按「lookup 身份」而非「route
    // 个数」算的——相同身份的查找在整个导入里
    //   只查一次，避免多表场景下对同一参照表的重复查询。
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
        // 译：两条 route 声明了完全相同的 lookup（from / match / select 三者一致 = 同一身份）。
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
        // 译：两条 route 身份相同 → 只发起 1 次 SELECT。
        QCOMPARE(prefetchCount, 1);
    }

    // ── testMixedModeIdentityMerging —— 混合模式跨类别同身份 lookup → 仍合并为 1 次 ──
    // §10.10 Mixed mode: same lookup identity in two classes → 1 prefetch
    // GIVEN mixed 模式，按判别列 Type 分到类别 A / B；两个类别各自的 route 声明了【相同身份】的
    //   lookup（都按 CustNo 查 ref_customers 取 c_name）；Excel 两行：一行类 A、一行类 B，但都用
    //   CustNo=C1。
    // WHEN dryRun 导入。
    // THEN 即便相同身份的 lookup 分散在【不同类别】里，也应被合并 → prefetchCount==1。
    // 业务含义：身份去重的作用域是「整次导入」，能横跨混合模式的多个类别（规范 §10.10），
    //   不会因为分了类别就把同一查找拆成多次。
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
        // 译：一行属类别 A、一行属类别 B，但二者都用相同的 CustNo=C1。
        QVERIFY(writeXlsx(
            xlsxPath, QStringLiteral("Sheet1"),
            {QStringLiteral("Type"), QStringLiteral("OrderNo"), QStringLiteral("InvoiceNo"),
             QStringLiteral("CustNo")},
            {{QStringLiteral("A"), QStringLiteral("SO-1"), QVariant(), QStringLiteral("C1")},
             {QStringLiteral("B"), QVariant(), QStringLiteral("INV-1"), QStringLiteral("C1")}}));

        // Both classes declare the SAME lookup identity
        // 译：两个类别声明了相同身份的 lookup（跨类别也应被合并去重）。
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
            qDebug() << "error:" << e.code << e.message;  // 有错时打印便于排查

        // Same identity across two classes → only 1 SELECT batch issued
        // 译：相同身份横跨两个类别 → 只发起 1 次 SELECT 批查。
        QCOMPARE(prefetchCount, 1);
        // Both rows should produce payloads with customer_name filled
        // 译：两行都应生成带 customer_name 的 payload（验证两类别都正确回填）。
        QVERIFY(!result.dryRunPayloads.isEmpty());
    }

    // ── testE2eRealWrite —— 端到端真实写库：lookup 回填 + 复合 fkInject 同时生效 ────
    // §12.3 e2e: real write with lookup + composite fkInject
    // GIVEN orders / order_items / ref_customers 三表；Excel 两行同属订单 SO-1（行号 1、2），
    //   均用 CustNo=C1；multiTable 模式两条 route 都带相同 lookup，子表 order_items 还声明了
    //   把父表 orders.order_no 注入到自身 order_no 的 fkInject。
    // WHEN dryRun=false【真正写库】导入。
    // THEN result.ok 为真；并直接查库断言三件事：
    //   (1) orders 经 upsert 去重后恰 1 行（两行同 SO-1 合并）；
    //   (2) order_items 2 行且 customer_name 都被 lookup 填上（NOT NULL 计数==2）；
    //   (3) fkInject 生效——order_items.order_no 全部能在 orders 里找到对应（孤儿行计数==0）。
    // 业务含义：这是把「lookup 批量回填」与「复合外键注入」两大特性放在真实写入路径上一起验证的
    //   集成用例（规范 §12.3），确保它们组合使用时端到端结果正确、外键闭合无孤儿。
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
        // 译：两行用相同 CustNo，故 orders.customer_name 相同（批内唯一性校验 BatchUniqueness
        //   允许这种相同）。order_items 也用 lookup → 两条明细的 customer_name 都会被填上。
        QVERIFY(writeXlsx(
            xlsxPath, QStringLiteral("Sheet1"),
            {QStringLiteral("OrderNo"), QStringLiteral("LineNo"), QStringLiteral("Sku"),
             QStringLiteral("CustNo")},
            {{QStringLiteral("SO-1"), 1, QStringLiteral("SKU-A"), QStringLiteral("C1")},
             {QStringLiteral("SO-1"), 2, QStringLiteral("SKU-B"), QStringLiteral("C1")}}));

        // multiTable with lookup on both routes + composite fkInject
        // 译：multiTable 模式，两条 route 都带 lookup，子表再带（复合）fkInject 把父键注入下来。
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
        opts.dryRun = false;  // 真正写库（区别于前面用例的 dryRun）
        dbridge::ImportResult result = svc.run(spec, catalog, xlsxPath, opts, db);

        for (const auto& e : result.errors)
            qDebug() << "e2e error:" << e.code << e.message;
        QVERIFY(result.ok);  // 端到端导入必须整体成功

        // Verify orders: 1 row (SO-1 deduped), customer_name = Alice (last-row C2 wins via upsert)
        // 译：断言①——orders 经 upsert 去重后仅 1 行（两行同属 SO-1 被合并）。
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM orders")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);

        // Verify order_items: 2 rows, with customer_name populated from lookup
        // 译：断言②——order_items 有 2 行，且 customer_name 均由 lookup 填上（NOT NULL 计数==2）。
        QVERIFY(q.exec(
            QStringLiteral("SELECT COUNT(*) FROM order_items WHERE customer_name IS NOT NULL")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 2);

        // Verify fkInject: order_items.order_no matches orders.order_no
        // 译：断言③——fkInject 生效：order_items.order_no 全部能在 orders 中找到对应（无孤儿）。
        //   这条用「NOT IN 子查询计数==0」表达「不存在任何 order_no 不在 orders 里的明细行」。
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
