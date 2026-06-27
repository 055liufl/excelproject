#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/anchor/OutboundAckStore.h"
#include "sync/peer/DeadPeerEvictor.h"

using namespace dbridge::sync;

class TstSyncDeadPeerEvictor : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

    DeadPeerEvictor::PeerState makePeer(const QString& peer, qint64 lagSeq, qint64 lagBytes,
                                        qint64 lastAckMs) {
        DeadPeerEvictor::PeerState ps;
        ps.peer = peer;
        ps.lagSeq = lagSeq;
        ps.lagBytes = lagBytes;
        ps.lastAckMs = lastAckMs;
        ps.evicted = false;
        return ps;
    }

   private slots:
    void init() {
        conn_ =
            QStringLiteral("tst_dp_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
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

    void evaluate_healthy_whenBelowAllThresholds() {
        DeadPeerEvictor ev;
        ev.configure(/*softSeq=*/1000, /*hardSeq=*/10000,
                     /*softBytes=*/1024 * 1024, /*hardBytes=*/10 * 1024 * 1024,
                     /*softMs=*/60000, /*hardMs=*/600000);
        // lagSeq=5, lagBytes=100, lastAckMs 1s ago → healthy
        auto ps = makePeer("A", 5, 100, /*nowMs=*/10000 - 1000);
        QCOMPARE(ev.evaluate(ps, 10000), DeadPeerEvictor::AlertLevel::Healthy);
    }

    void evaluate_lagging_whenAboveSoftThreshold() {
        DeadPeerEvictor ev;
        ev.configure(1000, 10000, 1024 * 1024, 10 * 1024 * 1024, 60000, 600000);
        // lagSeq > softSeq → lagging
        auto ps = makePeer("A", 2000, 100, 10000 - 1000);
        QCOMPARE(ev.evaluate(ps, 10000), DeadPeerEvictor::AlertLevel::Lagging);
    }

    void evaluate_dead_whenAboveHardSeq() {
        DeadPeerEvictor ev;
        ev.configure(1000, 10000, 1024 * 1024, 10 * 1024 * 1024, 60000, 600000);
        auto ps = makePeer("A", 20000, 100, 10000 - 1000);  // lagSeq > hardSeq
        QCOMPARE(ev.evaluate(ps, 10000), DeadPeerEvictor::AlertLevel::Dead);
    }

    void evaluate_dead_whenAboveHardMs() {
        DeadPeerEvictor ev;
        ev.configure(1000, 10000, 1024 * 1024, 10 * 1024 * 1024, 60000, 600000);
        // lastAckMs must be > 0 for hasTimeData=true; set to 1 ms ago nowMs=700001
        // elapsed = 700001 - 1 = 700000 ms >> hardMs=600000 → Dead
        auto ps = makePeer("A", 0, 0, /*lastAckMs=*/1);
        QCOMPARE(ev.evaluate(ps, 700001), DeadPeerEvictor::AlertLevel::Dead);
    }

    void evaluate_dead_whenAboveHardBytes() {
        DeadPeerEvictor ev;
        ev.configure(1000, 10000, 1024 * 1024, 10 * 1024 * 1024, 60000, 600000);
        // lagBytes > hardBytes
        auto ps = makePeer("A", 0, 20 * 1024 * 1024, 10000 - 1000);
        QCOMPARE(ev.evaluate(ps, 10000), DeadPeerEvictor::AlertLevel::Dead);
    }

    void evict_setsPendingBaseline() {
        DeadPeerEvictor ev;
        ev.configure(1000, 10000, 1024 * 1024, 10 * 1024 * 1024, 60000, 600000);
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        oas.updateAcked(db_, "peer1", "A", 1, 5, &err);
        QVERIFY(ev.evict(db_, "peer1", oas, &err));
        // pending_baseline should be set
        QSqlQuery q(db_);
        q.prepare("SELECT pending_baseline FROM __sync_outbound_ack WHERE peer=?");
        q.addBindValue("peer1");
        q.exec();
        q.next();
        QCOMPARE(q.value(0).toInt(), 1);
    }

    void defaultThresholds_reasonable() {
        DeadPeerEvictor ev;  // default thresholds
        // With default thresholds, a peer with lagSeq=1 and recent lastAck should be Healthy
        auto ps = makePeer("A", 1, 1024, 5000 - 100);
        auto level = ev.evaluate(ps, 5000);
        QCOMPARE(level, DeadPeerEvictor::AlertLevel::Healthy);
    }
};

QTEST_APPLESS_MAIN(TstSyncDeadPeerEvictor)
#include "tst_sync_dead_peer_evictor.moc"
