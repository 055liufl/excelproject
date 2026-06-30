#include <QSet>
#include <QtTest>

#include "sync/diff/InboundTableGate.h"

// ============================================================================
// tst_sync_inbound_table_gate.cpp — InboundTableGate（入站表门控）单元测试
// ============================================================================
//
// 【被测对象 InboundTableGate 是什么 / 解决什么问题】
//   在「字段级比对（comparison）」期间，用户正盯着某几张表看两端差异并做采用决策。
//   若此刻后台同步把对端发来的、涉及「正在比对的那些表」的入站 changeset 直接应用到
//   本地库，用户眼前的数据就会在脚下被改动，比对结果与决策都会失真。InboundTableGate
//   就是这道「门控」：在比对开始时 open(被监视的表集合)，把门打开；之后每来一个入站
//   payload，先问门控 shouldDefer(payload 涉及的表集合)——只要 payload 触及任意一张被
//   监视的表，就「延后（defer）」这批应用，待比对结束 releaseAll() 关门后再放行。
//   不触及被监视表的 payload 则照常立即应用。
//
// 【门控的极简状态模型（本测试逐一覆盖）】
//   · isOpen()：门是否处于打开（监视中）状态——只有打开时才会延后。
//   · open(set)：打开门并设置被监视表集合；重复 open 会「替换」而非合并旧集合。
//   · shouldDefer(payloadTables)：当且仅当「门已打开」且「payload 表集合与被监视集合
//     有交集」时返回 true（任意命中即延后）。
//   · releaseAll()：关门并清空被监视集合（恢复到初始的「不延后任何东西」状态）。
//
// 【测试基础设施】QtTest；QTEST_APPLESS_MAIN 用于「无需 GUI/事件循环」的纯逻辑测试。
//   门控是纯内存数据结构，无需数据库或事件循环，故用 APPLESS 版本最轻量。
// ============================================================================

using namespace dbridge::sync;

class TstSyncInboundTableGate : public QObject {
    Q_OBJECT
   private slots:
    // ── 初始态：门默认关闭 ──────────────────────────────────────────────────────
    // 刚构造、未 open 的门控不应处于打开态——保证「不在比对时」绝不会误延后正常同步。
    void initialState_notOpen() {
        InboundTableGate g;
        QVERIFY(!g.isOpen());
    }

    // ── open 设置监视集合并使门变为打开 ─────────────────────────────────────────
    // WHEN open({t_a,t_b}) → THEN isOpen()==true。验证 open 确实开门。
    void open_setsWatched_isOpenTrue() {
        InboundTableGate g;
        g.open({"t_a", "t_b"});
        QVERIFY(g.isOpen());
    }

    // ── 任意命中即延后 ──────────────────────────────────────────────────────────
    // GIVEN 监视 {t_a,t_b}。 WHEN payload 涉及 {t_x,t_a}（t_a 在监视集中）。
    // THEN shouldDefer==true。验证「只要交集非空就延后」，哪怕 payload 还含无关表 t_x。
    void shouldDefer_anyHit_true() {
        InboundTableGate g;
        g.open({"t_a", "t_b"});
        QSet<QString> payload = {"t_x", "t_a"};  // t_a is watched（t_a 在被监视集合中）
        QVERIFY(g.shouldDefer(payload));
    }

    // ── 无命中不延后 ────────────────────────────────────────────────────────────
    // payload 全是未被监视的表 {t_x,t_y} → 不延后，照常立即应用。
    // 业务含义：门控只拦「正在比对的表」，对其它表的同步毫无影响（最小干预）。
    void shouldDefer_noHit_false() {
        InboundTableGate g;
        g.open({"t_a", "t_b"});
        QSet<QString> payload = {"t_x", "t_y"};
        QVERIFY(!g.shouldDefer(payload));
    }

    // ── 单表恰好命中 ────────────────────────────────────────────────────────────
    // 最小命中场景：监视集与 payload 都只含同一张表 → 必然延后。
    void shouldDefer_singleHit_true() {
        InboundTableGate g;
        g.open({"only_watched"});
        QVERIFY(g.shouldDefer({"only_watched"}));
    }

    // ── 空 payload 不延后 ───────────────────────────────────────────────────────
    // 边界：payload 表集合为空（不涉及任何表）→ 与监视集合的交集必为空 → 不延后。
    // 防止把「空变更」误判为命中而无谓地积压。
    void shouldDefer_emptyPayload_false() {
        InboundTableGate g;
        g.open({"t_a"});
        QVERIFY(!g.shouldDefer({}));
    }

    // ── releaseAll 关门并清空监视集 ─────────────────────────────────────────────
    // WHEN open 后 releaseAll() → THEN 门关闭，且原本会命中的表也不再延后。
    // 验证比对结束后门控完全复位，被延后的同步得以重新放行（不会残留旧监视集）。
    void releaseAll_closesGate() {
        InboundTableGate g;
        g.open({"t_a"});
        g.releaseAll();
        QVERIFY(!g.isOpen());              // 门已关
        QVERIFY(!g.shouldDefer({"t_a"}));  // 原命中表现在也不再延后
    }

    // ── 重复 open 替换（而非合并）旧监视集 ──────────────────────────────────────
    // GIVEN open({t_a}) 后又 open({t_b})。
    // THEN  t_a 不再延后（旧集合被丢弃），t_b 才延后（新集合生效）。
    // 不变量：open 是「设置」语义而非「追加」——切换比对目标时不会残留上一轮的监视表。
    void reopen_replacesWatched() {
        InboundTableGate g;
        g.open({"t_a"});
        g.open({"t_b"});                   // replaces（替换，而非合并）
        QVERIFY(!g.shouldDefer({"t_a"}));  // 旧表 t_a 已不在监视集
        QVERIFY(g.shouldDefer({"t_b"}));   // 新表 t_b 生效
    }
};

QTEST_APPLESS_MAIN(TstSyncInboundTableGate)
#include "tst_sync_inbound_table_gate.moc"
