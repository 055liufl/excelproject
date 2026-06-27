#include <QtTest>

#include "sync/conflict/ConflictArbiter.h"

using namespace dbridge::sync;

class TstSyncConflictArbiter : public QObject {
    Q_OBJECT
   private slots:
    void setRankMap_rankOf() {
        ConflictArbiter arb;
        arb.setRankMap({{"A", 10}, {"B", 5}, {"C", 1}});
        QCOMPARE(arb.rankOf("A"), 10);
        QCOMPARE(arb.rankOf("B"), 5);
        QCOMPARE(arb.rankOf("C"), 1);
        QCOMPARE(arb.rankOf("UNKNOWN"), 0);  // default
    }

    void beats_higherRankWins() {
        ConflictArbiter arb;
        arb.setRankMap({{"A", 10}, {"B", 3}});
        // A rank=10 beats B rank=3 regardless of seq
        QVERIFY(arb.beats("A", 999, "B", 1));
        QVERIFY(!arb.beats("B", 999, "A", 1));
    }

    void beats_sameRankHigherSeqWins() {
        ConflictArbiter arb;
        arb.setRankMap({{"A", 5}, {"B", 5}});
        QVERIFY(arb.beats("A", 10, "B", 5));   // same rank, A seq=10 > B seq=5
        QVERIFY(!arb.beats("A", 5, "B", 10));  // same rank, A seq=5 < B seq=10
    }

    void beats_sameRankSameSeq_originTieBreak() {
        ConflictArbiter arb;
        arb.setRankMap({{"B", 5}, {"A", 5}});
        // tie-break: lexicographic "B" > "A" => B beats A
        QVERIFY(arb.beats("B", 7, "A", 7));
        QVERIFY(!arb.beats("A", 7, "B", 7));
    }

    void beats_irreflexive() {
        ConflictArbiter arb;
        arb.setRankMap({{"X", 5}});
        QVERIFY(!arb.beats("X", 3, "X", 3));
    }

    void beats_antisymmetric() {
        ConflictArbiter arb;
        arb.setRankMap({{"A", 7}, {"B", 3}});
        bool ab = arb.beats("A", 1, "B", 1);
        bool ba = arb.beats("B", 1, "A", 1);
        QVERIFY(ab != ba);  // exactly one is true
    }

    void beats_transitive() {
        ConflictArbiter arb;
        arb.setRankMap({{"A", 10}, {"B", 5}, {"C", 2}});
        QVERIFY(arb.beats("A", 1, "B", 1));  // A > B
        QVERIFY(arb.beats("B", 1, "C", 1));  // B > C
        QVERIFY(arb.beats("A", 1, "C", 1));  // A > C  (transitive)
    }

    void beats_totalOrder_data() {
        QTest::addColumn<QString>("aO");
        QTest::addColumn<qint64>("aS");
        QTest::addColumn<QString>("bO");
        QTest::addColumn<qint64>("bS");
        QTest::addColumn<bool>("expected");
        // rank A=10, B=5
        QTest::newRow("high_rank_beats_low_rank") << "A" << 1LL << "B" << 100LL << true;
        QTest::newRow("low_rank_loses_any_seq") << "B" << 100LL << "A" << 1LL << false;
        QTest::newRow("same_rank_higher_seq_wins") << "A" << 10LL << "A" << 5LL << true;
        QTest::newRow("same_rank_lower_seq_loses") << "A" << 5LL << "A" << 10LL << false;
    }

    void beats_totalOrder() {
        QFETCH(QString, aO);
        QFETCH(qint64, aS);
        QFETCH(QString, bO);
        QFETCH(qint64, bS);
        QFETCH(bool, expected);
        ConflictArbiter arb;
        arb.setRankMap({{"A", 10}, {"B", 5}});
        QCOMPARE(arb.beats(aO, aS, bO, bS), expected);
    }
};

QTEST_APPLESS_MAIN(TstSyncConflictArbiter)
#include "tst_sync_conflict_arbiter.moc"
