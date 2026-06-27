#include <QtTest>

#include "sync/conflict/RoutingTable.h"

using namespace dbridge::sync;

class TstSyncRoutingTable : public QObject {
    Q_OBJECT
   private slots:
    void shouldRoute_originEqualsPeer_false() {
        RoutingTable rt;
        rt.configure("center", {"A", "B"});
        // origin == peer => don't echo back to source
        QVERIFY(!rt.shouldRoute("A", "A", 10, 0));
    }

    void shouldRoute_seqAlreadyAcked_false() {
        RoutingTable rt;
        rt.configure("center", {"A", "B"});
        // originSeq <= peerAckedSeq => already confirmed
        QVERIFY(!rt.shouldRoute("B", "A", 5, 5));
        QVERIFY(!rt.shouldRoute("B", "A", 4, 5));
    }

    void shouldRoute_newToPeer_true() {
        RoutingTable rt;
        rt.configure("center", {"A", "B"});
        // origin != peer && seq > acked => should route
        QVERIFY(rt.shouldRoute("B", "A", 6, 5));
    }

    void shouldRoute_seqBoundary() {
        RoutingTable rt;
        rt.configure("center", {"A", "B"});
        // boundary: seq == acked => false; seq == acked+1 => true
        QVERIFY(!rt.shouldRoute("B", "A", 7, 7));
        QVERIFY(rt.shouldRoute("B", "A", 8, 7));
    }

    void shouldRoute_multiPeerFanout() {
        RoutingTable rt;
        rt.configure("center", {"A", "B", "C"});
        // origin=A, seq=7; peers: A acked=0, B acked=3, C acked=7
        QVERIFY(!rt.shouldRoute("A", "A", 7, 0));  // origin==peer
        QVERIFY(rt.shouldRoute("B", "A", 7, 3));   // 7>3, not echo
        QVERIFY(!rt.shouldRoute("C", "A", 7, 7));  // 7==7 already acked
    }

    void shouldRoute_truthTable_data() {
        QTest::addColumn<QString>("peer");
        QTest::addColumn<QString>("origin");
        QTest::addColumn<qint64>("originSeq");
        QTest::addColumn<qint64>("peerAckedSeq");
        QTest::addColumn<bool>("expected");

        QTest::newRow("echo_low") << "A" << "A" << 1LL << 0LL << false;
        QTest::newRow("echo_high") << "A" << "A" << 99LL << 0LL << false;
        QTest::newRow("acked_eq") << "B" << "A" << 5LL << 5LL << false;
        QTest::newRow("acked_lt") << "B" << "A" << 4LL << 5LL << false;
        QTest::newRow("new_plus1") << "B" << "A" << 6LL << 5LL << true;
        QTest::newRow("new_large") << "B" << "A" << 100LL << 5LL << true;
        QTest::newRow("fresh_peer") << "B" << "A" << 1LL << -1LL << true;
    }

    void shouldRoute_truthTable() {
        QFETCH(QString, peer);
        QFETCH(QString, origin);
        QFETCH(qint64, originSeq);
        QFETCH(qint64, peerAckedSeq);
        QFETCH(bool, expected);
        RoutingTable rt;
        rt.configure("center", {"A", "B", "C"});
        QCOMPARE(rt.shouldRoute(peer, origin, originSeq, peerAckedSeq), expected);
    }
};

QTEST_APPLESS_MAIN(TstSyncRoutingTable)
#include "tst_sync_routing_table.moc"
