// Integration tests for export-direction temporal format conversion (tasks.md 8.4).
//
// Test strategy: Real SQLite temp-file DB. Run ExportService.run() against a temp xlsx path,
// then read the xlsx back with QXlsx::Document to verify cell values.

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

static bool hasCode(const dbridge::ExportResult& r, const QString& code) {
    for (const auto& e : r.errors)
        if (e.code == code)
            return true;
    return false;
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

class TstTemporalExport : public QObject {
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

    // 8.4a: DB string parsed via dbFormat (V) then serialized via excelFormat (U)
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

    // ── epochSec export tests ──────────────────────────────────────────────────

    // 7.4.2: epochSec export — DB qlonglong → Excel string via excel.format
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

QTEST_MAIN(TstTemporalExport)
#include "tst_temporal_export.moc"
