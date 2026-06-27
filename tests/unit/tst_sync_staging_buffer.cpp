#include "dbridge/sync/SyncTypes.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/apply/UpsertExecutor.h"
#include "sync/diff/StagingBuffer.h"

using namespace dbridge::sync;

class TstSyncStagingBuffer : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

    void exec(const QString& sql) {
        QSqlQuery q(db_);
        QVERIFY2(q.exec(sql), qPrintable(q.lastError().text()));
    }
    int count(const QString& t) {
        QSqlQuery q(db_);
        q.exec("SELECT COUNT(*) FROM " + t);
        return q.next() ? q.value(0).toInt() : -1;
    }

   private slots:
    void init() {
        conn_ =
            QStringLiteral("tst_sb_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        exec("CREATE TABLE orders (id INTEGER PRIMARY KEY NOT NULL, name TEXT)");
    }
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    void isEmpty_initially() {
        StagingBuffer buf;
        QVERIFY(buf.isEmpty());
    }

    void stage_notEmpty() {
        StagingBuffer buf;
        QVariantMap row;
        row["id"] = 1;
        row["name"] = "Alice";
        buf.stage("orders", "1", row);
        QVERIFY(!buf.isEmpty());
    }

    void unstage_removesEntry() {
        StagingBuffer buf;
        QVariantMap row;
        row["id"] = 1;
        row["name"] = "A";
        buf.stage("orders", "1", row);
        buf.unstage("orders", "1");
        QVERIFY(buf.isEmpty());
    }

    void getRow_returnsStaged() {
        StagingBuffer buf;
        QVariantMap row;
        row["id"] = 42;
        row["name"] = "Bob";
        buf.stage("orders", "42", row);
        QVariantMap got = buf.getRow("orders", "42");
        QCOMPARE(got["name"].toString(), QString("Bob"));
    }

    void getRow_missing_returnsEmpty() {
        StagingBuffer buf;
        QVERIFY(buf.getRow("orders", "99").isEmpty());
    }

    void discard_zeroPersist() {
        StagingBuffer buf;
        QVariantMap row;
        row["id"] = 7;
        row["name"] = "Carol";
        buf.stage("orders", "7", row);
        buf.discard();
        QVERIFY(buf.isEmpty());
        QCOMPARE(count("orders"), 0);  // nothing written to DB
    }

    void save_writesToDb() {
        StagingBuffer buf;
        QVariantMap row;
        row["id"] = 10;
        row["name"] = "Dave";
        buf.stage("orders", "10", row);
        UpsertExecutor ue;
        QString err;
        QVERIFY(buf.save(db_, ue, QStringList{"id"}, &err));
        QCOMPARE(count("orders"), 1);
        // Note: save() does NOT clear staged_ (by design)
    }

    void save_empty_succeeds() {
        StagingBuffer buf;
        UpsertExecutor ue;
        QString err;
        QVERIFY(buf.save(db_, ue, QStringList{"id"}, &err));
        QCOMPARE(count("orders"), 0);
    }

    void toMutations_buildsCorrectly() {
        StagingBuffer buf;
        QVariantMap row;
        row["id"] = 5;
        row["name"] = "Eve";
        buf.stage("orders", "5", row);
        QHash<QString, QStringList> pkMap;
        pkMap["orders"] = QStringList{"id"};
        auto muts = buf.toMutations(pkMap);
        QCOMPARE(muts.size(), 1);
        QCOMPARE(muts[0].table, QString("orders"));
        QCOMPARE(muts[0].pkColumns, QStringList({"id"}));
        QCOMPARE(muts[0].mode, UpsertMode::DoUpdate);
    }

    void stage_multipleRows_allSaved() {
        StagingBuffer buf;
        for (int i = 1; i <= 3; ++i) {
            QVariantMap row;
            row["id"] = i;
            row["name"] = QString("N%1").arg(i);
            buf.stage("orders", QString::number(i), row);
        }
        UpsertExecutor ue;
        QString err;
        QVERIFY(buf.save(db_, ue, QStringList{"id"}, &err));
        QCOMPARE(count("orders"), 3);
    }
};

QTEST_APPLESS_MAIN(TstSyncStagingBuffer)
#include "tst_sync_staging_buffer.moc"
