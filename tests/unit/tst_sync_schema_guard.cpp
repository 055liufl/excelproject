#include "dbridge/Errors.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/schema/SchemaGuard.h"

using namespace dbridge::sync;

class TstSyncSchemaGuard : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

   private slots:
    void init() {
        conn_ =
            QStringLiteral("tst_sg_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        q.exec(QStringLiteral("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"));
        q.exec(QStringLiteral("CREATE TABLE orders (id INTEGER PRIMARY KEY, total REAL)"));
    }
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    void verifyPayload_matchingVersionAndFp_passes() {
        SchemaGuard sg;
        sg.setLocal(3, QStringLiteral("fp_abc"));
        QString err;
        QVERIFY(sg.verifyPayload(3, QStringLiteral("fp_abc"), &err));
        QVERIFY(err.isEmpty());
    }

    void verifyPayload_versionMismatch_fails() {
        SchemaGuard sg;
        sg.setLocal(3, QStringLiteral("fp_abc"));
        QString err;
        QVERIFY(!sg.verifyPayload(4, QStringLiteral("fp_abc"), &err));
        QVERIFY(!err.isEmpty() && err.contains("mismatch"));
    }

    void verifyPayload_fingerprintMismatch_fails() {
        SchemaGuard sg(/*verifyFingerprint=*/true);
        sg.setLocal(3, QStringLiteral("fp_abc"));
        QString err;
        QVERIFY(!sg.verifyPayload(3, QStringLiteral("fp_XYZ"), &err));
        QVERIFY(!err.isEmpty() && err.contains("mismatch"));
    }

    void verifyPayload_fingerprintDisabled_onlyChecksVersion() {
        SchemaGuard sg(/*verifyFingerprint=*/false);
        sg.setLocal(3, QStringLiteral("fp_abc"));
        QString err;
        // fingerprint mismatch ignored when verifyFingerprint=false
        QVERIFY(sg.verifyPayload(3, QStringLiteral("fp_DIFFERENT"), &err));
        // but version mismatch still fails
        QVERIFY(!sg.verifyPayload(5, QStringLiteral("fp_DIFFERENT"), &err));
    }

    void computeFingerprint_nonEmpty() {
        QString fp = SchemaGuard::computeFingerprint(db_, {QStringLiteral("items")});
        QVERIFY(!fp.isEmpty());
    }

    void computeFingerprint_sameInput_same() {
        QString fp1 = SchemaGuard::computeFingerprint(db_, {QStringLiteral("items")});
        QString fp2 = SchemaGuard::computeFingerprint(db_, {QStringLiteral("items")});
        QCOMPARE(fp1, fp2);
    }

    void computeFingerprint_differentTables_different() {
        QString fp1 = SchemaGuard::computeFingerprint(db_, {QStringLiteral("items")});
        QString fp2 = SchemaGuard::computeFingerprint(db_, {QStringLiteral("orders")});
        QVERIFY(fp1 != fp2);
    }

    void computeFingerprint_tableOrderInsensitive() {
        // fingerprint should be the same regardless of table list order
        QString fp1 = SchemaGuard::computeFingerprint(
            db_, {QStringLiteral("items"), QStringLiteral("orders")});
        QString fp2 = SchemaGuard::computeFingerprint(
            db_, {QStringLiteral("orders"), QStringLiteral("items")});
        QCOMPARE(fp1, fp2);
    }

    void fingerprint_getter_returnsLocalFp() {
        SchemaGuard sg;
        sg.setLocal(1, QStringLiteral("my_fp"));
        QCOMPARE(sg.fingerprint(), QStringLiteral("my_fp"));
    }
};

QTEST_APPLESS_MAIN(TstSyncSchemaGuard)
#include "tst_sync_schema_guard.moc"
