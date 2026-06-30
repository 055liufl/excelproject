// Unit + integration tests for export-direction reverse lookup (add-export-reverse-lookup).
//
// Tests 7.1-7.11 from tasks.md.
//
// Test strategy: Use a real SQLite in-memory (or temp-file) DB. Run ExportService.run()
// against a temp xlsx path, then read the xlsx back with QXlsx::Document to verify cell values.
//
// ============================================================================
// tst_reverse_lookup_export.cpp — 「导出方向反向查找(reverse lookup)」单元/集成测试
// ============================================================================
//
// 【先理解被测特性:什么是「反向查找」(理解全部用例的前提)】
//   ETL 导入时常做「正向查找」:Excel 里给的是人类可读码(如客户编号 CustNo='C1'),
//   导入时去参照表(G 表,如 ref_customers)按该码查出真实业务值(如客户名 'Alice')
//   写进主表的 H 列(customer_name)。
//   「反向查找」就是导出时的逆操作:数据库里主表存的是 H 值(customer_name='Alice'),
//   导出回 Excel 时,沿同一个 G 表把 H 值「反查」回当初那个 A 码(CustNo='C1'),
//   于是 Excel 里重新出现的是 A 列(CustNo)、而不是 H 列(customer_name)。
//   这样「导入→导出」能往返(round-trip)回到用户最初填的那张表的形态。
//
//   术语速记(贯穿全文件):
//     · A 列 / A-header:Excel 侧的「码」列,来自 lookup 的 match 右值(如 "CustNo")。
//     · H 列:数据库侧存的「真实值」列,来自 lookup 的 select 左值(如 customer_name)。
//     · G 表(from):参照表,A 码 ↔ H 值 的对照来源(如 ref_customers)。
//     · exportRoundtrip:该 lookup 导出时是否做反查。true=H 换成 A;false=H 原样保留。
//     · exportOnMissing:反查在 G 表里找不到对应行时的策略——
//           "error"=报 E_REVERSE_LOOKUP_NOT_FOUND 并「整行跳过」(H-03 契约);
//           "null" =A 列写空、不报错、行照常导出;
//           "skip" =行照常导出但 A 列留空、不报错(与 null 的细微差别见各用例)。
//
// 【测试策略(每个用例的统一套路 GIVEN/WHEN/THEN)】
//   GIVEN: 用临时文件 SQLite 库建好主表 + G 表并塞入已知数据(setupDb);
//   WHEN : 用一份描述 lookup 规则的 Profile(JSON)跑真实的 ExportService.run() 导出到临时 xlsx;
//   THEN : 用 QXlsx 把导出的 xlsx 读回来,断言「表头里该有 A 列、不该有 H 列」以及具体单元格值。
//   这是「集成测试」:走的是真库 + 真 xlsx 文件 + 真导出服务,而非 mock,故能端到端守住契约。
//
// 【与 tasks.md 的对应】方法注释里的 7.1/7.4a/W2/W3 等编号即需求清单中的验收项编号,务必保留。
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
#include "profile/ProfileValidator.h"
#include "schema/SchemaCatalog.h"
#include "schema/SchemaIntrospector.h"
#include "service/ErrorCollector.h"
#include "service/ExportService.h"
#include "service/ImportService.h"
#include <xlsxcellrange.h>
#include <xlsxdocument.h>

using namespace dbridge::detail;

// ── Shared helpers(测试夹具:供所有用例复用的小工具)──────────────────────────

// makeUuid —— 生成去掉花括号与连字符的纯字母数字 UUID 串。
//   用途:为每条数据库连接名、每个临时 db/xlsx 文件名拼一个唯一后缀,保证用例间不撞名。
static QString makeUuid() {
    return QUuid::createUuid().toString().remove('{').remove('}').remove('-');
}

// setupDb —— 在 path 处打开(创建)一个 SQLite 库,按顺序执行一批建表/插数 SQL,然后关闭。
//   做什么:为某个用例「布置好初始数据库状态」(GIVEN 阶段)。
//   返回:成功返回空串;任一步失败(打开失败 / 某条 SQL 失败)返回一段可读错误描述,
//        调用方据此 QVERIFY2 报错。用「返回错误字符串、空串即成功」而非抛异常,贴合 QtTest 风格。
//   细节:用独立的临时连接名建库,执行完即 close + removeDatabase,使「布置库」与「跑导出」
//        各用各的连接,互不干扰;库文件留在磁盘供随后的 ExportRunner 重新连上读取。
static QString setupDb(const QString& path, const QStringList& sqls) {
    QString conn = QStringLiteral("setup_") + makeUuid();
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(path);
    if (!db.open())
        return QStringLiteral("open failed: ") + db.lastError().text();
    QSqlQuery q(db);
    for (const QString& sql : sqls)
        if (!q.exec(sql))
            return sql + QStringLiteral(" failed: ") +
                   q.lastError().text();  // 哪条 SQL 失败一并回报
    db.close();
    QSqlDatabase::removeDatabase(conn);
    return {};
}

// writeXlsx —— 写一张简单的 .xlsx(单 sheet、首行表头 + 若干数据行),供「导入再导出」往返用例铺数据。
//   约定:第 1 行为表头(headers),数据从第 2 行起;QXlsx 行列号都是 1-based,故 +1/+2 换算。
//   NULL 值跳过不写(留空单元格),模拟 Excel 里「该格为空」的情形。
//   返回:saveAs 是否成功(磁盘写入失败 / addSheet 失败均返回 false)。
static bool writeXlsx(const QString& path, const QString& sheet, const QStringList& headers,
                      const QVector<QVector<QVariant>>& rows) {
    QXlsx::Document doc;
    if (!doc.addSheet(sheet))
        return false;
    if (!doc.selectSheet(sheet))
        return false;
    for (int c = 0; c < headers.size(); ++c)
        doc.write(1, c + 1, headers[c]);  // 表头写在第 1 行
    for (int r = 0; r < rows.size(); ++r)
        for (int c = 0; c < rows[r].size(); ++c)
            if (!rows[r][c].isNull())
                doc.write(r + 2, c + 1, rows[r][c]);  // 数据从第 2 行起;NULL 不写(留空格)
    return doc.saveAs(path);
}

// parseProfile —— 把一段 JSON 文本解析成 ProfileSpec(导入/导出规则对象)。
//   解析失败用 qFatal 直接终止整个测试进程——因为 Profile 是用例的「输入前提」,写错了再跑无意义,
//   早失败、报清楚比让后续断言莫名其妙地挂掉更好排查。
static ProfileSpec parseProfile(const QString& json) {
    ProfileLoader loader;
    ProfileSpec spec;
    QString err;
    if (!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err))
        qFatal("%s failed: %s", "parseProfile", err.toUtf8().constData());
    return spec;
}

// ExportRunner —— 封装「连库 → 自省 → 跑 ExportService 导出」这套固定动作的小夹具。
//   为何独立成结构:大多数用例都要做同样的几步,抽成 run() 一行调用,使各用例正文只关心
//   「给什么 Profile、断言什么输出」。dbPath/xlsxPath 由用例先填好,catalog 在 run() 内现自省。
struct ExportRunner {
    QString dbPath;         // 被导出的源库路径(用例先 setupDb 准备好)
    QString xlsxPath;       // 默认导出目标 xlsx 路径
    SchemaCatalog catalog;  // run() 内自省填充的结构目录(导出需要它做列映射)

    // run —— 用给定 Profile 把 dbPath 导出到 outXlsx(缺省用 xlsxPath),返回导出结果。
    //   流程:开唯一命名连接 → SchemaIntrospector 自省 schema → ExportService.run 执行导出 →
    //        关闭并注销连接。ExportResult 里含 writtenRows(写出行数)与 errors(错误列表),
    //        是各用例断言的主要对象。
    dbridge::ExportResult run(const ProfileSpec& spec, const QString& outXlsx = QString()) {
        QString out = outXlsx.isEmpty() ? xlsxPath : outXlsx;
        QString conn = QStringLiteral("run_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        db.open();

        SchemaIntrospector si;
        si.load(db, &catalog, nullptr);  // 导出前必须先有准确 schema(列名/主键等)

        ExportService svc;
        dbridge::ExportOptions opts;
        auto res = svc.run(spec, catalog, out, opts, db);

        db.close();
        QSqlDatabase::removeDatabase(conn);
        return res;
    }
};

// Read a cell value from a saved xlsx file (1-based row/col).
// readXlsxCell —— 从已保存的 xlsx 读取单个单元格的值(行列均 1-based)。选错 sheet 返回无效
// QVariant。
static QVariant readXlsxCell(const QString& path, const QString& sheet, int row, int col) {
    QXlsx::Document doc(path);
    if (!doc.selectSheet(sheet))
        return QVariant();
    return doc.read(row, col);
}

// Read all rows (including header row 1) from a saved xlsx.
// Returns header list and data rows as strings.
// readXlsxSheet —— 把整张 sheet 读回内存:headers 收第 1 行表头,rows 收第 2 行起的数据(均转字符串)。
//   是 THEN 阶段的核心工具——几乎所有用例都靠它取回「实际导出了哪些列、各格是什么值」来与预期比对。
//   先用 dimension() 求出有效行列范围,再逐格 read().toString()(统一成字符串便于直接 QCOMPARE
//   比较)。
static void readXlsxSheet(const QString& path, const QString& sheet, QStringList* headers,
                          QVector<QStringList>* rows) {
    QXlsx::Document doc(path);
    if (!doc.selectSheet(sheet))
        return;
    // Find dimensions
    // 【译】先取 sheet 的有效范围(末行、末列),据此界定要遍历多少行列。
    int maxRow = 0, maxCol = 0;
    QXlsx::CellRange dim = doc.dimension();
    maxRow = dim.lastRow();
    maxCol = dim.lastColumn();

    for (int c = 1; c <= maxCol; ++c)
        headers->append(doc.read(1, c).toString());  // 第 1 行 = 表头

    for (int r = 2; r <= maxRow; ++r) {  // 第 2 行起 = 数据
        QStringList dataRow;
        for (int c = 1; c <= maxCol; ++c)
            dataRow.append(doc.read(r, c).toString());
        rows->append(dataRow);
    }
}

// hasCode —— 在导出结果的 errors 列表里查找是否出现某个错误码(如 E_REVERSE_LOOKUP_NOT_FOUND)。
//   供「期望导出报某错」的用例做断言,把「遍历 errors 找 code」这件事收拢成一行可读判断。
static bool hasCode(const dbridge::ExportResult& r, const QString& code) {
    for (const auto& e : r.errors)
        if (e.code == code)
            return true;
    return false;
}

// ── Test class(测试类:每个 private slot 是一个验收用例)───────────────────────

class TstReverseLookupExport : public QObject {
    Q_OBJECT

    // 整个测试类共用的临时目录;析构时自动递归删除,免去手工清理临时 db/xlsx。
    QTemporaryDir tmp_;

    // newDb/newXlsx —— 在临时目录下生成一个唯一文件名(供本用例独占),避免用例间文件互相污染。
    QString newDb() const {
        return tmp_.path() + '/' + makeUuid() + ".db";
    }
    QString newXlsx() const {
        return tmp_.path() + '/' + makeUuid() + ".xlsx";
    }

   private slots:
    // initTestCase —— 整个测试类「只跑一次」的初始化:确认临时目录创建成功,否则后续无处落文件。
    void initTestCase() {
        QVERIFY(tmp_.isValid());
    }

    // 7.1a: single-pair lookup → A column appears, H column absent in output
    // testSinglePairRoundtrip —— 7.1a:最简单的「单对 match」反查。
    //   GIVEN: orders 存 customer_name(H 值,如 'Alice');ref_customers 是 A↔H 对照(C1↔Alice);
    //          lookup 用 match c_no↔CustNo、select c_name↔customer_name。
    //   WHEN : 导出;
    //   THEN : Excel 表头出现 A 列 "CustNo"、不出现 H 列 "customer_name";
    //          且 SO-1 行反查得 C1、SO-2 行反查得 C2(H 值被成功换回 A 码)。
    //   不变量:exportRoundtrip 默认开启时,H 列被 A 列取代,实现导入↔导出往返。
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
        // 【译】反查后:H 列(customer_name)不应出现;A 列(CustNo)应出现。
        QVERIFY2(headers.contains(QStringLiteral("CustNo")), "CustNo (A) not in headers");
        QVERIFY2(!headers.contains(QStringLiteral("customer_name")),
                 "customer_name (H) should be absent");

        // Row SO-1 → CustNo = C1, Row SO-2 → CustNo = C2
        // 【译】逐行核对反查结果:SO-1 的 'Alice' 反查回 C1、SO-2 的 'Bob' 反查回 C2。
        //   导出行无序,故按 OrderNo 定位各行再取其 CustNo 列值比对(而非假设行顺序)。
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
    // testCompositePairRoundtrip —— 7.1b:「复合 match」(两对匹配键)的反查。
    //   GIVEN: ref_sku 用 (t_code, sku_code) 复合主键对照 (tenant_id, line_no) 两个 H 值;
    //          lookup 的 match 含两对、select 含两个 H 列。
    //   WHEN : 导出;
    //   THEN : 两个 A 列(TenantCode/SkuCode)都出现、两个 H 列(tenant_id/line_no)都消失;
    //          且每行的两个 A 码都被同时正确还原(复合键反查必须整组匹配)。
    //   不变量:多对 match 时,A 列须「成组」出现/还原,不能只还原其中一部分。
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
    // testIdentityMerging —— 7.1c:「身份合并」——两条路由用了完全相同的 lookup 定义
    //   (相同的 from+match+select),导出器应识别为同一「身份」并共享同一次 G 表预取,而不是
    //   各查一遍、各报一遍错。
    //   GIVEN: multiTable 模式下 orders 与 items 两条路由都声明了同一个 ref_customers 反查;
    //   WHEN : 导出;
    //   THEN : 不产生重复错误,所有行的 CustNo 都正确反查为 C1(合并预取后结果仍正确)。
    //   不变量:相同身份的反查去重共享,既省查询又避免重复错误干扰诊断。
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
    // testFullRoundtrip —— 7.2:完整「往返」集成测试——先导入(A 码→存 H 值),再导出(H 值→还原 A 码),
    //   验证导出回来的 A 列值与最初导入的 A 码完全一致(端到端闭环)。
    //   GIVEN: 空 orders 表 + ref_customers 对照;一张含 OrderNo/CustNo 的导入 xlsx(C1/C2);
    //   WHEN : 先用 ImportService 把该 xlsx 正向导入(CustNo 经正查写成 customer_name 存库),
    //          再用 ExportService 反向导出;
    //   THEN : 导出的 SO-1 行 CustNo==C1、SO-2 行==C2——与导入时填的码一字不差。
    //   不变量:正向查找与反向查找互为逆运算,数据经一来一回不失真。
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
        // 【译】第一程:正向导入——把 OrderNo/CustNo 的 xlsx 经 ImportService 写库
        //   (CustNo 会经正向查找转成 customer_name 落到 orders.customer_name)。
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
        // 【译】第二程:反向导出——同一份 Profile,把库里的 customer_name 反查回 CustNo。
        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 2);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        int custNoCol = headers.indexOf(QStringLiteral("CustNo"));
        int orderNoCol = headers.indexOf(QStringLiteral("OrderNo"));
        QVERIFY(custNoCol >= 0 && orderNoCol >= 0);

        // 按 OrderNo 定位各行,核对其 CustNo 是否回到导入时填的码。
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
    // testRoundtripFalse —— 7.3:把 lookup 的 exportRoundtrip 关掉,验证「不做反查」。
    //   GIVEN: lookup 显式声明 "exportRoundtrip":false;
    //   WHEN : 导出;
    //   THEN : H 列(customer_name)原样保留、A 列(CustNo)不出现——与 7.1a 恰好相反。
    //   不变量:exportRoundtrip 是反查的总开关;关掉即退化为「直接导出 H 列」的普通行为。
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
        // 【译】关掉反查后:H 列(customer_name)保留、A 列(CustNo)缺席。
        QVERIFY2(headers.contains(QStringLiteral("customer_name")),
                 "customer_name (H) should be present");
        QVERIFY2(!headers.contains(QStringLiteral("CustNo")), "CustNo (A) should be absent");
    }

    // 7.4a: exportOnMissing: "error" (default) → E_REVERSE_LOOKUP_NOT_FOUND, entire row skipped
    // H-03 fix: exportOnMissing="error" must skip the entire row (rowSkip=true), matching the
    // OpenSpec contract. The error is still reported. Previously (H-04 fix) only the A-column was
    // cleared, but that deviates from the spec which says "error" means skip-the-row.
    // testOnMissingError —— 7.4a:反查未命中且策略为 "error" 时,应报错并「整行跳过」。
    //   GIVEN: ref_customers 为空(任何反查必然落空);主表有一行 customer_name='NoSuchCustomer';
    //   WHEN : 以 exportOnMissing:"error" 导出;
    //   THEN : 结果含错误码 E_REVERSE_LOOKUP_NOT_FOUND,且 writtenRows==0(整行被丢弃)。
    //   契约要点(H-03):"error" 的语义是「跳过整行」而非「只清空 A 列」——这是 H-03 对早先
    //     H-04 行为的纠正;原英文注释保留了这段演进背景,务必照看。
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
        QVERIFY(hasCode(res, QStringLiteral("E_REVERSE_LOOKUP_NOT_FOUND")));  // 必须报该错
        // H-03 fix: row is skipped entirely when exportOnMissing="error" and lookup misses.
        // 【译】H-03:"error" 且反查落空时整行跳过,故写出 0 行。
        QCOMPARE(res.writtenRows, 0);
    }

    // 7.4b: exportOnMissing: "null" → A-col written as empty, no error, row continues
    // testOnMissingNull —— 7.4b:反查未命中且策略为 "null" 时,A 列写空、不报错、行照常导出。
    //   GIVEN: 反查必落空(ref_customers 空);WHEN: 以 exportOnMissing:"null" 导出;
    //   THEN : 无错误、writtenRows==1、且该行 CustNo 单元格为空串(NULL 在 xlsx 表现为空)。
    //   不变量:"null" 是「容忍缺失」策略——不阻断导出,只把还原不出的 A 列留白。
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
        QCOMPARE(res.writtenRows, 1);  // row written with NULL A-col(行照写,A 列为 NULL)

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);
        int custNoCol = headers.indexOf(QStringLiteral("CustNo"));
        QVERIFY(custNoCol >= 0);
        QVERIFY(
            rows[0][custNoCol].isEmpty());  // NULL → empty string in xlsx(NULL 在 xlsx 里是空串)
    }

    // 7.4c: exportOnMissing: "skip" → row written (no error), A-col is empty
    // testOnMissingSkip —— 7.4c:反查未命中且策略为 "skip" 时,行照常写出(不报错)、A 列留空。
    //   GIVEN: 反查必落空;WHEN: 以 exportOnMissing:"skip" 导出;
    //   THEN : 无错误、writtenRows==1(本用例只校验「行不丢、不报错」,不再细看单元格)。
    //   与 "null" 的语义差别:此处契约关注「跳过缺失项而保留整行」,具体留白行为见实现。
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
        QCOMPARE(res.writtenRows,
                 1);  // row written with empty A-col, no error(行写出、A 列空、无错)
    }

    // 7.5: E_REVERSE_LOOKUP_AMBIGUOUS — G table has duplicate select-col values
    // testAmbiguous —— 7.5:G 表里「同一个 H 值对应多个 A 码」时,反查歧义应报错。
    //   GIVEN: ref_customers 有两行 c_name 都是 'Alice'(C1→Alice、C2→Alice),即 H 值不唯一;
    //   WHEN : 反查 'Alice' 该取 C1 还是 C2? 无法确定;
    //   THEN : 报 E_REVERSE_LOOKUP_AMBIGUOUS 且 writtenRows==0。
    //   不变量:反查要求 H→A 映射唯一;一对多时宁可报错也不能任意挑一个,以免悄悄写错数据。
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
    // testPrefetchFailed —— 7.6:预取 G 表本身就 SQL 出错(如 G 表根本不存在)时,应报查询失败。
    //   GIVEN: lookup 指向 ref_customers,但该表从未创建,SELECT 必然失败;
    //   WHEN : 导出试图预取 G 表;
    //   THEN : 报 E_REVERSE_LOOKUP_QUERY_FAILED 且 writtenRows==0。
    //   不变量:把「G 表查询执行失败」与「查到了但没匹配(NOT_FOUND)」区分为两类错误,便于排障。
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
    // testNullHValue —— 7.11:数据库里 H 列本身就是 NULL 时,应「当作未命中」按 exportOnMissing
    // 处理。
    //   GIVEN: orders 一行 customer_name 为 NULL;lookup 用 exportOnMissing:"null";
    //   WHEN : 反查一个 NULL 的 H 值(没有可查的码);
    //   THEN : 等同未命中→按 "null" 策略:行照写、A 列留空、不报错。
    //   不变量:NULL H 值不应崩溃或乱填,而是归入既有的「缺失」分支由 exportOnMissing 统一裁决。
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
    // testColumnOrderWithAHeader —— 7.9:导出 columnOrder(指定列顺序)里写 A 列名应被接受。
    //   GIVEN: export.columnOrder = ["CustNo","OrderNo"](CustNo 是反查产生的 A 列名);
    //   WHEN : 导出;
    //   THEN : 无错误,且导出表头第 0 列为 CustNo、第 1 列为 OrderNo(列顺序如约)。
    //   不变量:反查产生的 A 列名是「合法的输出表头」,可被 columnOrder 引用来排版;
    //          与之相对的「H 列名出现在 columnOrder」的非法情形由 7.9b 用例覆盖。
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
    // testMixedModeRoundtrip —— 7.8:mixed(多类别混合)模式下的反查,跨类别共享同一身份的预取。
    //   GIVEN: mixed 模式两个 class(orders、invoices)各自一条路由,都对同一 ref_customers 反查;
    //          一份输出 sheet 同时容纳两类记录(靠 classColumn 区分)。
    //   WHEN : 导出;
    //   THEN : 表头出现 A 列 CustNo、无 H 列;两类各自的行被正确反查(出现 C1 与 C2)。
    //   不变量:mixed 模式下相同身份的反查跨 class 合并预取,但每个 class 仍按自身路由独立解析输出。
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
            custNos.insert(row[custNoCol]);  // 收集所有行的 CustNo,用集合判断「两个码都出现过」
        // One row should have C1 (Alice→SO-1) and one C2 (Bob→INV-1)
        // 【译】应有一行得 C1(orders 类 Alice)、另一行得 C2(invoices 类 Bob)。
        QVERIFY(custNos.contains(QStringLiteral("C1")));
        QVERIFY(custNos.contains(QStringLiteral("C2")));
    }

    // 7.10: A-column governed by dateFormat → temporal processing applied to reverse-lookup value
    // testAColumnWithTemporalFormat —— 7.10:A 列若同时受 dateFormat
    // 约束,反查得到的值应再过一遍日期格式化。
    //   场景:G 表 ref_periods 把 period_id(码)对照 event_date(日期值,dbFormat "yyyy-MM-dd");
    //        主表 event_date 列为 NULL,故导出值「回退」到反查结果;且该列 ColumnSpec 带 dateFormat。
    //   GIVEN: ref_periods 有 P1→'2025-03-14';orders 一行 event_date=NULL、period_id='P1';
    //          列 event_date 的 dateFormat 为 excel "yyyy/M/d" ↔ db "yyyy-MM-dd"。
    //   WHEN : 导出(反查得到日期串 '2025-03-14');
    //   THEN : H 列 period_id 不出现、A 列 OrderDate 出现;且该格被 temporal 转成 Excel 格式
    //   '2025/3/14'。
    //   不变量:反查结果并非「原样落格」,后续的列级日期/格式处理仍会作用于它(两段逻辑可叠加)。
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
        // 【译】反查得 dbFormat 的 "2025-03-14",再经 temporal 转成 excelFormat 的 "2025/3/14"。
        QCOMPARE(rows[0][orderDateCol], QStringLiteral("2025/3/14"));
    }

    // W2: same route with L1(roundtrip=true) and L2(roundtrip=false) — L1's H replaced, L2's H kept
    // testMixedRoundtripAcrossLookups —— W2:同一路由里两个 lookup 的 roundtrip 开关互不影响。
    //   GIVEN: 同一 orders 路由声明 L1(ref_customers,roundtrip=true)与
    //   L2(ref_categories,roundtrip=false); WHEN : 导出; THEN : L1 → H 列 customer_name 被换成 A 列
    //   CustNo;L2 → H 列 category_id 原样保留、A 列 Category 不出现;
    //          且 CustNo 为反查结果 C1、category_id 为直接 DB 值 category_id_1。
    //   不变量:roundtrip 是「逐 lookup」的开关,同一行里可一个反查、一个直出,彼此独立结算。
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
        QCOMPARE(rows[0][custNoCol], QStringLiteral("C1"));  // reverse-lookup result(L1 反查结果)
        int catCol = headers.indexOf(QStringLiteral("category_id"));
        QCOMPARE(rows[0][catCol],
                 QStringLiteral("category_id_1"));  // direct DB value(L2 直出 DB 原值)
    }

    // W3: D5 — ColumnSpec.source value wins over reverse-lookup A value when non-NULL
    // testD5SourceWinsOverReverseLookup —— W3:当某列既有显式 ColumnSpec.source、又恰是某 lookup 的
    // A 列名时,
    //   且该列在 DB 里有非 NULL 直值,则「直值优先」覆盖反查结果(决策点代号 D5)。
    //   GIVEN: orders.client_no 列 source="ClientNo"(非空,值 'DIRECT-99');同名 ClientNo 又是反查的
    //   A 列名;
    //          反查若生效会得 c_no='C1';
    //   WHEN : 导出;
    //   THEN : ClientNo 列输出 'DIRECT-99'(ColumnSpec 直值)而非 'C1'(反查值)。
    //   不变量(D5
    //   优先级):列的「显式直值」高于「反查推导值」——避免反查悄悄改写用户已明确给出的列值。
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
        // 【译】ColumnSpec 的直值 'DIRECT-99' 必须压过反查结果 'C1'(D5 优先级)。
        QCOMPARE(rows[0][clientNoCol], QStringLiteral("DIRECT-99"));
    }

    // 7.9b: H-col in columnOrder (with exportRoundtrip=true) → E_EXPORT_UNKNOWN_HEADER from
    // validator
    // testColumnOrderRejectsHHeader —— 7.9b:roundtrip=true 时把 H 列名写进 columnOrder
    // 应被校验器拒绝。
    //   与 7.9 互补:7.9 证明「A 列名合法」,本用例证明「H 列名非法」。
    //   GIVEN: lookup roundtrip 默认开启(H→A),但 columnOrder 里却写了 H 列名 "customer_name";
    //          已知的合法输出表头只有 OrderNo(source)与 CustNo(A 列名);
    //   WHEN : 用 ProfileValidator.validate 对照真库 schema + 已知表头集合做校验;
    //   THEN : 校验不通过(返回 false),且错误列表含 E_EXPORT_UNKNOWN_HEADER。
    //   不变量:roundtrip 开启后 H 列名不再是有效输出表头,引用它即「未知表头」,应在校验期就拦下。
    //   注意:本用例不跑导出,直接调 validator——因为这是「配置校验期」就该报的错,而非运行期。
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

// QTEST_MAIN —— 生成 main():实例化 TstReverseLookupExport,依次跑遍其所有 private slot 用例。
QTEST_MAIN(TstReverseLookupExport)
// 引入 moc 为本 TU 生成的元对象代码(Q_OBJECT 所需);文件名须与本 .cpp 同名。
#include "tst_reverse_lookup_export.moc"
