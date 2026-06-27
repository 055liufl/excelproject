#include "dbridge/Errors.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/schema/SchemaEligibility.h"

using namespace dbridge::sync;

class TstSyncSchemaEligibility : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

    void exec(const QString& sql) {
        QSqlQuery q(db_);
        if (!q.exec(sql))
            qWarning() << "exec failed:" << q.lastError().text() << "| SQL:" << sql;
    }

   private slots:
    void init() {
        conn_ =
            QStringLiteral("tst_se_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
    }
    void cleanup() {
        db_ = QSqlDatabase();
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // --- eligible cases ---
    void verify_rowidExplicitSinglePk_eligible() {
        exec("CREATE TABLE t_ok (id INTEGER PRIMARY KEY NOT NULL, name TEXT)");
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"t_ok"}, &rejected, &err);
        QVERIFY2(ok, qPrintable("Rejected: " + rejected.join(',') + " err: " + err));
        QVERIFY(rejected.isEmpty());
    }

    void verify_withoutRowid_eligible() {
        exec("CREATE TABLE t_wr (id INTEGER NOT NULL, name TEXT, PRIMARY KEY(id)) WITHOUT ROWID");
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"t_wr"}, &rejected, &err);
        QVERIFY2(ok, qPrintable("Rejected: " + rejected.join(',') + " err: " + err));
    }

    // --- rejected: no explicit PK ---
    void verify_noExplicitPk_rejected() {
        exec("CREATE TABLE t_nopk (name TEXT, val INTEGER)");
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"t_nopk"}, &rejected, &err);
        QVERIFY(!ok);
        QVERIFY([&]() {
            for (auto& r : rejected)
                if (r.startsWith("t_nopk"))
                    return true;
            return false;
        }());
        // err is empty for reject-path; error is conveyed via rejected entries, not *err
    }

    // --- rejected: composite PK (MVP limitation) ---
    void verify_compositePk_rejected() {
        exec(
            "CREATE TABLE t_cpk (a INTEGER NOT NULL, b INTEGER NOT NULL, "
            "val TEXT, PRIMARY KEY(a,b))");
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"t_cpk"}, &rejected, &err);
        QVERIFY(!ok);
        QVERIFY([&]() {
            for (auto& r : rejected)
                if (r.startsWith("t_cpk"))
                    return true;
            return false;
        }());
        // E_SYNC_COMPOSITE_PK_NOT_SUPPORTED is embedded in the rejected entry text, not *err
        bool hasCode = false;
        for (const auto& r : rejected)
            if (r.contains(QLatin1String(dbridge::err::E_SYNC_COMPOSITE_PK_NOT_SUPPORTED))) {
                hasCode = true;
                break;
            }
        QVERIFY(hasCode);
    }

    // --- rejected: view ---
    void verify_view_rejected() {
        exec("CREATE TABLE base_t (id INTEGER PRIMARY KEY)");
        exec("CREATE VIEW v_ok AS SELECT id FROM base_t");
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"v_ok"}, &rejected, &err);
        QVERIFY(!ok);
        QVERIFY([&]() {
            for (auto& r : rejected)
                if (r.startsWith("v_ok"))
                    return true;
            return false;
        }());
    }

    // --- partial rejection ---
    void verify_mixedSet_partialRejection() {
        exec("CREATE TABLE t_good (id INTEGER PRIMARY KEY NOT NULL)");
        exec("CREATE TABLE t_bad  (x TEXT, y TEXT)");  // no PK
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"t_good", "t_bad"}, &rejected, &err);
        QVERIFY(!ok);
        QVERIFY(![&]() {
            for (auto& r : rejected)
                if (r.startsWith("t_good"))
                    return true;
            return false;
        }());
        QVERIFY([&]() {
            for (auto& r : rejected)
                if (r.startsWith("t_bad"))
                    return true;
            return false;
        }());
    }

    // --- generated column: table not rejected, column just excluded ---
    void verify_generatedColumn_tableNotRejected() {
        exec(
            "CREATE TABLE t_gen (id INTEGER PRIMARY KEY NOT NULL, "
            "name TEXT, upper_name TEXT GENERATED ALWAYS AS (UPPER(name)) VIRTUAL)");
        QStringList rejected;
        QString err;
        SchemaEligibility::verify(db_, {"t_gen"}, &rejected, &err);
        // table_gen must NOT be in rejected (generated column only excluded, not blocking)
        QVERIFY(![&]() {
            for (auto& r : rejected)
                if (r.startsWith("t_gen"))
                    return true;
            return false;
        }());
    }

    // --- expandSyncTables: empty list expands to all user tables ---
    void expandSyncTables_empty_returnsUserTables() {
        exec("CREATE TABLE user1 (id INTEGER PRIMARY KEY)");
        exec("CREATE TABLE user2 (id INTEGER PRIMARY KEY)");
        QString err;
        QStringList tables = SchemaEligibility::expandSyncTables(db_, {}, &err);
        QVERIFY(tables.contains("user1"));
        QVERIFY(tables.contains("user2"));
        for (const QString& t : tables)
            QVERIFY(!t.startsWith("sqlite_"));
    }

    void expandSyncTables_explicit_passthrough() {
        exec("CREATE TABLE ua (id INTEGER PRIMARY KEY)");
        exec("CREATE TABLE ub (id INTEGER PRIMARY KEY)");
        QString err;
        QStringList tables = SchemaEligibility::expandSyncTables(db_, {"ua"}, &err);
        QCOMPARE(tables.size(), 1);
        QCOMPARE(tables[0], QString("ua"));
    }

    // --- non-existent table in syncTables → rejected ---
    void verify_missingTable_rejected() {
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"no_such_table"}, &rejected, &err);
        QVERIFY(!ok);
        QVERIFY([&]() {
            for (auto& r : rejected)
                if (r.startsWith("no_such_table"))
                    return true;
            return false;
        }());
    }

    // --- FTS5 virtual table → rejected ---
    void verify_fts5Virtual_rejected() {
        exec("CREATE VIRTUAL TABLE t_fts USING fts5(content)");
        QStringList rejected;
        QString err;
        bool ok = SchemaEligibility::verify(db_, {"t_fts"}, &rejected, &err);
        QVERIFY(!ok);
        QVERIFY([&]() {
            for (auto& r : rejected)
                if (r.startsWith("t_fts"))
                    return true;
            return false;
        }());
    }
};

QTEST_APPLESS_MAIN(TstSyncSchemaEligibility)
#include "tst_sync_schema_eligibility.moc"
