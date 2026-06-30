#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/anchor/OutboundAckStore.h"

// ============================================================================
// tst_sync_outbound_ack.cpp — OutboundAckStore（对外发送方向 ACK 水位表）单元测试
// ============================================================================
//
// 【被测对象】OutboundAckStore：__sync_outbound_ack 表的 DAO。它记录本节点「对各对端
//   发送方向」的两条水位——逐 origin 的「已确认水位 acked_seq」与广播的「已发送水位
//   last_sent_seq」（详尽语义见 src/sync/anchor/OutboundAckStore.h 文件头）。
//
// 【这组测试守护的关键不变量】
//   1) 缺省读 = -1：从未记录过的 (peer,origin,epoch) 读 acked_seq / lastSent 应为 -1
//      （ackedSeq_noRow_minusOne / minAckedSeq_noRows_minusOne / lastSentLocalSeq_default）。
//   2) UPSERT 写后可读：updateAcked 写入后 ackedSeq 能读回同值（updateAcked_stores）。
//   3) 水位只增不减（MAX 语义）：迟到的较小 seq 不得覆盖较大水位（updateAcked_onlyAdvances）
//      —— 这是抵御「乱序/重复 ACK 导致水位回退」的核心保证。
//   4) minAckedSeq 取全体对端最小值：作为 changelog 安全裁剪下界（minAckedSeq_acrossAllPeers）。
//   5) setPendingBaseline 落库可验：基线挂起标志确实写进 pending_baseline 列。
//   6) 已发送水位独立存取：updateLastSent / lastSentLocalSeq 走哨兵行，与 acked_seq 解耦。
//   7) 多对端多来源相互隔离：不同 (peer,origin) 的水位互不串扰（multiPeerMultiOrigin_isolated）。
//
// 【测试夹具策略】每个用例前后由 init()/cleanup() 钩子各建/拆一个独立的 :memory: SQLite，
//   并跑 SyncDDL 建好全部 __sync_* 表。内存库 + 每例独立连接 → 用例之间零状态泄漏、可并行。
// ============================================================================

using namespace dbridge::sync;

class TstSyncOutboundAck : public QObject {
    Q_OBJECT
    QString conn_;  // 本用例使用的唯一连接名（含 UUID，避免与其它用例/连接重名）
    QSqlDatabase db_;  // 本用例的内存数据库连接

   private slots:
    // ── init —— 每个测试用例「之前」自动调用的夹具：建库 + 建表 ─────────────────
    // QtTest 约定：名为 init() 的 slot 在「每个」测试用例前运行（区别于只跑一次的
    //   initTestCase）。这里为每例准备一个全新的内存库，确保用例间互不影响。
    void init() {
        // 用 UUID 拼出全局唯一连接名：取前 12 位足够避免碰撞，又不至过长。
        conn_ =
            QStringLiteral("tst_oa_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));  // 纯内存库：快、隔离、无需清理文件
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        // 跑 SyncDDL 的全部建表语句，把 __sync_* 元数据表（含 __sync_outbound_ack）建好。
        for (const QString& s : dbridge::sync::ddl::allCreateStatements())
            q.exec(s);
    }
    // ── cleanup —— 每个用例「之后」自动调用：稳妥释放并移除连接 ──────────────────
    // 释放顺序很关键：必须先让成员 db_ 失去对连接的引用（赋空 QSqlDatabase），
    //   再 close() + removeDatabase()，否则 Qt 会警告「连接仍在使用中」并拒绝移除，
    //   进而在多用例间泄漏连接句柄。
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase（先释放成员持有的句柄）
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // ── init_ok —— 探活：表已就绪时 OutboundAckStore::init 应成功 ────────────────
    // SyncDDL 已建好表，故 init() 的「WHERE 0」探针应通过，返回 true。
    void init_ok() {
        OutboundAckStore oas;
        QString err;
        QVERIFY(oas.init(db_, &err));
    }

    // ── ackedSeq_noRow_minusOne —— 无记录读 acked_seq = -1 ──────────────────────
    // 从未对 (peer1, originA, epoch1) 写过任何 ACK → 读应得 -1（语义：「尚未确认任何东西」）。
    void ackedSeq_noRow_minusOne() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        QCOMPARE(oas.ackedSeq(db_, "peer1", "originA", 1), qint64(-1));
    }

    // ── updateAcked_stores —— 写入后能读回同值（UPSERT 插入路径）─────────────────
    // 首次 updateAcked(…,10) 走「插入」分支；随后 ackedSeq 应读回 10。
    void updateAcked_stores() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        QVERIFY(oas.updateAcked(db_, "peer1", "originA", 1, 10, &err));
        QCOMPARE(oas.ackedSeq(db_, "peer1", "originA", 1), qint64(10));
    }

    // ── updateAcked_onlyAdvances —— 水位只增不减（MAX 语义）★核心不变量 ──────────
    // GIVEN 先写到 20。 WHEN 又来一个更小的 10（模拟迟到/乱序的旧 ACK）。
    // THEN  水位仍为 20，不被回退。这正是抵御乱序 ACK 误导致漏发/误删的关键保证。
    void updateAcked_onlyAdvances() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        oas.updateAcked(db_, "peer1", "A", 1, 20, &err);
        // lower seq should not overwrite（更小的 seq 不得覆盖已确认的较高水位）
        oas.updateAcked(db_, "peer1", "A", 1, 10, &err);
        QCOMPARE(oas.ackedSeq(db_, "peer1", "A", 1), qint64(20));
    }

    // ── minAckedSeq_acrossAllPeers —— 跨所有对端取最小（changelog 裁剪下界）────────
    // GIVEN 三个对端对来源 A 分别确认到 5/10/3。
    // THEN  minAckedSeq("A",1)==3（最落后的对端决定可安全裁剪的下界——木桶最短板）。
    void minAckedSeq_acrossAllPeers() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        oas.updateAcked(db_, "peer1", "A", 1, 5, &err);
        oas.updateAcked(db_, "peer2", "A", 1, 10, &err);
        oas.updateAcked(db_, "peer3", "A", 1, 3, &err);
        // minimum is 3（最小值 3 = 全体对端公认安全的裁剪下界）
        QCOMPARE(oas.minAckedSeq(db_, "A", 1), qint64(3));
    }

    // ── minAckedSeq_noRows_minusOne —— 无任何相关行时返回 -1 ────────────────────
    // 对不存在的 (origin, epoch) 求 MIN 应得 -1（语义：「没有可安全裁剪的下界」→ 不裁剪）。
    void minAckedSeq_noRows_minusOne() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        QCOMPARE(oas.minAckedSeq(db_, "unknown", 99), qint64(-1));
    }

    // ── setPendingBaseline_flagsRow —— 基线挂起标志确实落库 ─────────────────────
    // 先建一行（updateAcked），再 setPendingBaseline(peer1,true)，然后用「直接 SELECT」
    //   绕过 DAO 校验 pending_baseline 列确实被置 1。
    // 业务含义：被标记基线挂起的对端会被 minAckedSeq 排除，避免基线期把别人 changelog 误删。
    void setPendingBaseline_flagsRow() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        oas.updateAcked(db_, "peer1", "A", 1, 5, &err);
        QVERIFY(oas.setPendingBaseline(db_, "peer1", true, &err));
        // verify via direct SELECT（用裸 SQL 直查列值，独立于被测 DAO 自证）
        QSqlQuery q(db_);
        q.prepare(QStringLiteral(
            "SELECT pending_baseline FROM __sync_outbound_ack WHERE peer=? AND origin=?"));
        q.addBindValue("peer1");
        q.addBindValue("A");
        q.exec();
        q.next();
        QCOMPARE(q.value(0).toInt(), 1);  // 1 = 已挂起
    }

    // ── lastSentLocalSeq_default_minusOne —— 无哨兵行时已发送水位 = -1 ───────────
    // 从未给 peer1 广播过 → 读 lastSentLocalSeq 应得 -1（语义：「还没给它发过」）。
    void lastSentLocalSeq_default_minusOne() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        QCOMPARE(oas.lastSentLocalSeq(db_, "peer1", 1), qint64(-1));
    }

    // ── updateLastSent_stores —— 已发送水位独立于 acked_seq 存取（哨兵行）────────
    // 先写一条普通 acked 行（origin="A"），再 updateLastSent 到 42（写 '__broadcast__'
    //   哨兵行）；lastSentLocalSeq 应读回 42——证明「已发送水位」走的是独立哨兵行，
    //   与逐 origin 的 acked_seq 互不污染（对应 J-01 设计）。
    void updateLastSent_stores() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        oas.updateAcked(db_, "peer1", "A", 1, 0, &err);
        QVERIFY(oas.updateLastSent(db_, "peer1", 1, 42, &err));
        QCOMPARE(oas.lastSentLocalSeq(db_, "peer1", 1), qint64(42));
    }

    // ── multiPeerMultiOrigin_isolated —— 多对端多来源水位相互隔离 ───────────────
    // 写入三组互不相同的 (peer,origin) 水位后逐一读回，验证彼此独立、无串扰。
    // 不变量：水位以 (peer,origin,epoch) 三元组为主键，任一维不同即是另一行。
    void multiPeerMultiOrigin_isolated() {
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);
        oas.updateAcked(db_, "peer1", "A", 1, 5, &err);
        oas.updateAcked(db_, "peer1", "B", 1, 8, &err);  // 同 peer 不同 origin
        oas.updateAcked(db_, "peer2", "A", 1, 3, &err);  // 同 origin 不同 peer
        QCOMPARE(oas.ackedSeq(db_, "peer1", "A", 1), qint64(5));
        QCOMPARE(oas.ackedSeq(db_, "peer1", "B", 1), qint64(8));
        QCOMPARE(oas.ackedSeq(db_, "peer2", "A", 1), qint64(3));
    }
};

QTEST_APPLESS_MAIN(TstSyncOutboundAck)
#include "tst_sync_outbound_ack.moc"
