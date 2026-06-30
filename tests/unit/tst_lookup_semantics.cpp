// ============================================================================
// tst_lookup_semantics.cpp — lookup（参照表查找）运行期语义的端到端测试
// ============================================================================
//
// 【lookup 是什么 / 验证什么】
//   导入时，Excel 里常是「业务编码」（如客户编号 CustNo），但库里要存「主键/规范值」
//   （如 customer_name 或 tenant_id）。lookup 就是「拿 Excel 的某列去参照表里查，把查到的
//   值写进本地列」的机制。本文件验证 lookup 在真实导入中的「命中数语义」与若干边界：
//     · §5.4 零命中 → E_LOOKUP_NOT_FOUND（查不到，报错）；
//     · §5.6 一命中 → 把值传播进 payload（成功）；
//     · §5.5 多命中 → E_LOOKUP_AMBIGUOUS（参照表有重复，歧义，报错，且提示「去重」）；
//     · §5.2 空键语义：null/""/"  "（纯空白）算「空键」→ E_LOOKUP_KEY_EMPTY；
//             但「数字 0」不算空（0 是合法键值）；
//     · §5.6 NULL 透传：参照表 select 出的列本身是 NULL → 不报错，NULL 原样进 payload；
//     · §10.9 lookup 产出可参与 conflict.columns（查出的值能当冲突键的一部分）；
//     · §10.8 级联抑制：父路由 lookup 失败时，子路由不应再报「次生错误」（只报根因一条）。
//
// 【测试手法：真库 + 真 xlsx + dryRun 导入】
//   每个用例都：建一个磁盘临时库 + 写一份真实 xlsx + 跑 ImportService 的「dry-run 导入」。
//   dryRun=true 关键：它走完整的「映射→lookup→fkInject」全链路、把构造好的 payload 填进
//   ImportResult::dryRunPayloads，但「不真正 UPSERT 落库」——于是既能观察「值是否被正确查出/
//   传播」，又不污染库、可反复跑。断言对象就是 result.errors（错误码）与 dryRunPayloads（值）。
//
// 【夹具】initTestCase 只建一次临时目录 tmp_（QTemporaryDir，析构自动删）；每个用例用
//   newDb()/newXlsx() 在该目录下生成唯一文件名，互不干扰。
// ============================================================================
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
// ── 公共测试辅助 ──────────────────────────────────────────────────────────────

// makeUuid —— 生成一段去掉花括号与连字符的 UUID，用作唯一的连接名/文件名片段。
static QString makeUuid() {
    return QUuid::createUuid().toString().remove('{').remove('}').remove('-');
}

// writeXlsx —— 把给定表头与行数据写成一份真实 .xlsx 文件（用 QXlsx）。
//   参数：path 落盘路径；sheet 工作表名；headers 第 1 行表头；rows 第 2 行起的数据。
//   细节：① 第 1 行写表头，第 r 行数据写到 Excel 第 r+2 行（让出表头那行）；
//         ② 对 null 的单元格「跳过不写」——这样该格在 xlsx 里就是「真正的空」，用于精确
//            构造「源列为 null」这类边界场景（如 testEmptyKeyNull）。
//   返回：保存成功与否。
static bool writeXlsx(const QString& path, const QString& sheet, const QStringList& headers,
                      const QVector<QVector<QVariant>>& rows) {
    QXlsx::Document doc;
    if (!doc.addSheet(sheet))
        return false;
    if (!doc.selectSheet(sheet))
        return false;
    for (int c = 0; c < headers.size(); ++c)
        doc.write(1, c + 1, headers[c]);  // 表头落在第 1 行（QXlsx 行列从 1 起）
    for (int r = 0; r < rows.size(); ++r)
        for (int c = 0; c < rows[r].size(); ++c)
            if (!rows[r][c].isNull())  // null 跳过 → 该格保持真空（用于空键测试）
                doc.write(r + 2, c + 1, rows[r][c]);
    return doc.saveAs(path);
}

// setupDb —— 在 path 处建库并按序执行一组建表/插入 SQL，用于铺好「业务表 + 参照表」。
//   返回空串=成功；非空串=出错（首条失败的 SQL + 底层报错），便于 QVERIFY2 直接打印定位。
//   用完即关闭并 removeDatabase（建库与跑导入用的是不同连接，互不干扰）。
static QString setupDb(const QString& path, const QStringList& sqls) {
    QString conn = QStringLiteral("setup_") + makeUuid();
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(path);
    if (!db.open())
        return QStringLiteral("open failed: ") + db.lastError().text();
    QSqlQuery q(db);
    for (const QString& sql : sqls)
        if (!q.exec(sql))
            return sql + " failed: " + q.lastError().text();  // 出错即回传出错的那条 SQL
    db.close();
    QSqlDatabase::removeDatabase(conn);
    return {};
}

// parseProfile —— 把 profile JSON 解析成 ProfileSpec；解析失败直接 qFatal 终止测试。
//   这里用 qFatal（而非返回错误）是因为：profile 是测试的「输入前提」，写错就该立即崩、
//   而非掩盖成后续断言失败——属测试自身的前置条件，不是被测对象的行为。
static ProfileSpec parseProfile(const QString& json) {
    ProfileLoader loader;
    ProfileSpec spec;
    QString err;
    if (!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err))
        qFatal("%s failed: %s", "parseProfile", err.toUtf8().constData());
    return spec;
}

// RunResult —— 一次 dry-run 导入的产物打包：导入结果 + 当时反射出的 schema catalog。
struct RunResult {
    dbridge::ImportResult result;  // 导入结果（errors / dryRunPayloads 等）
    SchemaCatalog catalog;         // 由 SchemaIntrospector 反射出的库结构快照
};

// runDryRun —— 本文件的核心夹具：对 (库, xlsx, profile) 跑一次「dry-run 导入」。
//   步骤：开库 → 用 SchemaIntrospector 反射出 catalog（导入需要库结构信息）→ 配 ImportOptions
//         并强制 dryRun=true → 调 ImportService::run 走完整映射/lookup/fk 链路但不落库 →
//         关库返回结果。dryRun 让我们能检查「构造出的 payload 值」与「报出的错误」而不改库。
static RunResult runDryRun(const QString& dbPath, const QString& xlsxPath,
                           const ProfileSpec& spec) {
    QString conn = QStringLiteral("run_") + makeUuid();
    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
    db.setDatabaseName(dbPath);
    db.open();

    SchemaIntrospector si;
    RunResult res;
    si.load(db, &res.catalog, nullptr);  // 反射库结构 → catalog（列类型/主键等）

    ImportService svc;
    dbridge::ImportOptions opts;
    opts.profileName = spec.name;
    opts.dryRun = true;  // 关键：只构造 payload、不 UPSERT，使值可观测且不污染库
    res.result = svc.run(spec, res.catalog, xlsxPath, opts, db);

    db.close();
    QSqlDatabase::removeDatabase(conn);
    return res;
}

// hasCode —— 便捷断言辅助：导入结果的 errors 列表里是否出现过某个错误码。
static bool hasCode(const dbridge::ImportResult& r, const QString& code) {
    for (const auto& e : r.errors)
        if (e.code == code)
            return true;
    return false;
}

// ── Test class ──────────────────────────────────────────────────────────────

class TstLookupSemantics : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;  // 整个测试类共用的临时目录（析构自动清理）
    QString newDb() const {
        return tmp_.path() + '/' + makeUuid() + ".db";  // 每次返回一个唯一库文件路径
    }
    QString newXlsx() const {
        return tmp_.path() + '/' + makeUuid() + ".xlsx";  // 每次返回一个唯一 xlsx 路径
    }

   private slots:
    // initTestCase —— 整个测试类「只跑一次」的前置：确认临时目录创建成功。
    void initTestCase() {
        QVERIFY(tmp_.isValid());
    }

    // §5.4 zero hits → E_LOOKUP_NOT_FOUND
    // GIVEN 参照表 ref_customers 为空（没有任何客户行），Excel 行携带 CustNo="X999"，
    // WHEN  按「拿 CustNo 查 c_no、取 c_name 写 customer_name」的 lookup 做 dry-run 导入，
    // THEN  应报 E_LOOKUP_NOT_FOUND——查无此键即视为数据错误，必须显式失败而非静默写空。
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
    // GIVEN 参照表里恰有一行 (C1, Alice)，Excel 行 CustNo="C1"，
    // WHEN  dry-run 导入，
    // THEN  ① 无错误；② dryRunPayloads 里该行的 customer_name 列被填成查到的 "Alice"。
    //   验证「唯一命中时，select 的值确实被传播进了构造好的 payload」这一正路径。
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
    // GIVEN 参照表里 c_no="C1" 出现了两行（C1→Alice、C1→AliceDup，注意此表无主键约束），
    // WHEN  用 CustNo="C1" 查找，
    // THEN  ① 报 E_LOOKUP_AMBIGUOUS（多命中即歧义，不能擅自选一行）；
    //       ② 错误消息里应提到 "deduplicat"（去重）——提示用户「参照表需去重」的可操作指引。
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
    // 空键之「null」分支：Excel 的 CustNo 单元格为 null（writeXlsx 跳过该格 → 真空）。
    // WHEN 查找，THEN 报 E_LOOKUP_KEY_EMPTY——空键不去查表（空键查表无意义且易误命中），直接判错。
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

    // 空键之「纯空白」分支：CustNo="  "（两个空格）。WHEN 查找，THEN 同样报 E_LOOKUP_KEY_EMPTY——
    // 纯空白被规范为「空键」（trim 后为空），与 null/"" 同等对待。验证空键判定会先 trim。
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
    // 关键边界：「数字 0」不是空键。参照表 c_no 为 INTEGER 且有一行 (0,'Zero Corp')，
    // Excel CustId 填数字 0。THEN 既不应报 E_LOOKUP_KEY_EMPTY（0 是合法键值，不是空），
    // 也不应报 E_LOOKUP_NOT_FOUND（应能命中那行 0）。防止把「0」误当空值丢弃这一常见陷阱。
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
    // NULL 透传：参照行存在 (C1, NULL)——键能命中，但 select 出来的 c_name 本身是 NULL。
    // THEN 不报错（命中是成功的），且 payload 里的 customer_name 为 NULL。
    //   语义区分：这是「查到了，但值就是空」（合法，透传 NULL），与「查不到」（NOT_FOUND）
    //   是两回事；与「键为空」（KEY_EMPTY）更是两回事。
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
    // lookup 产出参与冲突键：表 items 的 conflict.columns 含 tenant_id，而 tenant_id 不是来自
    // Excel，而是 lookup 用 TenantCode="ACME" 查 ref_tenants 得到的 t_id="T001"。
    // THEN payload 的 conflictKey/conflictVals 里应出现 tenant_id=T001——证明「先做 lookup，
    //   再用其产出值构成冲突键」的时序正确（冲突键依赖 lookup 结果，必须在 lookup 之后组装）。
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
    // 级联抑制：multiTable 下父路由 orders 的 lookup 必然失败（ref_customers 空 → NOT_FOUND）。
    // 子路由 items 通过 fkInject 依赖父行；父都失败了，子自然无父可挂。
    // THEN 整个导入应「恰好 1 条错误」，且就是父上的 E_LOOKUP_NOT_FOUND——
    //   子路由不再追加「外键缺失」之类的次生错误（那只会淹没根因）。验证「只报根因、抑制级联」
    //   这一关键用户体验：一处真错，不该炸出一连串派生错误。
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

// QTEST_MAIN：生成跑本测试类全部用例的 main()；末尾并入 moc 生成的元对象代码（固定写法）。
QTEST_MAIN(TstLookupSemantics)
#include "tst_lookup_semantics.moc"
