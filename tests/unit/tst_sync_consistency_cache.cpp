#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/selection/ConsistencyCache.h"

using namespace dbridge::sync;

class TstSyncConsistencyCache : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

   private slots:
    void init() {
        conn_ =
            QStringLiteral("tst_cc_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
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

    void init_inMemory_ok() {
        ConsistencyCache cc;
        QString err;
        QVERIFY(cc.init(db_, /*durable=*/false, &err));
    }

    void init_durable_ok() {
        ConsistencyCache cc;
        QString err;
        QVERIFY(cc.init(db_, /*durable=*/true, &err));
    }

    void isConsistent_unknownEntry_false() {
        ConsistencyCache cc;
        QString err;
        cc.init(db_, false, &err);
        QVERIFY(!cc.isConsistent("orders", "pk1", QByteArray("fp")));
    }

    void stampAndCheck_consistent() {
        ConsistencyCache cc;
        QString err;
        cc.init(db_, false, &err);
        QByteArray fp("center_fingerprint_abc");
        cc.stampFromAuthoritative(db_, "orders", "pk1", fp);
        QVERIFY(cc.isConsistent("orders", "pk1", fp));
    }

    void isConsistent_differentFp_false() {
        ConsistencyCache cc;
        QString err;
        cc.init(db_, false, &err);
        cc.stampFromAuthoritative(db_, "orders", "pk1", QByteArray("fp_center"));
        QVERIFY(!cc.isConsistent("orders", "pk1", QByteArray("fp_local_different")));
    }

    void invalidateTable_clearsEntries() {
        ConsistencyCache cc;
        QString err;
        cc.init(db_, false, &err);
        cc.stampFromAuthoritative(db_, "orders", "pk1", QByteArray("fp"));
        cc.stampFromAuthoritative(db_, "orders", "pk2", QByteArray("fp"));
        cc.stampFromAuthoritative(db_, "items", "pk3", QByteArray("fp"));
        cc.invalidateTable(db_, "orders");
        QVERIFY(!cc.isConsistent("orders", "pk1", QByteArray("fp")));
        QVERIFY(!cc.isConsistent("orders", "pk2", QByteArray("fp")));
        QVERIFY(cc.isConsistent("items", "pk3", QByteArray("fp")));  // other table unaffected
    }

    void durable_persistsAcrossInit() {
        {
            ConsistencyCache cc;
            QString err;
            cc.init(db_, /*durable=*/true, &err);
            cc.stampFromAuthoritative(db_, "t", "pk", QByteArray("fp_v1"));
        }
        // new instance, same DB
        ConsistencyCache cc2;
        QString err;
        cc2.init(db_, /*durable=*/true, &err);
        QVERIFY(cc2.isConsistent("t", "pk", QByteArray("fp_v1")));
    }

    void nonDurable_doesNotPersist() {
        {
            ConsistencyCache cc;
            QString err;
            cc.init(db_, /*durable=*/false, &err);
            cc.stampFromAuthoritative(db_, "t", "pk", QByteArray("fp"));
        }
        ConsistencyCache cc2;
        QString err;
        cc2.init(db_, /*durable=*/false, &err);
        QVERIFY(!cc2.isConsistent("t", "pk", QByteArray("fp")));
    }

    void multipleTablesPkIsolated() {
        ConsistencyCache cc;
        QString err;
        cc.init(db_, false, &err);
        cc.stampFromAuthoritative(db_, "tA", "pk1", QByteArray("fp_a"));
        cc.stampFromAuthoritative(db_, "tB", "pk1", QByteArray("fp_b"));
        QVERIFY(cc.isConsistent("tA", "pk1", QByteArray("fp_a")));
        QVERIFY(!cc.isConsistent("tA", "pk1", QByteArray("fp_b")));
        QVERIFY(cc.isConsistent("tB", "pk1", QByteArray("fp_b")));
    }
};

QTEST_APPLESS_MAIN(TstSyncConsistencyCache)
#include "tst_sync_consistency_cache.moc"
