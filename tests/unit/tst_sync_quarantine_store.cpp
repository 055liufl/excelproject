#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/schema/QuarantineStore.h"

using namespace dbridge::sync;

class TstSyncQuarantineStore : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

   private slots:
    void init() {
        conn_ =
            QStringLiteral("tst_qs_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        for (const QString& s : dbridge::sync::ddl::allCreateStatements())
            q.exec(s);
    }
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    void init_ok() {
        QuarantineStore qs;
        QString err;
        QVERIFY(qs.init(db_, &err));
    }

    void quarantine_storesRow() {
        QuarantineStore qs;
        QString err;
        qs.init(db_, &err);
        QVERIFY(qs.quarantine(db_, "nodeA", /*seq=*/5, /*epoch=*/1,
                              /*schemaVer=*/3, QByteArray("payload_data"), &err));
        QSqlQuery q(db_);
        q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_quarantine"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
    }

    void drainReady_returnsMatchingSchemaVer() {
        QuarantineStore qs;
        QString err;
        qs.init(db_, &err);
        // store with schemaVer=3
        qs.quarantine(db_, "A", 1, 1, 3, QByteArray("data_v3"), &err);
        // drainReady with currentSchemaVer=3 → returns it
        auto items = qs.drainReady(db_, 3);
        QCOMPARE(items.size(), 1);
    }

    void drainReady_excludesHigherSchemaVer() {
        QuarantineStore qs;
        QString err;
        qs.init(db_, &err);
        qs.quarantine(db_, "A", 1, 1, 5, QByteArray("future"), &err);
        // currentSchemaVer=3 < quarantined schemaVer=5 → not ready
        auto items = qs.drainReady(db_, 3);
        QCOMPARE(items.size(), 0);
    }

    void drainReady_multipleVersions() {
        QuarantineStore qs;
        QString err;
        qs.init(db_, &err);
        qs.quarantine(db_, "A", 1, 1, 2, QByteArray("v2"), &err);
        qs.quarantine(db_, "B", 2, 1, 4, QByteArray("v4"), &err);
        qs.quarantine(db_, "C", 3, 1, 6, QByteArray("v6"), &err);
        // currentSchemaVer=4 → v2 and v4 are ready, v6 not
        auto items = qs.drainReady(db_, 4);
        QCOMPARE(items.size(), 2);
    }

    void markReplayed_removesRow() {
        QuarantineStore qs;
        QString err;
        qs.init(db_, &err);
        qs.quarantine(db_, "A", 1, 1, 3, QByteArray("payload"), &err);
        auto items = qs.drainReady(db_, 3);
        QCOMPARE(items.size(), 1);
        qint64 id = items.first().first;
        qs.markReplayed(db_, id);
        auto items2 = qs.drainReady(db_, 3);
        QCOMPARE(items2.size(), 0);
    }
};

QTEST_APPLESS_MAIN(TstSyncQuarantineStore)
#include "tst_sync_quarantine_store.moc"
