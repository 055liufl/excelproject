#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/apply/AppliedVectorStore.h"

// ============================================================================
// tst_sync_applied_vector.cpp — AppliedVectorStore（已应用序列向量）的单元测试
// ============================================================================
//
// 【被测对象是什么】
//   AppliedVectorStore 维护「每个 (origin, epoch) 流我已应用到第几个 origin_seq」这条
//   水位线（即 applied vector），落盘在 __sync_applied_vector 元表里。它是入站方向去重
//   与「补洞(gap)」判定的核心：每收到一条变更，先用 check() 问「这条 seq 现在该怎么办」，
//   答案只有三种（SeqCheckResult）：
//     · Apply —— seq 正好是「当前水位 +1」，是下一条该应用的；
//     · NoOp  —— seq <= 当前水位，早已应用过 → 幂等跳过（重复投递是常态，不能当错误）；
//     · Gap   —— seq > 水位 +1，中间缺了更早的若干条 → 出现空洞，须等补齐而非贸然应用。
//   应用成功后再调 advance() 把水位推进到该 seq。reset()/resetTo() 用于基线重置时回退水位。
//
// 【为什么要严格「连续」而不是「只要更大就应用」】
//   SQLite changeset 之间可能有依赖（如先 INSERT 父行再 INSERT 子行）。若允许跳号应用，
//   后到的变更可能依赖尚未到达的前序变更，导致外键失败或数据错位。故水位只能 +1 递进，
//   缺号时停下等待——这正是这些用例反复在验证的不变量。
//
// 【测试夹具与生命周期】
//   每个测试前 init() 建一个独立的内存库(:memory:)，跑 ddl::allCreateStatements() 建齐
//   所有 __sync_* 元表；测试后 cleanup() 释放并 removeDatabase。每条连接用 UUID 取唯一名，
//   保证用例之间互不串扰（内存库随连接关闭即销毁，天然隔离）。
//   备注：QSqlDatabase 连接不可跨线程，本测试全程单线程，无并发考量。
// ============================================================================

using namespace dbridge::sync;

class TstSyncAppliedVector : public QObject {
    Q_OBJECT
    QString conn_;  // 本用例专属连接名（UUID 派生，避免与其它用例/连接重名）
    QSqlDatabase db_;  // 指向内存库的连接句柄

    // setupDb —— 夹具搭建：开一个全新内存库并建齐全部 __sync_* 元表。
    // 为什么用 :memory: + 唯一连接名：每个用例拿到一张干净空表，互不影响；连接关闭即回收，
    //   无需手工清表。建表 SQL 直接取自生产 DDL（ddl::allCreateStatements），保证测的是真表结构。
    void setupDb() {
        conn_ =
            QStringLiteral("tst_av_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        for (const QString& s : dbridge::sync::ddl::allCreateStatements())
            q.exec(s);
    }

   private slots:
    // QtTest 约定：init()/cleanup() 在「每个」测试槽前后自动各跑一次（区别于全局
    //   initTestCase/cleanupTestCase 只跑一次）。这里用它实现「每用例一张干净库」。
    void init() {
        setupDb();
    }
    void cleanup() {
        // 必须先把 db_ 置空、断开本地句柄持有，再 removeDatabase——否则 Qt 会警告
        //   「连接仍在使用中」并拒绝移除（QSqlDatabase 句柄计数未归零）。顺序不可颠倒。
        db_ = QSqlDatabase();  // release handle before removeDatabase（先释放句柄）
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // ── init_ok —— 契约：init() 仅探测元表存在即返回成功 ───────────────────────
    // GIVEN 一个已建齐 __sync_* 表的库；WHEN 调 av.init()；THEN 返回 true（表就绪）。
    // 断言失败时用 qPrintable(err) 把驱动错误文本打进 QtTest 输出，便于定位。
    // init() only checks table exists（init 只校验表存在）
    void init_ok() {
        AppliedVectorStore av;
        QString err;
        QVERIFY2(av.init(db_, &err), qPrintable(err));
    }

    // ── check_freshOrigin_seq1_Apply —— 全新 origin 的第一条须从 seq==1 起 ──────
    // GIVEN 从未见过的 origin "A"（无水位记录，隐含水位=0）；WHEN 收到 seq=1；
    // THEN 结果为 Apply（1 == 0+1，正是该应用的第一条）。这是「流的起点」契约。
    // First ever seq==1 for unknown origin → Apply
    void check_freshOrigin_seq1_Apply() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        QCOMPARE(av.check(db_, "A", 1, 1, &err), SeqCheckResult::Apply);
    }

    // ── check_freshOrigin_seqGt1_Gap —— 全新 origin 一上来就跳号即 Gap ──────────
    // GIVEN 全新 origin（水位=0）；WHEN 第一条就是 seq=2（或 99）；THEN 判为 Gap——
    //   因为 seq 1（及 1..98）尚未到达，不能从中间开始应用，须等前序补齐。
    //   两次断言覆盖「刚跳一格」和「跳很多格」两种缺口规模，行为应一致。
    // First ever seq>1 for unknown origin → Gap
    void check_freshOrigin_seqGt1_Gap() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        QCOMPARE(av.check(db_, "A", 1, 2, &err), SeqCheckResult::Gap);
        QCOMPARE(av.check(db_, "A", 1, 99, &err), SeqCheckResult::Gap);
    }

    // ── check_strictSequential —— 核心不变量：严格连续 + 已应用幂等 ────────────
    // 用一条完整的「应用流水」验证三态联动：
    //   seq=1 → Apply → advance（水位推到 1）
    //   seq=2 → Apply → advance（水位推到 2）
    //   再问 seq=1 / seq=2 → 均 NoOp（<=水位，早已应用，重复投递须幂等跳过）。
    // 这串断言背后的业务含义：重发/乱序到达的旧变更绝不能被二次应用，否则数据被重复写。
    // seq == applied+1 → Apply; advance; seq <= applied → NoOp
    void check_strictSequential() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);

        // apply seq=1
        QCOMPARE(av.check(db_, "A", 1, 1, &err), SeqCheckResult::Apply);
        QVERIFY(av.advance(db_, "A", 1, 1, &err));

        // seq=2 is next → Apply
        QCOMPARE(av.check(db_, "A", 1, 2, &err), SeqCheckResult::Apply);
        QVERIFY(av.advance(db_, "A", 1, 2, &err));

        // seq=1 again → NoOp (already applied)
        QCOMPARE(av.check(db_, "A", 1, 1, &err), SeqCheckResult::NoOp);
        QCOMPARE(av.check(db_, "A", 1, 2, &err), SeqCheckResult::NoOp);
    }

    // ── check_gap —— 跳号产生 Gap，且「缺口不推进水位」是关键 ──────────────────
    // GIVEN 已应用到 seq=1（水位=1）；WHEN 来的是 seq=3（跳过了 2）；THEN 判 Gap。
    // 关键二次断言：随后再问 seq=2 仍是 Apply——说明刚才那次 Gap 没有把水位错误地推到 3。
    //   若水位被 Gap 误推，补来的 seq=2 会被当成 NoOp 而永久丢失，这正是要防的 bug。
    // seq > applied+1 → Gap (缺口不推进水位)
    void check_gap() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        av.check(db_, "A", 1, 1, &err);
        av.advance(db_, "A", 1, 1, &err);
        // skip seq=2, deliver seq=3 → Gap（跳过 2 直接来 3 → 缺口）
        QCOMPARE(av.check(db_, "A", 1, 3, &err), SeqCheckResult::Gap);
        // seq=2 is still Apply (water mark did NOT advance to 3)
        // —— seq=2 仍可应用：证明 Gap 未推进水位（否则补来的 2 会被吞掉）。
        QCOMPARE(av.check(db_, "A", 1, 2, &err), SeqCheckResult::Apply);
    }

    // ── check_multipleOrigins_independent —— 各 origin 的水位彼此隔离 ──────────
    // GIVEN origin A 已应用到 seq=1；WHEN 全新 origin B 收到它的 seq=1；
    // THEN B 的 seq=1 仍是 Apply——A 的进度绝不影响 B。水位的键是 (origin, epoch)，
    //   每个来源各记各的，这条用例守的就是「水位按 origin 维度分桶」这一不变量。
    // multiple origins are independent
    void check_multipleOrigins_independent() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        // origin A at seq=1（A 推进到 1）
        QCOMPARE(av.check(db_, "A", 1, 1, &err), SeqCheckResult::Apply);
        av.advance(db_, "A", 1, 1, &err);
        // origin B still fresh, seq=1 → Apply（B 不受 A 影响，仍从 1 起）
        QCOMPARE(av.check(db_, "B", 1, 1, &err), SeqCheckResult::Apply);
    }

    // ── current_noRow_returnsMinusOne —— 无记录时 current() 约定返回 -1 ────────
    // GIVEN 从未写过水位的 origin "X"；WHEN 查 current()；THEN 返回 -1（而非 0）。
    //   -1 表示「尚无任何已应用记录」，与「已应用到 seq 0」语义不同，调用方据此区分。
    // current() returns -1 if no row, or applied_seq otherwise
    void current_noRow_returnsMinusOne() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        QCOMPARE(av.current(db_, "X", 1), qint64(-1));
    }

    // ── current_afterAdvance —— current() 如实反映最近一次 advance 后的水位 ─────
    // 连续 apply+advance 到 seq=1 再到 seq=2，每步后 current() 应分别读出 1、2。
    // 验证 advance 确实把水位持久化、current 确实读回该值（写读一致）。
    void current_afterAdvance() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        av.check(db_, "A", 1, 1, &err);
        av.advance(db_, "A", 1, 1, &err);
        QCOMPARE(av.current(db_, "A", 1), qint64(1));
        av.check(db_, "A", 1, 2, &err);
        av.advance(db_, "A", 1, 2, &err);
        QCOMPARE(av.current(db_, "A", 1), qint64(2));
    }

    // ── reset_zeroes —— 基线重置把水位清零、并提升 baseline_generation ────────
    // GIVEN A 已应用到 seq=1；WHEN reset(...,baselineGen=2)（模拟「该 origin 做了基线重置」）；
    // THEN current() 归 0，且 seq=1 再次变成 Apply——即「整条流从头来过」。
    //   baselineGen 递增用于隔离新旧两代流：重置后新流的 seq 即便与旧流重号也不会被误判为已应用。
    // reset() zeroes applied_seq and bumps baseline_generation
    void reset_zeroes() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        av.check(db_, "A", 1, 1, &err);
        av.advance(db_, "A", 1, 1, &err);
        QVERIFY(av.reset(db_, "A", 1, /*baselineGen=*/2, &err));
        QCOMPARE(av.current(db_, "A", 1), qint64(0));
        // after reset, seq=1 is Apply again（重置后 seq=1 重新可应用）
        QCOMPARE(av.check(db_, "A", 1, 1, &err), SeqCheckResult::Apply);
    }

    // ── resetTo_specificSeq —— 把水位「截断」回退到指定 seq ────────────────────
    // GIVEN A 连续应用到 seq=5；WHEN resetTo(originSeq=3)（回退到第 3 条之后）；
    // THEN current() 变为 3。区别于 reset()（清零重来），resetTo() 用于「部分回卷」——
    //   例如基线只覆盖到某个 seq，需把水位精确对齐到该点，后续从 seq=4 继续增量应用。
    // resetTo() truncates to a specific seq
    void resetTo_specificSeq() {
        AppliedVectorStore av;
        QString err;
        av.init(db_, &err);
        for (int i = 1; i <= 5; ++i) {  // 先把水位推到 5
            av.check(db_, "A", 1, i, &err);
            av.advance(db_, "A", 1, i, &err);
        }
        QCOMPARE(av.current(db_, "A", 1), qint64(5));
        QVERIFY(av.resetTo(db_, "A", 1, /*originSeq=*/3, /*baselineGen=*/1, &err));
        QCOMPARE(av.current(db_, "A", 1), qint64(3));  // 水位被精确截断到 3
    }
};

QTEST_APPLESS_MAIN(TstSyncAppliedVector)
#include "tst_sync_applied_vector.moc"
