#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/anchor/OutboundAckStore.h"

using namespace dbridge::sync;

class TstSyncOutboundAck : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

   private slots:
    void init() {
        conn_ =
            QStringLiteral("tst_oa_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
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
        OutboundAckStore oas;
        QString err;
        QVERIFY(oas.init(db_, &err));
    }

    void ackedSeq_noRow_minusOne() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        QCOMPARE(oas.ackedSeq(db_, "peer1", "originA", 1), qint64(-1));
    }

    void updateAcked_stores() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        QVERIFY(oas.updateAcked(db_, "peer1", "originA", 1, 10, &err));
        QCOMPARE(oas.ackedSeq(db_, "peer1", "originA", 1), qint64(10));
    }

    void updateAcked_onlyAdvances() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        oas.updateAcked(db_, "peer1", "A", 1, 20, &err);
        // lower seq should not overwrite
        oas.updateAcked(db_, "peer1", "A", 1, 10, &err);
        QCOMPARE(oas.ackedSeq(db_, "peer1", "A", 1), qint64(20));
    }

    void minAckedSeq_acrossAllPeers() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        oas.updateAcked(db_, "peer1", "A", 1, 5, &err);
        oas.updateAcked(db_, "peer2", "A", 1, 10, &err);
        oas.updateAcked(db_, "peer3", "A", 1, 3, &err);
        // minimum is 3
        QCOMPARE(oas.minAckedSeq(db_, "A", 1), qint64(3));
    }

    void minAckedSeq_noRows_minusOne() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        QCOMPARE(oas.minAckedSeq(db_, "unknown", 99), qint64(-1));
    }

    void setPendingBaseline_flagsRow() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        oas.updateAcked(db_, "peer1", "A", 1, 5, &err);
        QVERIFY(oas.setPendingBaseline(db_, "peer1", true, &err));
        // verify via direct SELECT
        QSqlQuery q(db_);
        q.prepare(QStringLiteral(
            "SELECT pending_baseline FROM __sync_outbound_ack WHERE peer=? AND origin=?"));
        q.addBindValue("peer1");
        q.addBindValue("A");
        q.exec();
        q.next();
        QCOMPARE(q.value(0).toInt(), 1);
    }

    void lastSentLocalSeq_default_minusOne() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        QCOMPARE(oas.lastSentLocalSeq(db_, "peer1", 1), qint64(-1));
    }

    void updateLastSent_stores() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        oas.updateAcked(db_, "peer1", "A", 1, 0, &err);
        QVERIFY(oas.updateLastSent(db_, "peer1", 1, 42, &err));
        QCOMPARE(oas.lastSentLocalSeq(db_, "peer1", 1), qint64(42));
    }

    void multiPeerMultiOrigin_isolated() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        oas.updateAcked(db_, "peer1", "A", 1, 5, &err);
        oas.updateAcked(db_, "peer1", "B", 1, 8, &err);
        oas.updateAcked(db_, "peer2", "A", 1, 3, &err);
        QCOMPARE(oas.ackedSeq(db_, "peer1", "A", 1), qint64(5));
        QCOMPARE(oas.ackedSeq(db_, "peer1", "B", 1), qint64(8));
        QCOMPARE(oas.ackedSeq(db_, "peer2", "A", 1), qint64(3));
    }
};

QTEST_APPLESS_MAIN(TstSyncOutboundAck)
#include "tst_sync_outbound_ack.moc"
