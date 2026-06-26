// Integration tests for import-direction temporal format conversion (tasks.md 8.3).
//
// Test strategy: Real SQLite temp-file DB + real QXlsx xlsx files.
// Run ImportService.run(), then query DB to verify stored values.

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
    Q_ASSERT_X(loader.load(QJsonDocument::fromJson(json.toUtf8()), &spec, &err), "parseProfile",
               err.toUtf8());
    return spec;
}

static bool hasCode(const dbridge::ImportResult& r, const QString& code) {
    for (const auto& e : r.errors)
        if (e.code == code)
            return true;
    return false;
}

static void readXlsxCol(const QString& path, const QString& sheet, const QString& header,
                        QStringList* values) {
    QXlsx::Document doc(path);
    if (!doc.selectSheet(sheet))
        return;
    QXlsx::CellRange dim = doc.dimension();
    int maxCol = dim.lastColumn();
    int maxRow = dim.lastRow();
    int targetCol = -1;
    for (int c = 1; c <= maxCol; ++c) {
        if (doc.read(1, c).toString() == header) {
            targetCol = c;
            break;
        }
    }
    if (targetCol < 0)
        return;
    for (int r = 2; r <= maxRow; ++r)
        values->append(doc.read(r, targetCol).toString());
}

struct ImportRunner {
    QString dbPath;
    SchemaCatalog catalog;

    dbridge::ImportResult run(const ProfileSpec& spec, const QString& xlsxPath,
                              bool abortOnError = true) {
        QString conn = QStringLiteral("run_") + makeUuid();
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        db.open();

        SchemaIntrospector si;
        si.load(db, &catalog, nullptr);

        ImportService svc;
        dbridge::ImportOptions opts;
        opts.abortOnError = abortOnError;
        auto res = svc.run(spec, catalog, xlsxPath, opts, db);

        db.close();
        QSqlDatabase::removeDatabase(conn);
        return res;
    }

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

class TstTemporalImport : public QObject {
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

    // 8.3a: String cell matches primary excelFormat → serialized via dbFormat in DB
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

    // 8.3b: Primary excelFormat fails, fallback rescues → serialized via dbFormat
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

    // 8.3c: All formats fail → E_TIME_PARSE on failing row; other rows still succeed
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

    // 8.3d: Native QDate cell in xlsx → bypass excelFormat parsing, serialize via dbFormat
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

    // 7.3.2: epochSec import — Excel string "2024-05-21 10:00:00" → DB qlonglong
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

    // 7.3.3: epochSec import — native QDateTime cell in xlsx bypasses excel string parse
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

    // 7.3.4: epochSec import boundary — Excel "1970-01-01 00:00:00" → DB qlonglong roundtrip
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

    // 7.3.5: epochSec import failure — unparseable Excel string → E_TIME_PARSE
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

    // 7.6.1: e2e — import EpochEvents (string→epoch) → DB qlonglong → export → roundtrip match
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
