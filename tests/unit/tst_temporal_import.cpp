// Integration tests for import-direction temporal format conversion (tasks.md 8.3).
//
// Test strategy: Real SQLite temp-file DB + real QXlsx xlsx files.
// Run ImportService.run(), then query DB to verify stored values.
//
// ============================================================================
// tst_temporal_import.cpp —「导入方向」时间/日期格式转换的集成测试（tasks.md 8.3）
// ============================================================================
//
// 【这个文件验证什么】
//   ETL 导入时，Excel 单元格里的「日期/时间」需要按 Profile 配置转换后再写入数据库：
//     · dateFormat   —— 纯日期：excelFormat（Excel 端字符串格式）→ dbFormat（库端字符串格式），
//                       可配 excelFormatFallback（主格式解析失败时的后备格式列表）；
//     · datetimeFormat —— 日期时间：excel.{type,format} → db.{type}，其中 db.type 可为
//                       epochSec（存为自 1970 起的秒数，qlonglong）等。
//   本测试覆盖：主格式命中、后备格式救场、全部失败报 E_TIME_PARSE 且不影响其它行、
//   原生 QDate/QDateTime 单元格「绕过」字符串解析、epochSec 各场景与边界、以及
//   「导入→导出」的端到端往返一致性。
//
// 【测试策略——真实组件、端到端】
//   不用 mock：真建一个临时文件 SQLite 库 + 真用 QXlsx 写出 .xlsx，跑真正的
//   ImportService::run()，再直接查库断言「存进去的值」。如此能验证整条链路
//   （ExcelReader 读 → Mapper 类型转换 → UPSERT 写库）的真实行为，而非桩替身。
//
// 【关键断言模式】
//   · QVERIFY2(res.errors.isEmpty(), …) —— 正常用例要求零错误，失败时打印首条错误消息；
//   · hasCode(res, "E_TIME_PARSE")     —— 失败用例要求出现特定错误码；
//   · runner.queryOne("SELECT …")      —— 回查库中实际存储值，验证转换结果正确。
// ============================================================================

#include "dbridge/Errors.h"

#include <QDate>
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
#include "service/ImportService.h"
#include <xlsxcellrange.h>
#include <xlsxdocument.h>

using namespace dbridge::detail;

// ── Shared helpers ────────────────────────────────────────────────────────────
// 共享夹具辅助：建库、写 xlsx、解析 Profile、查错误码、读 xlsx 列、以及封装一次导入运行。

// makeUuid —— 生成去掉花括号与连字符的 UUID 字符串。
// 用途：给每个临时数据库连接拼一个唯一连接名，避免多用例/多连接重名冲突。
static QString makeUuid() {
    return QUuid::createUuid().toString().remove('{').remove('}').remove('-');
}

// setupDb —— 在 path 处建库并依次执行一组建表 SQL。
// 返回：空字符串=成功；非空=出错描述（含失败的那条 SQL 与驱动错误文本）。
// 设计：用「返回错误字符串」而非布尔，使调用处 QVERIFY2(setupDb(...).isEmpty(), …) 能
//   在失败时直接打印原因。建完即 close + removeDatabase，不残留连接。
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
    return {};  // 全部成功
}

// writeXlsx —— 用 QXlsx 写出一个真实 .xlsx：第 1 行表头，第 2 行起为数据。
// 参数：path 输出文件；sheet 表名；headers 表头列名；rows 数据（外层=行，内层=列）。
// 关键细节：
//   · QXlsx 行列号 1 基，故表头写在 (1, c+1)，数据写在 (r+2, c+1)；
//   · 对 isNull() 的单元格「不写」——留空，用以模拟「该格在 Excel 里就是空的」。
// 返回：saveAs 成功与否。
static bool writeXlsx(const QString& path, const QString& sheet, const QStringList& headers,
                      const QVector<QVector<QVariant>>& rows) {
    QXlsx::Document doc;
    if (!doc.addSheet(sheet))
        return false;
    if (!doc.selectSheet(sheet))
        return false;
    for (int c = 0; c < headers.size(); ++c)
        doc.write(1, c + 1, headers[c]);  // 表头行（1 基坐标）
    for (int r = 0; r < rows.size(); ++r)
        for (int c = 0; c < rows[r].size(); ++c)
            if (!rows[r][c].isNull())                 // 空值不写 → 模拟真实空单元格
                doc.write(r + 2, c + 1, rows[r][c]);  // 数据从第 2 行起
    return doc.saveAs(path);
}

// parseProfile —— 把 JSON 文本解析成 ProfileSpec；解析失败直接 qFatal 终止测试。
// 为什么失败即 qFatal：测试里的 Profile JSON 是「测试自己写死的」，解析失败说明测试
//   用例本身写错了（而非被测逻辑出问题），属不可恢复的夹具错误，应立即中止并报位置。
static ProfileSpec parseProfile(const QString& json) {
    ProfileLoader loader;
    ProfileSpec spec;
    QString err;
    if (!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err))
        qFatal("%s failed: %s", "parseProfile", err.toUtf8().constData());
    return spec;
}

// hasCode —— 判断一次导入结果的错误列表里是否出现某个错误码。
// 用于「失败用例」断言：如要求出现 E_TIME_PARSE。
static bool hasCode(const dbridge::ImportResult& r, const QString& code) {
    for (const auto& e : r.errors)
        if (e.code == code)
            return true;
    return false;
}

// readXlsxCol —— 从导出的 .xlsx 里按表头名读取某一整列的字符串值。
// 用途：往返(roundtrip)测试里，读回「导出后的 xlsx」某列，与原始输入逐行比对。
// 流程：① 选表 → ② 取数据维度 → ③ 在第 1 行(表头)里找到名为 header 的列号 →
//   ④ 从第 2 行到末行逐格读出字符串追加到 *values。找不到该列则直接返回（values 不变）。
static void readXlsxCol(const QString& path, const QString& sheet, const QString& header,
                        QStringList* values) {
    QXlsx::Document doc(path);
    if (!doc.selectSheet(sheet))
        return;
    QXlsx::CellRange dim = doc.dimension();  // 数据矩形区域
    int maxCol = dim.lastColumn();
    int maxRow = dim.lastRow();
    int targetCol = -1;
    for (int c = 1; c <= maxCol; ++c) {  // 在表头行定位目标列
        if (doc.read(1, c).toString() == header) {
            targetCol = c;
            break;
        }
    }
    if (targetCol < 0)
        return;                        // 没有这一列
    for (int r = 2; r <= maxRow; ++r)  // 逐数据行读该列
        values->append(doc.read(r, targetCol).toString());
}

// ── ImportRunner —— 封装「打开库 → 内省 schema → 跑导入 / 单值查询」的小夹具 ──────
// 为什么独立成结构体：多数用例都要「跑一次导入再查库验证」，把这套样板收拢于此，
//   每个用例只需 runner.run(spec, xlsx) + runner.queryOne(sql) 两步，读起来贴近意图。
// 状态：dbPath 是被测库路径（用例在 newDb() 后赋值）；catalog 缓存内省到的表结构。
struct ImportRunner {
    QString dbPath;         // 被测 SQLite 库文件路径
    SchemaCatalog catalog;  // 由 SchemaIntrospector 填充的库结构目录

    // run —— 打开 dbPath、内省 schema、用给定 Profile 对 xlsxPath 跑一次导入。
    // 参数：spec 导入配置；xlsxPath 输入表；abortOnError 是否「遇错即整体中止」
    //   （true=严格模式：任一行出错则不写任何行；false=逐行韧性模式：跳过坏行、写好行）。
    // 返回：ImportResult（含 writtenRows 写入行数与 errors 错误列表）。
    // 每次都新建/移除独立连接，避免与其它 run/query 串用。
    dbridge::ImportResult run(const ProfileSpec& spec, const QString& xlsxPath,
                              bool abortOnError = true) {
        QString conn = QStringLiteral("run_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        db.open();

        SchemaIntrospector si;
        si.load(db, &catalog, nullptr);  // 读出表/列/主键等结构，供导入做类型校验与路由

        ImportService svc;
        dbridge::ImportOptions opts;
        opts.abortOnError = abortOnError;
        auto res = svc.run(spec, catalog, xlsxPath, opts, db);

        db.close();
        QSqlDatabase::removeDatabase(conn);
        return res;
    }

    // queryOne —— 执行一条 SELECT，返回第一行第一列的值（无结果则返回无效 QVariant）。
    // 用途：导入后回查「某行某列实际存了什么」，是本测试验证转换结果的主要手段。
    QVariant queryOne(const QString& sql) {
        QString conn = QStringLiteral("qry_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        db.open();
        QSqlQuery q(db);
        q.exec(sql);
        QVariant result;
        if (q.next())
            result = q.value(0);
        db.close();
        QSqlDatabase::removeDatabase(conn);
        return result;
    }
};

// ── Test class ────────────────────────────────────────────────────────────────
// 每个 `private slots:` 方法是一个用例；测试用一个临时目录 tmp_ 存放所有库/xlsx 文件，
// 测试结束随 QTemporaryDir 析构自动清理。newDb()/newXlsx() 在该目录下生成唯一文件名。

class TstTemporalImport : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;  // 测试期临时目录（析构时自动整目录删除，无需手动清理文件）

    // 在临时目录下造一个唯一的 .db 路径（不创建文件，仅给出路径，供 setupDb 建库用）。
    QString newDb() const {
        return tmp_.path() + '/' + makeUuid() + ".db";
    }
    // 在临时目录下造一个唯一的 .xlsx 路径（供 writeXlsx 写入用）。
    QString newXlsx() const {
        return tmp_.path() + '/' + makeUuid() + ".xlsx";
    }

   private slots:
    // initTestCase —— 整个测试类「只跑一次」的前置：确认临时目录可用。
    void initTestCase() {
        QVERIFY(tmp_.isValid());
    }

    // ── 8.3a: 主格式命中 → 按 dbFormat 序列化入库 ───────────────────────────────
    // GIVEN dateFormat = {excel "yyyy/M/d", db "yyyy-MM-dd"}，单元格字符串 "2025/3/14"。
    // WHEN  导入。
    // THEN  零错误、写 1 行，且库中 event_date == "2025-03-14"（已按 dbFormat 重格式化）。
    // 验证最常规路径：Excel 端格式解析成功后，以库端格式存储。
    void testPrimaryFormatSuccess() {
        ImportRunner runner;
        runner.dbPath = newDb();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {QStringLiteral("CREATE TABLE events (id INTEGER PRIMARY KEY, event_date TEXT)")})
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"temporal_test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"events","conflict":{"columns":["id"]},
            "dateFormat":{"excelFormat":"yyyy/M/d","dbFormat":"yyyy-MM-dd"},
            "columns":{
                "id":{"source":"ID","validators":["notNull","int"]},
                "event_date":{"source":"EventDate"}
            }
        })");

        QString xlsxPath = newXlsx();
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("ID"), QStringLiteral("EventDate")},
                          {{QVariant(1), QStringLiteral("2025/3/14")}}));

        auto res = runner.run(spec, xlsxPath);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 1);

        QVariant stored =
            runner.queryOne(QStringLiteral("SELECT event_date FROM events WHERE id=1"));
        QCOMPARE(stored.toString(), QStringLiteral("2025-03-14"));
    }

    // ── 8.3b: 主格式失败、后备格式救场 → 仍按 dbFormat 入库 ─────────────────────
    // GIVEN 列级 dateFormat：主 excelFormat "yyyy-MM-dd" 解析不了 "14/3/2025"，
    //   但 excelFormatFallback ["d/M/yyyy"] 能解析。
    // THEN  零错误、写 1 行，库中 event_date == "2025-03-14"。
    // 验证后备格式机制：主格式不匹配时依次尝试后备格式，任一命中即成功。
    void testFallbackRescues() {
        ImportRunner runner;
        runner.dbPath = newDb();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {QStringLiteral("CREATE TABLE events (id INTEGER PRIMARY KEY, event_date TEXT)")})
                .isEmpty(),
            "DB setup failed");

        // Per-column dateFormat: primary "yyyy-MM-dd" doesn't match "14/3/2025",
        // but fallback "d/M/yyyy" does.
        auto spec = parseProfile(R"({
            "profileName":"temporal_test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"events","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["notNull","int"]},
                "event_date":{
                    "source":"EventDate",
                    "dateFormat":{
                        "excelFormat":"yyyy-MM-dd",
                        "dbFormat":"yyyy-MM-dd",
                        "excelFormatFallback":["d/M/yyyy"]
                    }
                }
            }
        })");

        QString xlsxPath = newXlsx();
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("ID"), QStringLiteral("EventDate")},
                          {{QVariant(1), QStringLiteral("14/3/2025")}}));

        auto res = runner.run(spec, xlsxPath);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 1);

        QVariant stored =
            runner.queryOne(QStringLiteral("SELECT event_date FROM events WHERE id=1"));
        QCOMPARE(stored.toString(), QStringLiteral("2025-03-14"));
    }

    // ── 8.3c: 全部格式失败 → 坏行报 E_TIME_PARSE 并被跳过，好行照常写入 ─────────
    // GIVEN 三行：第 1、3 行是合法日期，第 2 行 "notadate" 无法解析；abortOnError=false。
    // THEN  错误列表含 E_TIME_PARSE；writtenRows==2；库中无 id=2，而 id=1/3 正确入库。
    // 验证「逐行韧性模式」：单行时间解析失败不污染整批，坏行被跳过、其余行仍成功。
    void testAllFormatsFail_RowSkipped_OthersSucceed() {
        ImportRunner runner;
        runner.dbPath = newDb();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {QStringLiteral("CREATE TABLE events (id INTEGER PRIMARY KEY, event_date TEXT)")})
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"temporal_test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"events","conflict":{"columns":["id"]},
            "dateFormat":{"excelFormat":"yyyy-MM-dd","dbFormat":"yyyy-MM-dd"},
            "columns":{
                "id":{"source":"ID","validators":["notNull","int"]},
                "event_date":{"source":"EventDate"}
            }
        })");

        QString xlsxPath = newXlsx();
        // Row 1: valid → imported; Row 2: "notadate" fails → E_TIME_PARSE, skipped;
        // Row 3: valid → imported
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("ID"), QStringLiteral("EventDate")},
                          {
                              {QVariant(1), QStringLiteral("2025-03-14")},
                              {QVariant(2), QStringLiteral("notadate")},
                              {QVariant(3), QStringLiteral("2025-03-15")},
                          }));

        // C-2: use abortOnError=false (row-resilient mode, per time-format OpenSpec) so that a
        // row with E_TIME_PARSE is skipped while other valid rows are still written.
        auto res = runner.run(spec, xlsxPath, /*abortOnError=*/false);

        // E_TIME_PARSE must be emitted for the failing row
        QVERIFY2(hasCode(res, QStringLiteral("E_TIME_PARSE")), "Expected E_TIME_PARSE");

        // Other rows must still be written
        QCOMPARE(res.writtenRows, 2);

        // Row 2 must not exist in DB
        QVariant d2 = runner.queryOne(QStringLiteral("SELECT event_date FROM events WHERE id=2"));
        QVERIFY(d2.isNull());

        // Rows 1 and 3 must be in DB with correct dbFormat values
        QVariant d1 = runner.queryOne(QStringLiteral("SELECT event_date FROM events WHERE id=1"));
        QVariant d3 = runner.queryOne(QStringLiteral("SELECT event_date FROM events WHERE id=3"));
        QCOMPARE(d1.toString(), QStringLiteral("2025-03-14"));
        QCOMPARE(d3.toString(), QStringLiteral("2025-03-15"));
    }

    // ── 8.3d: xlsx 中的原生 QDate 单元格 → 绕过 excelFormat 解析，直接按 dbFormat 序列化 ──
    // 关键设计：当单元格本身就是「日期类型值」(而非日期字符串) 时，无需也不应再用
    //   excelFormat 去「解析字符串」(QDate 不是字符串，套字符串格式会失败)；应直接进入
    //   dbFormat 序列化。本用例特意让 dbFormat "yyyyMMdd" 与 excelFormat "yyyy/M/d" 不同，
    //   从存进库的值是 "20250314" 而非 "2025/3/14" 即可证明「确实走了 dbFormat、绕过了 excel
    //   解析」。
    void testNativeQDateCell_BypassesExcelFormat() {
        ImportRunner runner;
        runner.dbPath = newDb();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {QStringLiteral("CREATE TABLE events (id INTEGER PRIMARY KEY, event_date TEXT)")})
                .isEmpty(),
            "DB setup failed");

        // dbFormat "yyyyMMdd" differs from excelFormat "yyyy/M/d" to make bypass visible.
        // If excelFormat were applied to a native QDate, it would fail (QDate isn't a string).
        // With bypass, the QDate goes straight to dbFormat serialization.
        auto spec = parseProfile(R"({
            "profileName":"temporal_test","sheet":"S","headerRow":1,"mode":"singleTable",
            "table":"events","conflict":{"columns":["id"]},
            "dateFormat":{"excelFormat":"yyyy/M/d","dbFormat":"yyyyMMdd"},
            "columns":{
                "id":{"source":"ID","validators":["notNull","int"]},
                "event_date":{"source":"EventDate"}
            }
        })");

        QString xlsxPath = newXlsx();
        // Write a native QDate value (QXlsx stores it as a numeric date cell)
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("ID"), QStringLiteral("EventDate")},
                          {{QVariant(1), QVariant(QDate(2025, 3, 14))}}));

        auto res = runner.run(spec, xlsxPath);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 1);

        // DB must have "20250314" (dbFormat "yyyyMMdd"), not "2025/3/14" (excelFormat)
        QVariant stored =
            runner.queryOne(QStringLiteral("SELECT event_date FROM events WHERE id=1"));
        QCOMPARE(stored.toString(), QStringLiteral("20250314"));
    }

    // ── 7.3.2: epochSec 导入 — Excel 日期时间字符串 → 库存 qlonglong（秒）──────────
    // GIVEN datetimeFormat：excel {string, "yyyy-MM-dd HH:mm:ss"} → db {epochSec}；
    //   单元格 "2024-05-21 10:00:00"。
    // THEN  零错误、写 1 行；库中 happen_at 是一个正的 qlonglong，且
    //   fromSecsSinceEpoch(它) 还原回原始 QDateTime。
    // 验证 epochSec 目标类型：字符串日期时间被解析后存为「自纪元起的秒数」整数。
    void testEpochSecImportStringCell() {
        ImportRunner runner;
        runner.dbPath = newDb();
        QString xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {QStringLiteral(
                    "CREATE TABLE events (id INTEGER PRIMARY KEY, happen_at INTEGER NOT NULL)")})
                .isEmpty(),
            "DB setup failed");

        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("ID"), QStringLiteral("HappenAt")},
                          {{QVariant(1), QVariant(QStringLiteral("2024-05-21 10:00:00"))}}));

        auto spec = parseProfile(R"({
            "profileName":"ep","sheet":"S","headerRow":1,
            "mode":"singleTable","table":"events","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "happen_at":{"source":"HappenAt",
                             "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                                              "db":{"type":"epochSec"}}}
            }
        })");

        auto res = runner.run(spec, xlsxPath);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 1);

        QVariant stored =
            runner.queryOne(QStringLiteral("SELECT happen_at FROM events WHERE id=1"));
        QVERIFY(stored.isValid());
        bool ok = false;
        qlonglong secs = stored.toLongLong(&ok);
        QVERIFY2(ok, "stored value must be qlonglong");
        QVERIFY2(secs > 0, "epoch seconds must be positive");
        QDateTime expected = QDateTime::fromString(QStringLiteral("2024-05-21 10:00:00"),
                                                   QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        QCOMPARE(QDateTime::fromSecsSinceEpoch(secs), expected);
    }

    // ── 7.3.3: epochSec 导入 — 原生 QDateTime 单元格绕过 excel 字符串解析 ───────────
    // 同 8.3d 的「绕过」思想，但目标是 epochSec：单元格本身是 QDateTime 值，应直接转成
    //   epoch 秒，而非先套字符串格式解析。断言 fromSecsSinceEpoch(存值) == 原始 cellDt。
    void testEpochSecImportNativeQDateTimeCell() {
        ImportRunner runner;
        runner.dbPath = newDb();
        QString xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {QStringLiteral(
                    "CREATE TABLE events (id INTEGER PRIMARY KEY, happen_at INTEGER NOT NULL)")})
                .isEmpty(),
            "DB setup failed");

        QDateTime cellDt = QDateTime(QDate(2024, 5, 21), QTime(10, 0, 0));
        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("ID"), QStringLiteral("HappenAt")},
                          {{QVariant(1), QVariant(cellDt)}}));

        auto spec = parseProfile(R"({
            "profileName":"ep","sheet":"S","headerRow":1,
            "mode":"singleTable","table":"events","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "happen_at":{"source":"HappenAt",
                             "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                                              "db":{"type":"epochSec"}}}
            }
        })");

        auto res = runner.run(spec, xlsxPath);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 1);

        QVariant stored =
            runner.queryOne(QStringLiteral("SELECT happen_at FROM events WHERE id=1"));
        QVERIFY(stored.isValid());
        bool ok = false;
        qlonglong secs = stored.toLongLong(&ok);
        QVERIFY2(ok, "stored value must be qlonglong");
        QCOMPARE(QDateTime::fromSecsSinceEpoch(secs), cellDt);
    }

    // ── 7.3.4: epochSec 边界 — 纪元零点 "1970-01-01 00:00:00" 往返一致 ──────────────
    // 边界用例：纪元起点（epoch 秒理论上应为 0，但受本地/UTC 时区影响其整数值可能非 0），
    //   关键断言是 fromSecsSinceEpoch(存值) 能精确还原回该时刻，验证零点附近无 off-by-one
    //   或时区错位。注意本用例不直接断言 secs==0，而是断言「往返相等」，对时区更稳健。
    void testEpochSecImportEpochZero() {
        ImportRunner runner;
        runner.dbPath = newDb();
        QString xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {QStringLiteral(
                    "CREATE TABLE events (id INTEGER PRIMARY KEY, happen_at INTEGER NOT NULL)")})
                .isEmpty(),
            "DB setup failed");

        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("ID"), QStringLiteral("HappenAt")},
                          {{QVariant(1), QVariant(QStringLiteral("1970-01-01 00:00:00"))}}));

        auto spec = parseProfile(R"({
            "profileName":"ep","sheet":"S","headerRow":1,
            "mode":"singleTable","table":"events","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "happen_at":{"source":"HappenAt",
                             "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                                              "db":{"type":"epochSec"}}}
            }
        })");

        auto res = runner.run(spec, xlsxPath);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());

        QVariant stored =
            runner.queryOne(QStringLiteral("SELECT happen_at FROM events WHERE id=1"));
        QVERIFY(stored.isValid());
        bool ok = false;
        qlonglong secs = stored.toLongLong(&ok);
        QVERIFY2(ok, "stored value must be qlonglong");
        QDateTime expected = QDateTime::fromString(QStringLiteral("1970-01-01 00:00:00"),
                                                   QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        QCOMPARE(QDateTime::fromSecsSinceEpoch(secs), expected);
    }

    // ── 7.3.5: epochSec 导入失败 — 不可解析的字符串 → E_TIME_PARSE ─────────────────
    // GIVEN 单元格 "not-a-date"，目标 epochSec。
    // THEN  错误列表非空且含 E_TIME_PARSE。
    // 验证 epochSec 路径同样有健全的解析失败上报（与纯日期路径 8.3c 的错误语义一致）。
    void testEpochSecImportParseFailure() {
        ImportRunner runner;
        runner.dbPath = newDb();
        QString xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {QStringLiteral("CREATE TABLE events (id INTEGER PRIMARY KEY, happen_at INTEGER)")})
                .isEmpty(),
            "DB setup failed");

        QVERIFY(writeXlsx(xlsxPath, QStringLiteral("S"),
                          {QStringLiteral("ID"), QStringLiteral("HappenAt")},
                          {{QVariant(1), QVariant(QStringLiteral("not-a-date"))}}));

        auto spec = parseProfile(R"({
            "profileName":"ep","sheet":"S","headerRow":1,
            "mode":"singleTable","table":"events","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "happen_at":{"source":"HappenAt",
                             "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                                              "db":{"type":"epochSec"}}}
            }
        })");

        auto res = runner.run(spec, xlsxPath);
        QVERIFY2(!res.errors.isEmpty(), "Expected E_TIME_PARSE");
        bool hasTimeParseError = false;
        for (const auto& e : res.errors)
            if (e.code == QStringLiteral("E_TIME_PARSE"))
                hasTimeParseError = true;
        QVERIFY2(hasTimeParseError, "Expected E_TIME_PARSE error code");
    }

    // ── 7.6.1: 端到端往返 — 导入(字符串→epoch) → 库存 qlonglong → 导出 → 与原输入逐行一致 ─
    // 这是最综合的用例，串起导入与导出两个方向：
    //   ① 写含 3 个日期时间字符串的 xlsx → 导入(db.type=epochSec) → 库中三行均为 qlonglong；
    //   ② 逐行校验 fromSecsSinceEpoch(库值) 等于原始时刻；
    //   ③ 再用 ExportService 把库导出成新 xlsx（同一 Profile，excel 端仍是字符串格式）；
    //   ④ 读回导出文件的 HappenAt 列，要求与最初的输入字符串「逐行完全相同」。
    // 不变量：字符串 → epoch 秒 → 字符串 的整条往返是无损的（含纪元零点这一行）。
    void testEpochRoundtripE2E() {
        ImportRunner runner;
        runner.dbPath = newDb();
        QString inXlsx = newXlsx();
        QString outXlsx = newXlsx();

        QStringList inputDatetimes = {
            QStringLiteral("2024-05-21 10:00:00"),
            QStringLiteral("2024-06-01 00:00:00"),
            QStringLiteral("1970-01-01 00:00:00"),
        };

        QVERIFY2(setupDb(runner.dbPath,
                         {QStringLiteral("CREATE TABLE epoch_event (event_id INTEGER PRIMARY KEY,"
                                         " happen_at INTEGER NOT NULL)")})
                     .isEmpty(),
                 "DB setup failed");

        QVector<QVector<QVariant>> rows;
        for (int i = 0; i < inputDatetimes.size(); ++i)
            rows.append({QVariant(i + 1), QVariant(inputDatetimes[i])});
        QVERIFY(writeXlsx(inXlsx, QStringLiteral("EpochEvents"),
                          {QStringLiteral("EventID"), QStringLiteral("HappenAt")}, rows));

        auto spec = parseProfile(R"({
            "profileName":"epoch_time","sheet":"EpochEvents","headerRow":1,
            "mode":"singleTable","table":"epoch_event","conflict":{"columns":["event_id"]},
            "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                              "db":{"type":"epochSec"}},
            "columns":{
                "event_id":{"source":"EventID","validators":["notNull","int"]},
                "happen_at":{"source":"HappenAt"}
            },
            "export":{"orderBy":["event_id"]}
        })");

        // Import
        auto impRes = runner.run(spec, inXlsx);
        QVERIFY2(impRes.errors.isEmpty(),
                 impRes.errors.isEmpty() ? "" : impRes.errors[0].message.toUtf8());
        QCOMPARE(impRes.writtenRows, 3);

        // Verify DB stores qlonglong
        for (int i = 0; i < inputDatetimes.size(); ++i) {
            QVariant stored = runner.queryOne(QStringLiteral("SELECT happen_at FROM epoch_event"
                                                             " WHERE event_id=") +
                                              QString::number(i + 1));
            QVERIFY(stored.isValid());
            bool ok = false;
            qlonglong secs = stored.toLongLong(&ok);
            QVERIFY2(ok, ("event_id=" + QString::number(i + 1) + ": expected qlonglong").toUtf8());
            QDateTime expected =
                QDateTime::fromString(inputDatetimes[i], QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            QCOMPARE(QDateTime::fromSecsSinceEpoch(secs), expected);
        }

        // Export back
        {
            QString conn = QStringLiteral("exp_") + makeUuid();
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
            db.setDatabaseName(runner.dbPath);
            db.open();
            SchemaCatalog cat;
            SchemaIntrospector si;
            si.load(db, &cat, nullptr);
            ExportService svc;
            dbridge::ExportOptions opts;
            auto expRes = svc.run(spec, cat, outXlsx, opts, db);
            db.close();
            QSqlDatabase::removeDatabase(conn);
            QVERIFY2(expRes.errors.isEmpty(),
                     expRes.errors.isEmpty() ? "" : expRes.errors[0].message.toUtf8());
            QCOMPARE(expRes.writtenRows, 3);
        }

        // Roundtrip: exported HappenAt strings must match original input
        QStringList exportedDatetimes;
        readXlsxCol(outXlsx, QStringLiteral("EpochEvents"), QStringLiteral("HappenAt"),
                    &exportedDatetimes);
        QCOMPARE(exportedDatetimes.size(), inputDatetimes.size());
        for (int i = 0; i < inputDatetimes.size(); ++i)
            QCOMPARE(exportedDatetimes[i], inputDatetimes[i]);
    }
};

QTEST_MAIN(TstTemporalImport)
#include "tst_temporal_import.moc"
