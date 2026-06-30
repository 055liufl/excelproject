// ============================================================================
// tst_sync_routing_table.cpp — RoutingTable（变更转发路由表）单元测试
// ============================================================================
//
// 【被测对象是什么】
//   RoutingTable 回答一个核心的「该不该转发」问题：对于「来源 origin、序号 originSeq 的某条
//   变更」，要不要把它转发（route）给「对端 peer」？它由中心节点在广播 / 中转变更时调用，
//   核心方法签名：
//     shouldRoute(peer, origin, originSeq, peerAckedSeq) -> bool
//   其中 peerAckedSeq 是「peer 已经对该 origin 确认到的序号」（水位）。
//
// 【判定规则（两条铁律，贯穿全部用例）】
//   规则①「不回声（no echo）」：若 peer == origin，返回 false。
//       一条变更绝不该被转发回它自己的来源节点——来源本来就有它，回送既浪费又可能成环。
//   规则②「不重发已确认的（already acked）」：若 originSeq <= peerAckedSeq，返回 false。
//       该对端已经确认收到并应用到 originSeq（或更新）了，再发就是冗余。
//   只有「peer != origin  且  originSeq > peerAckedSeq」时才返回 true（确为「新且非回声」）。
//   关键边界：originSeq == acked → false（恰好已确认）；originSeq == acked+1 → true（下一条新的）。
//   特例：peerAckedSeq == -1 表示「该对端尚未确认过任何东西」（全新对端），此时任意正序号都该发。
//
// 【为什么需要它（系统价值）】多节点扇出（fanout）广播时，中心要对每个对端分别判定，避免
//   把变更回送来源、避免重复发送已确认内容——这既是带宽优化，也是防止广播风暴 / 环路的关键。
//
// 【测试策略】纯内存、无库无线程。configure(centerId, {peers...}) 设定拓扑后，逐场景断言
//   shouldRoute 的布尔结果；最后用 Qt 的「数据驱动测试」(_data + QFETCH) 跑一张真值表覆盖
//   所有典型组合。
// ============================================================================
#include <QtTest>

#include "sync/conflict/RoutingTable.h"

using namespace dbridge::sync;

class TstSyncRoutingTable : public QObject {
    Q_OBJECT
   private slots:
    // shouldRoute_originEqualsPeer_false —— 规则①「不回声」：peer==origin 永不转发。
    // 这里 origin=peer=A，即便序号 10 远大于 acked=0，也不能把 A 的变更回送给 A。
    void shouldRoute_originEqualsPeer_false() {
        RoutingTable rt;
        rt.configure("center", {"A", "B"});
        // origin == peer => don't echo back to source（不回送给来源）
        QVERIFY(!rt.shouldRoute("A", "A", 10, 0));
    }

    // shouldRoute_seqAlreadyAcked_false —— 规则②「不重发」：originSeq <= acked 不转发。
    // seq=5,acked=5（恰好已确认）与 seq=4,acked=5（更旧）都应被抑制。
    void shouldRoute_seqAlreadyAcked_false() {
        RoutingTable rt;
        rt.configure("center", {"A", "B"});
        // originSeq <= peerAckedSeq => already confirmed（已确认，无需重发）
        QVERIFY(!rt.shouldRoute("B", "A", 5, 5));
        QVERIFY(!rt.shouldRoute("B", "A", 4, 5));
    }

    // shouldRoute_newToPeer_true —— 正路径：非回声 且 序号超过水位 → 应转发。
    // peer=B != origin=A，seq=6 > acked=5：这是 B 还没确认的新变更，应当发给它。
    void shouldRoute_newToPeer_true() {
        RoutingTable rt;
        rt.configure("center", {"A", "B"});
        // origin != peer && seq > acked => should route（新且非回声 → 转发）
        QVERIFY(rt.shouldRoute("B", "A", 6, 5));
    }

    // shouldRoute_seqBoundary —— 边界：恰好等于水位不发，水位 +1 才发（界定 ">" 而非 ">="）。
    // seq==acked(7) → false；seq==acked+1(8) → true。锁死「严格大于」的比较语义。
    void shouldRoute_seqBoundary() {
        RoutingTable rt;
        rt.configure("center", {"A", "B"});
        // boundary: seq == acked => false; seq == acked+1 => true（边界用例）
        QVERIFY(!rt.shouldRoute("B", "A", 7, 7));
        QVERIFY(rt.shouldRoute("B", "A", 8, 7));
    }

    // shouldRoute_multiPeerFanout —— 扇出场景：同一条变更对不同对端各自独立判定。
    // GIVEN origin=A、seq=7，三个对端水位各异：A(自己,0)、B(已确认到3)、C(已确认到7)
    // THEN  对 A → false（回声）；对 B → true（7>3，且非回声）；对 C → false（7==7 已确认）。
    // 体现：路由判定是「逐对端」的，结论可能因对端水位不同而不同。
    void shouldRoute_multiPeerFanout() {
        RoutingTable rt;
        rt.configure("center", {"A", "B", "C"});
        // origin=A, seq=7; peers: A acked=0, B acked=3, C acked=7
        QVERIFY(!rt.shouldRoute("A", "A", 7, 0));  // origin==peer（回声，不发）
        QVERIFY(rt.shouldRoute("B", "A", 7, 3));   // 7>3, not echo（新且非回声，发）
        QVERIFY(!rt.shouldRoute("C", "A", 7, 7));  // 7==7 already acked（已确认，不发）
    }

    // shouldRoute_truthTable_data —— 为下面的数据驱动测试准备「真值表」数据集。
    // Qt 数据驱动机制：addColumn 声明每行的字段；newRow("名字") << ... 逐列灌入一行数据；
    // 框架会以每一行各跑一次 shouldRoute_truthTable()，QFETCH 在那里取出本行各字段。
    // 这张表系统化覆盖两条规则的所有典型组合（回声/已确认/新/全新对端），是回归基线。
    void shouldRoute_truthTable_data() {
        QTest::addColumn<QString>("peer");
        QTest::addColumn<QString>("origin");
        QTest::addColumn<qint64>("originSeq");
        QTest::addColumn<qint64>("peerAckedSeq");
        QTest::addColumn<bool>("expected");

        // 回声：peer==origin，无论序号高低都不发。
        QTest::newRow("echo_low") << "A" << "A" << 1LL << 0LL << false;
        QTest::newRow("echo_high") << "A" << "A" << 99LL << 0LL << false;
        // 已确认：seq<=acked 不发（相等 / 更旧）。
        QTest::newRow("acked_eq") << "B" << "A" << 5LL << 5LL << false;
        QTest::newRow("acked_lt") << "B" << "A" << 4LL << 5LL << false;
        // 新变更：seq>acked 应发（紧邻 +1 / 远超）。
        QTest::newRow("new_plus1") << "B" << "A" << 6LL << 5LL << true;
        QTest::newRow("new_large") << "B" << "A" << 100LL << 5LL << true;
        // 全新对端：acked=-1（从未确认），任意正序号都该发。
        QTest::newRow("fresh_peer") << "B" << "A" << 1LL << -1LL << true;
    }

    // shouldRoute_truthTable —— 数据驱动执行体：对上表每一行取出参数并断言 shouldRoute 结果。
    // QFETCH 从当前数据行抽取同名字段；QCOMPARE 把实际结果与 expected 比对（不符则该行报失败）。
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

// 无 GUI 的测试入口；末行引入 moc 生成代码（Qt Test 单文件固定写法）。
QTEST_APPLESS_MAIN(TstSyncRoutingTable)
#include "tst_sync_routing_table.moc"
