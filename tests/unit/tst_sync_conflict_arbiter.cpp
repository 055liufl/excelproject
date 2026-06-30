#include <QtTest>

#include "sync/conflict/ConflictArbiter.h"

// ============================================================================
// tst_sync_conflict_arbiter.cpp — ConflictArbiter「冲突仲裁全序」单元测试
// ============================================================================
//
// 【被测对象是什么】
//   ConflictArbiter（见 src/sync/conflict/ConflictArbiter.h）裁决「同一行被多个节点
//   并发修改时，谁的版本说了算」。它定义了一个**与到达顺序无关的全序（total order）**：
//   按 (rank 降序 → originSeq 降序 → originId 字典序降序) 比较两个候选版本。只要各节点都
//   按这同一个全序独立仲裁，无论变更以什么顺序传播到达，最终都会收敛到同一个胜者——
//   这是多节点同步「不分叉、全域一致」的数学基石。
//
// 【这个测试文件在守护什么不变量】
//   全序的正确性靠数学性质保证，本文件就是逐条把这些性质钉死：
//     · 规则正确：rank 优先、其次 seq、最后 originId 决胜；
//     · 严格全序的三大公理：irreflexive（不自反）、antisymmetric（反对称）、
//       transitive（传递）。
//   任何一条被破坏，都意味着可能出现「A 胜 B、B 胜 C、却 C 胜 A」之类的环，进而导致
//   不同节点收敛到不同结果。故这些测试是同步正确性的核心回归防线。
//
// 【测试框架】Qt Test：类内每个 private slot 即一个测试用例；末尾 QTEST_APPLESS_MAIN
//   生成入口（APPLESS = 不创建 QApplication，因为纯逻辑测试不需要事件循环/GUI）。
//   断言宏：QVERIFY(布尔条件)、QCOMPARE(实际, 期望)。
//   所有用例都遵循 GIVEN（setRankMap 配好排名）→ WHEN（调 beats/rankOf）→ THEN（断言）。
// ============================================================================

using namespace dbridge::sync;

class TstSyncConflictArbiter : public QObject {
    Q_OBJECT
   private slots:
    // setRankMap_rankOf —— 验证「排名表的存取」这一最基础契约。
    // GIVEN 配置 A=10/B=5/C=1 的排名表；WHEN 查询各 origin 的 rank；
    // THEN 已配置的如实返回，未配置的 "UNKNOWN" 回退到默认 0（最低优先级）。
    // 业务含义：rankOf 对未知节点返回 0 是「陌生来源天然最弱」的安全默认，避免崩溃或抛错。
    void setRankMap_rankOf() {
        ConflictArbiter arb;
        arb.setRankMap({{"A", 10}, {"B", 5}, {"C", 1}});
        QCOMPARE(arb.rankOf("A"), 10);
        QCOMPARE(arb.rankOf("B"), 5);
        QCOMPARE(arb.rankOf("C"), 1);
        QCOMPARE(arb.rankOf("UNKNOWN"), 0);  // default —— 未配置的来源回退到最低 rank=0
    }

    // beats_higherRankWins —— 验证全序第一关键字：rank 高者无条件胜，与 seq 无关。
    // GIVEN A=10、B=3；WHEN 让 A 以极小 seq(1) 对阵 B 的极大 seq(999)、再反向；
    // THEN A 始终胜、B 始终败。业务含义：权威等级（rank）凌驾于「新旧」之上——
    //   中心节点（高 rank）即便序号更小，也压过子节点更新的改动，确保「以指定数据为准」。
    void beats_higherRankWins() {
        ConflictArbiter arb;
        arb.setRankMap({{"A", 10}, {"B", 3}});
        // A rank=10 beats B rank=3 regardless of seq
        // 译：A 的 rank=10 击败 B 的 rank=3，与各自 seq 大小无关。
        QVERIFY(arb.beats("A", 999, "B", 1));
        QVERIFY(!arb.beats("B", 999, "A", 1));  // 反向：低 rank 的 B 即便 seq 极大也败
    }

    // beats_sameRankHigherSeqWins —— 验证全序第二关键字：rank 相同时，seq 高者胜。
    // GIVEN A、B 同为 rank=5；WHEN 比较二者不同 seq；THEN seq 大的一方胜。
    // 业务含义：同等权威下，「更新的版本」（seq 越大越新）覆盖较旧版本——即同级时按时序取最新。
    void beats_sameRankHigherSeqWins() {
        ConflictArbiter arb;
        arb.setRankMap({{"A", 5}, {"B", 5}});
        QVERIFY(arb.beats("A", 10, "B", 5));   // same rank, A seq=10 > B seq=5 → A 胜
        QVERIFY(!arb.beats("A", 5, "B", 10));  // same rank, A seq=5 < B seq=10 → A 败
    }

    // beats_sameRankSameSeq_originTieBreak —— 验证全序第三（决胜）关键字：originId 字典序。
    // GIVEN A、B 同 rank、同 seq（前两关键字打平）；WHEN 用相同 seq=7 比较；
    // THEN 按 originId 字典序大者胜（"B" > "A" ⇒ B 胜）。
    // 业务含义：当 rank 与 seq 都无法分出胜负时，必须有一个**确定且全局一致**的决胜手段，
    //   否则全序不完整、可能分叉。用 originId 字典序作纯粹的稳定 tie-break（见 .cpp H-01）。
    void beats_sameRankSameSeq_originTieBreak() {
        ConflictArbiter arb;
        arb.setRankMap({{"B", 5}, {"A", 5}});
        // tie-break: lexicographic "B" > "A" => B beats A
        // 译：决胜——字典序 "B" > "A"，故 B 击败 A。
        QVERIFY(arb.beats("B", 7, "A", 7));
        QVERIFY(!arb.beats("A", 7, "B", 7));  // 反向自洽：A 不能胜 B
    }

    // beats_irreflexive —— 全序公理①：不自反（irreflexive）——任何候选都不能击败它自己。
    // GIVEN 单一节点 X；WHEN 让完全相同的 (X,3) 自我对阵；THEN beats 必为 false。
    // 业务含义：若某版本能「击败自己」，仲裁就会陷入自相矛盾；该用例钉死 a==b ⇒ 不胜。
    void beats_irreflexive() {
        ConflictArbiter arb;
        arb.setRankMap({{"X", 5}});
        QVERIFY(!arb.beats("X", 3, "X", 3));  // 自己不能击败自己
    }

    // beats_antisymmetric —— 全序公理②：反对称（antisymmetric）——对不相等的两者，
    //   beats(a,b) 与 beats(b,a) 恰有且仅有一个为真。
    // GIVEN A=7、B=3；WHEN 双向各比一次；THEN ab 与 ba 必不相等（异或为真）。
    // 业务含义：保证「胜负关系唯一确定」，不会出现「互相都胜」或「互相都不胜」的悬而未决。
    void beats_antisymmetric() {
        ConflictArbiter arb;
        arb.setRankMap({{"A", 7}, {"B", 3}});
        bool ab = arb.beats("A", 1, "B", 1);
        bool ba = arb.beats("B", 1, "A", 1);
        QVERIFY(ab != ba);  // exactly one is true —— 恰好一个为真
    }

    // beats_transitive —— 全序公理③：传递性（transitive）——A 胜 B 且 B 胜 C ⇒ A 胜 C。
    // GIVEN A=10、B=5、C=2；WHEN 验证 A>B、B>C 后再验 A>C；THEN 三者皆成立。
    // 业务含义：传递性是「无环」的保证。若它被破坏（出现 A>B>C>A 的环），多节点仲裁就无法
    //   收敛到唯一胜者——这是全序里最关键、也最易被实现 bug 破坏的一条，故单列用例守护。
    void beats_transitive() {
        ConflictArbiter arb;
        arb.setRankMap({{"A", 10}, {"B", 5}, {"C", 2}});
        QVERIFY(arb.beats("A", 1, "B", 1));  // A > B
        QVERIFY(arb.beats("B", 1, "C", 1));  // B > C
        QVERIFY(arb.beats("A", 1, "C", 1));  // A > C  (transitive) —— 由前两条传递得出
    }

    // beats_totalOrder_data —— 数据驱动测试的「数据表」（与下方 beats_totalOrder 配对）。
    // 【Qt Test 数据驱动机制】带 _data 后缀的槽用 addColumn 声明列、newRow 逐行填入一组
    //   输入与期望；框架会以每一行各跑一次同名（去掉 _data）的测试槽，用 QFETCH 取出该行数据。
    //   好处：同一套断言逻辑覆盖多组场景，新增场景只需加一行，且失败时能精确报出是哪一行（行名）。
    // 这里以 rank A=10、B=5 为背景，列出四类边界：高 rank 压低 rank（无视 seq）、
    //   低 rank 任何 seq 都败、同 rank 高 seq 胜、同 rank 低 seq 败。
    void beats_totalOrder_data() {
        QTest::addColumn<QString>("aO");     // 候选 a 的 origin
        QTest::addColumn<qint64>("aS");      // 候选 a 的 seq
        QTest::addColumn<QString>("bO");     // 候选 b 的 origin
        QTest::addColumn<qint64>("bS");      // 候选 b 的 seq
        QTest::addColumn<bool>("expected");  // 期望 a 是否击败 b
        // rank A=10, B=5（背景排名，见下方 beats_totalOrder 里的 setRankMap）
        QTest::newRow("high_rank_beats_low_rank") << "A" << 1LL << "B" << 100LL << true;
        QTest::newRow("low_rank_loses_any_seq") << "B" << 100LL << "A" << 1LL << false;
        QTest::newRow("same_rank_higher_seq_wins") << "A" << 10LL << "A" << 5LL << true;
        QTest::newRow("same_rank_lower_seq_loses") << "A" << 5LL << "A" << 10LL << false;
    }

    // beats_totalOrder —— 数据驱动测试本体：对上表每一行取数、跑同一条断言。
    // QFETCH 把当前数据行的各列取进同名局部变量；随后用固定背景排名(A=10,B=5)验证
    //   beats 的实际结果与该行 expected 相符。它把「规则正确」的多个边界一次性收口验证。
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
