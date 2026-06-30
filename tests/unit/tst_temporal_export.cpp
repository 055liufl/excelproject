// ============================================================================
// tst_temporal_export.cpp — 导出方向时间格式转换的「集成测试」(tasks.md 8.4 / 7.4.x)
// ============================================================================
//
// Integration tests for export-direction temporal format conversion (tasks.md 8.4).
//
// Test strategy: Real SQLite temp-file DB. Run ExportService.run() against a temp xlsx path,
// then read the xlsx back with QXlsx::Document to verify cell values.
// 【译 + 扩展】导出方向时间格式转换的集成测试（对应 tasks.md 第 8.4 / 7.4.x 项）。
//
// 【这些测试在验证什么】
//   导出（DB → Excel）时，时间列要经历「按 DB 侧规格解析(V) → 按 Excel 侧规格序列化(U)」两步
//   （这正是 TemporalConvert/tconv 的职责，本测试从 ExportService 端到端地驱动它）。覆盖：
//     · 字符串↔字符串：DB 存 "2025-03-14"（dbFormat）→ Excel 写 "2025/3/14"（excelFormat）。
//     · 纪元秒(epochSec)：DB 存整数秒 → Excel 写格式化时间串；含 0、负数、非数值失败等边界。
//     · 容错语义：单个时间格解析失败 → 该格写 NULL（空单元格）+ 报 E_TIME_PARSE_DB，但【整行
//       仍照常导出】（导出是「宽容」的：坏一格不毁整行）；DB 为 NULL → 空单元格、且【不报错】。
//
// 【为什么是集成测试而非单元测试】
//   它不直接调 tconv 的某个函数，而是真打开 SQLite 临时库、灌入数据、跑完整 ExportService.run()
//   产出真实 .xlsx，再用 QXlsx 把文件读回逐格校验——验证的是「整条导出链 + 时间转换」协同后的
//   最终落盘结果，最贴近用户实际所见。
//
// 【测试策略（与文件头英文一致）】真实的 SQLite 临时文件库（QTemporaryDir 管理生命周期）；
//   对临时 xlsx 路径跑 ExportService.run()；再用 QXlsx::Document 读回单元格值做断言。
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

// ── Shared helpers —— 各测试用例共用的夹具/工具函数 ────────────────────────────

// makeUuid —— 生成去除花括号与连字符的纯十六进制 UUID 串。
// 用途：给每次临时数据库连接名 / 临时文件名拼一个全局唯一后缀，避免连接重名与文件互撞。
static QString makeUuid() {
    return QUuid::createUuid().toString().remove('{').remove('}').remove('-');
}

// setupDb —— 在指定库文件 path 上依次执行一组建表/插数据 SQL，准备好测试数据。
// 做什么：用唯一连接名打开（不存在则创建）SQLite 文件，逐条执行 sqls，全程出错即提前返回错误文本。
// 返回：空串表示全部成功；非空串为「open failed: ...」或「<出错的SQL> failed: ...」的诊断信息。
// 副作用：建/改 path 指向的库；结束后 close + removeDatabase，不残留连接。
// 错误模式：open 失败 / 任一 SQL 执行失败。调用方一般用 QVERIFY2(....isEmpty(), "DB setup
// failed")。
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

// parseProfile —— 把内联的 JSON 字符串解析成 ProfileSpec（ETL 映射配置）。
// 为什么用 qFatal：测试里 profile JSON 是写死的常量，解析失败属「测试自身写错了」的致命问题，
//   直接中止比返回错误更利于定位（不应进入后续断言）。
// 返回：解析好的 ProfileSpec（成功时）。
static ProfileSpec parseProfile(const QString& json) {
    ProfileLoader loader;
    ProfileSpec spec;
    QString err;
    if (!loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err))
        qFatal("%s failed: %s", "parseProfile", err.toUtf8().constData());
    return spec;
}

// hasCode —— 在导出结果的 errors 列表里查找是否出现了某个错误码。
// 用途：断言「确实报告了 E_TIME_PARSE_DB」这类「应报错但非致命」的情形。返回 true=出现过该码。
static bool hasCode(const dbridge::ExportResult& r, const QString& code) {
    for (const auto& e : r.errors)
        if (e.code == code)
            return true;
    return false;
}

// readXlsxSheet —— 用 QXlsx 把导出的 .xlsx 读回内存：第 1 行作表头，其余行作数据。
// 做什么：打开 path、选中 sheet，按 dimension() 得到的有效区域，把第 1 行各列文本收进 headers，
//   第 2 行起每行各列文本收进 rows（全部 toString，便于直接与期望字符串 QCOMPARE）。
// 参数：headers/rows 为出参（追加写入）。sheet 选不中则直接返回（headers/rows 保持空）。
// 为什么读回文件而非信任 writtenRows：本测试要验证「单元格里到底写了什么文本」，必须真读盘。
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

// ExportRunner —— 一次导出的「执行夹具」：把 dbPath + xlsxPath 打包，run() 跑完整条导出链。
// 为什么独立成结构体：每个用例都要「打开库 → 内省 schema → 跑 ExportService → 关库清连接」，
//   抽成 run() 复用，用例只需准备数据与 profile、再校验产出的 xlsx。
struct ExportRunner {
    QString dbPath;         // 待导出的源 SQLite 库路径
    QString xlsxPath;       // 导出目标 .xlsx 路径
    SchemaCatalog catalog;  // 由 SchemaIntrospector 填充的表结构目录

    // run —— 打开 dbPath、内省出 schema catalog、用给定 profile 跑 ExportService.run() 产出 xlsx。
    // 返回：ExportResult（errors / writtenRows 等），供用例断言。副作用：写 xlsxPath 文件。
    dbridge::ExportResult run(const ProfileSpec& spec) {
        QString conn = QStringLiteral("run_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        db.open();

        // 内省：从真实库读出列类型/约束，供导出时定位列与判断时间列种类。
        SchemaIntrospector si;
        si.load(db, &catalog, nullptr);

        ExportService svc;
        dbridge::ExportOptions opts;
        auto res = svc.run(spec, catalog, xlsxPath, opts, db);

        db.close();
        QSqlDatabase::removeDatabase(conn);  // 清理连接，避免与下一个用例重名冲突
        return res;
    }
};

// ── Test class —— 时间导出集成测试 ──────────────────────────────────────────────

class TstTemporalExport : public QObject {
    Q_OBJECT

    QTemporaryDir tmp_;  // 进程级临时目录，析构时自动整目录删除（库文件/xlsx 都建在这里）

    // newDb / newXlsx —— 在临时目录里生成唯一的库 / xlsx 路径，避免用例之间互相覆盖文件。
    QString newDb() const {
        return tmp_.path() + '/' + makeUuid() + ".db";
    }
    QString newXlsx() const {
        return tmp_.path() + '/' + makeUuid() + ".xlsx";
    }

   private slots:
    // initTestCase —— 全类只跑一次的前置：确认临时目录创建成功（否则后续全废）。
    void initTestCase() {
        QVERIFY(tmp_.isValid());
    }

    // 8.4a: DB string parsed via dbFormat (V) then serialized via excelFormat (U)
    // 【契约 8.4a】DB 端字符串 → 按 dbFormat 解析(V) → 按 excelFormat 序列化(U) → 写入 Excel。
    // GIVEN events.event_date 存 "2025-03-14"，dbFormat="yyyy-MM-dd"、excelFormat="yyyy/M/d"
    // WHEN  导出
    // THEN  无错误、写 1 行，且 EventDate 列单元格 == "2025/3/14"（完成了一次格式转换）。
    void testDbStringToExcelFormat() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {
                    QStringLiteral("CREATE TABLE events (id INTEGER PRIMARY KEY, event_date TEXT)"),
                    QStringLiteral("INSERT INTO events VALUES (1, '2025-03-14')"),
                })
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"temporal_export_test","sheet":"S","headerRow":1,
            "mode":"singleTable","table":"events","conflict":{"columns":["id"]},
            "dateFormat":{"excelFormat":"yyyy/M/d","dbFormat":"yyyy-MM-dd"},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "event_date":{"source":"EventDate"}
            },
            "export":{"orderBy":["id"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 1);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        int dateCol = headers.indexOf(QStringLiteral("EventDate"));
        QVERIFY(dateCol >= 0);
        // DB "2025-03-14" (V) → QDate → excelFormat "yyyy/M/d" → "2025/3/14"
        QCOMPARE(rows[0][dateCol], QStringLiteral("2025/3/14"));
    }

    // 8.4b: DB string fails V parse → E_TIME_PARSE_DB; that cell is NULL, rest of row written
    // 【契约 8.4b】某行的时间格按 dbFormat 解析失败时：报 E_TIME_PARSE_DB、该单元格写 NULL（空），
    //   但【整行其余列照常导出】——导出是宽容的，坏一格不毁整行。
    // GIVEN event_date 存 "14/03/2025"（不匹配 dbFormat "yyyy-MM-dd"），同表还有正常的 title 列
    // WHEN  导出
    // THEN  结果含 E_TIME_PARSE_DB；writtenRows 仍为 1；Title 列正确写出 "TestEvent"；EventDate
    // 为空。
    void testDbParseFailure_CellNull_RowContinues() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        // "14/03/2025" doesn't match dbFormat "yyyy-MM-dd"
        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral("CREATE TABLE events (id INTEGER PRIMARY KEY, title TEXT, "
                                       "event_date TEXT)"),
                        QStringLiteral("INSERT INTO events VALUES (1, 'TestEvent', '14/03/2025')"),
                    })
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"temporal_export_test","sheet":"S","headerRow":1,
            "mode":"singleTable","table":"events","conflict":{"columns":["id"]},
            "dateFormat":{"excelFormat":"yyyy/M/d","dbFormat":"yyyy-MM-dd"},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "title":{"source":"Title","validators":["notNull"]},
                "event_date":{"source":"EventDate"}
            },
            "export":{"orderBy":["id"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(hasCode(res, QStringLiteral("E_TIME_PARSE_DB")), "Expected E_TIME_PARSE_DB");
        // Export is lenient: row is still written (cell is NULL for the bad date)
        QCOMPARE(res.writtenRows, 1);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        int titleCol = headers.indexOf(QStringLiteral("Title"));
        int dateCol = headers.indexOf(QStringLiteral("EventDate"));
        QVERIFY(titleCol >= 0 && dateCol >= 0);

        // Other column (Title) is written correctly
        QCOMPARE(rows[0][titleCol], QStringLiteral("TestEvent"));
        // The bad date cell is empty (NULL written to xlsx)
        QVERIFY(rows[0][dateCol].isEmpty());
    }

    // 8.4c: DB NULL → empty Excel cell, no error emitted
    // 【契约 8.4c】DB 里时间列为 NULL（无值）→ Excel 写空单元格，且【不报任何错误】。
    // 与 8.4b 的关键区别：NULL 是「合法的无值」（用户没填），不是「解析失败」，故不应产生
    //   E_TIME_PARSE_DB。GIVEN event_date 为 NULL  THEN errors 为空、writtenRows=1、该格为空。
    void testDbNullWritesEmptyCell_NoError() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {
                    QStringLiteral("CREATE TABLE events (id INTEGER PRIMARY KEY, event_date TEXT)"),
                    QStringLiteral("INSERT INTO events VALUES (1, NULL)"),
                })
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"temporal_export_test","sheet":"S","headerRow":1,
            "mode":"singleTable","table":"events","conflict":{"columns":["id"]},
            "dateFormat":{"excelFormat":"yyyy/M/d","dbFormat":"yyyy-MM-dd"},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "event_date":{"source":"EventDate"}
            },
            "export":{"orderBy":["id"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 1);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        int dateCol = headers.indexOf(QStringLiteral("EventDate"));
        QVERIFY(dateCol >= 0);
        // NULL DB value → empty cell, no error
        QVERIFY(rows[0][dateCol].isEmpty());
    }

    // ── epochSec export tests —— 纪元秒（Unix 时间戳）导出系列 ────────────────────
    // 纪元秒模式：DB 端把时间存成「自 1970-01-01 起的整数秒」(db.type=epochSec)，导出时
    //   QDateTime::fromSecsSinceEpoch 还原为时刻，再按 excel.format 格式化成字符串写入 Excel。

    // 7.4.2: epochSec export — DB qlonglong → Excel string via excel.format
    // 【契约 7.4.2】DB 整数秒 → 按 excel.format "yyyy-MM-dd HH:mm:ss" 序列化为字符串。
    // 这里先用 QDateTime 把 "2024-05-21 10:00:00" 反算出本地时区下的秒数灌库（保证 roundtrip
    //   与本机时区一致），导出后应原样还原成该时间串。
    void testEpochSecExport_LongToString() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        // 1716286800 = 2024-05-21 10:00:00 UTC+8 (or local; we verify roundtrip)
        qlonglong epochSecs = QDateTime::fromString(QStringLiteral("2024-05-21 10:00:00"),
                                                    QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                                  .toSecsSinceEpoch();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {QStringLiteral(
                     "CREATE TABLE events (id INTEGER PRIMARY KEY, happen_at INTEGER NOT NULL)"),
                 QStringLiteral("INSERT INTO events VALUES (1, %1)").arg(epochSecs)})
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"ep","sheet":"S","headerRow":1,
            "mode":"singleTable","table":"events","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "happen_at":{"source":"HappenAt",
                             "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                                              "db":{"type":"epochSec"}}}
            },
            "export":{"orderBy":["id"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 1);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        int col = headers.indexOf(QStringLiteral("HappenAt"));
        QVERIFY(col >= 0);
        QCOMPARE(rows[0][col], QStringLiteral("2024-05-21 10:00:00"));
    }

    // 7.4.3: NULL vs 0 distinction
    // qlonglong(0) = 1970-01-01 00:00:00, not treated as empty
    // 【契约 7.4.3】区分「NULL（无值）」与「整数 0」：0 是合法时刻（纪元元年 1970-01-01
    // 00:00:00），
    //   绝不能被当成空而漏写。这正是 tconv::isEmptyForTemporal「数值 0 不算空」规则的端到端验证。
    // GIVEN happen_at 存整数 0  WHEN 导出  THEN 该格 == 本地时区下 epoch0
    // 的格式化串（非空、无错）。
    void testEpochSecExport_ZeroIsNotNull() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {QStringLiteral("CREATE TABLE events (id INTEGER PRIMARY KEY, happen_at INTEGER)"),
                 QStringLiteral("INSERT INTO events VALUES (1, 0)")})
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"ep","sheet":"S","headerRow":1,
            "mode":"singleTable","table":"events","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "happen_at":{"source":"HappenAt",
                             "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                                              "db":{"type":"epochSec"}}}
            },
            "export":{"orderBy":["id"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        int col = headers.indexOf(QStringLiteral("HappenAt"));
        QVERIFY(col >= 0);
        // qlonglong(0) → 1970-01-01 00:00:00 local time
        QDateTime epoch0 = QDateTime::fromSecsSinceEpoch(0);
        QCOMPARE(rows[0][col], epoch0.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    }

    // 7.4.4: epochSec negative — DB qlonglong(-86400) → valid date before 1970
    // 【契约 7.4.4】负纪元秒也是合法时刻（1970 之前）：-86400 秒 = 1970 年前一天。
    //   验证转换不把负数误判为非法/空，能正确还原为 1969-12-31 那天的日期串。
    void testEpochSecExport_NegativeEpoch() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(
                runner.dbPath,
                {QStringLiteral("CREATE TABLE events (id INTEGER PRIMARY KEY, happen_at INTEGER)"),
                 QStringLiteral("INSERT INTO events VALUES (1, -86400)")})
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"ep","sheet":"S","headerRow":1,
            "mode":"singleTable","table":"events","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "happen_at":{"source":"HappenAt",
                             "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd"},
                                              "db":{"type":"epochSec"}}}
            },
            "export":{"orderBy":["id"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        int col = headers.indexOf(QStringLiteral("HappenAt"));
        QVERIFY(col >= 0);
        QDateTime dt = QDateTime::fromSecsSinceEpoch(-86400);
        QCOMPARE(rows[0][col], dt.toString(QStringLiteral("yyyy-MM-dd")));
    }

    // 7.4.5: epochSec export failure — DB "not_a_number" with db.type=epochSec → E_TIME_PARSE_DB
    // 【契约 7.4.5】db.type=epochSec 却在库里存了非数值文本 "not_a_number"：无法解析为整数秒，
    //   报 E_TIME_PARSE_DB；但与 8.4b 同理「非致命」——该格写空、整行仍导出（writtenRows=1）。
    void testEpochSecExport_NonNumericDbValue() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {QStringLiteral("CREATE TABLE events (id INTEGER PRIMARY KEY, happen_at TEXT)"),
                     QStringLiteral("INSERT INTO events VALUES (1, 'not_a_number')")})
                .isEmpty(),
            "DB setup failed");

        auto spec = parseProfile(R"({
            "profileName":"ep","sheet":"S","headerRow":1,
            "mode":"singleTable","table":"events","conflict":{"columns":["id"]},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "happen_at":{"source":"HappenAt",
                             "datetimeFormat":{"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"},
                                              "db":{"type":"epochSec"}}}
            },
            "export":{"orderBy":["id"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(hasCode(res, QStringLiteral("E_TIME_PARSE_DB")), "Expected E_TIME_PARSE_DB");
        // Row is still written (non-fatal), cell is empty
        QCOMPARE(res.writtenRows, 1);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        int col = headers.indexOf(QStringLiteral("HappenAt"));
        QVERIFY(col >= 0);
        QVERIFY(rows[0][col].isEmpty());
    }

    // 8.4d: datetimeFormat round-trip: DB stores V-format string → export serializes to U-format
    // (SQLite always returns TEXT, so the "native QVariant::DateTime bypass" path is exercised
    // by other Qt SQL drivers; here we verify the common V→parse→U path for datetimes.)
    // 【契约 8.4d】DateTime（非仅 Date）的字符串 V→U 往返：DB 存 dbFormat 串 → 解析 → 按
    //   excelFormat 串序列化。
    // 原注释要点：SQLite 总是把列值以 TEXT 返回，所以「QVariant 本身就是 QDateTime、可跳过字符串
    //   解析」那条捷径由其它 Qt SQL 驱动触发；这里验证最常见的「V 解析 → U 序列化」DateTime 路径。
    // GIVEN event_datetime 存 "2025-03-14 09:30:00"（V="yyyy-MM-dd HH:mm:ss"），U="d/M/yyyy H:mm"
    // THEN  导出后该格 == "14/3/2025 9:30"（日/月/年 时:分，且无前导零）。
    void testDatetimeFormat_VtoU() {
        ExportRunner runner;
        runner.dbPath = newDb();
        runner.xlsxPath = newXlsx();

        QVERIFY2(
            setupDb(runner.dbPath,
                    {
                        QStringLiteral(
                            "CREATE TABLE events (id INTEGER PRIMARY KEY, event_datetime TEXT)"),
                        QStringLiteral("INSERT INTO events VALUES (1, '2025-03-14 09:30:00')"),
                    })
                .isEmpty(),
            "DB setup failed");

        // V = "yyyy-MM-dd HH:mm:ss", U = "d/M/yyyy H:mm"
        auto spec = parseProfile(R"({
            "profileName":"temporal_export_test","sheet":"S","headerRow":1,
            "mode":"singleTable","table":"events","conflict":{"columns":["id"]},
            "datetimeFormat":{"excelFormat":"d/M/yyyy H:mm","dbFormat":"yyyy-MM-dd HH:mm:ss"},
            "columns":{
                "id":{"source":"ID","validators":["int"]},
                "event_datetime":{"source":"EventDateTime"}
            },
            "export":{"orderBy":["id"]}
        })");

        auto res = runner.run(spec);
        QVERIFY2(res.errors.isEmpty(), res.errors.isEmpty() ? "" : res.errors[0].message.toUtf8());
        QCOMPARE(res.writtenRows, 1);

        QStringList headers;
        QVector<QStringList> rows;
        readXlsxSheet(runner.xlsxPath, QStringLiteral("S"), &headers, &rows);

        int dtCol = headers.indexOf(QStringLiteral("EventDateTime"));
        QVERIFY(dtCol >= 0);
        // DB "2025-03-14 09:30:00" → QDateTime → excelFormat "d/M/yyyy H:mm"
        QCOMPARE(rows[0][dtCol], QStringLiteral("14/3/2025 9:30"));
    }
};

// QTEST_MAIN（注意非 APPLESS）：本测试用 QXlsx，需要完整 QCoreApplication 事件循环支撑，
// 故用 QTEST_MAIN 而非 QTEST_APPLESS_MAIN。末行引入 moc 生成代码（Qt Test 单文件固定写法）。
QTEST_MAIN(TstTemporalExport)
#include "tst_temporal_export.moc"
