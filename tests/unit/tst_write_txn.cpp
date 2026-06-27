#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/WriteTxn.h"

using namespace dbridge::sync;

class TstWriteTxn : public QObject {
    Q_OBJECT
    QString conn_;
    // store the db handle as a member so we can bind non-const lvalue refs
    QSqlDatabase db_;

   private slots:
    void init() {
        conn_ = QStringLiteral("tst_wtxn_") +
                QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        QVERIFY(q.exec(QStringLiteral("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)")));
    }
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    void begin_setsActive() {
        WriteTxn txn(db_);
        QString err;
        QVERIFY(txn.begin(&err));
        QVERIFY(txn.isActive());
    }

    void commit_persists() {
        WriteTxn txn(db_);
        QString err;
        txn.begin(&err);
        QSqlQuery q(db_);
        q.exec(QStringLiteral("INSERT INTO t VALUES (1, 'hello')"));
        QVERIFY(txn.commit(&err));
        QVERIFY(!txn.isActive());
        // verify row is visible
        QSqlQuery s(db_);
        s.exec(QStringLiteral("SELECT COUNT(*) FROM t WHERE id=1"));
        QVERIFY(s.next());
        QCOMPARE(s.value(0).toInt(), 1);
    }

    void rollback_discards() {
        WriteTxn txn(db_);
        QString err;
        txn.begin(&err);
        QSqlQuery q(db_);
        q.exec(QStringLiteral("INSERT INTO t VALUES (2, 'gone')"));
        txn.rollback();
        QVERIFY(!txn.isActive());
        QSqlQuery s(db_);
        s.exec(QStringLiteral("SELECT COUNT(*) FROM t WHERE id=2"));
        QVERIFY(s.next());
        QCOMPARE(s.value(0).toInt(), 0);
    }

    void destructor_rollsBackIfActive() {
        {
            WriteTxn txn(db_);
            QString err;
            txn.begin(&err);
            QSqlQuery q(db_);
            q.exec(QStringLiteral("INSERT INTO t VALUES (3, 'raii')"));
        }  // destructor should rollback
        QSqlQuery s(db_);
        s.exec(QStringLiteral("SELECT COUNT(*) FROM t WHERE id=3"));
        QVERIFY(s.next());
        QCOMPARE(s.value(0).toInt(), 0);
    }

    void rollback_whenInactive_noop() {
        WriteTxn txn(db_);
        txn.rollback();  // must not crash
        QVERIFY(!txn.isActive());
    }

    void isActive_tracksState() {
        WriteTxn txn(db_);
        QVERIFY(!txn.isActive());
        QString err;
        txn.begin(&err);
        QVERIFY(txn.isActive());
        txn.rollback();
        QVERIFY(!txn.isActive());
    }
};

QTEST_APPLESS_MAIN(TstWriteTxn)
#include "tst_write_txn.moc"
