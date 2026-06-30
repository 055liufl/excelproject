// ============================================================================
// tst_sync_dead_peer_evictor.cpp — DeadPeerEvictor（失联对端探测/驱逐）单元测试
// ============================================================================
//
// 【被测对象 DeadPeerEvictor 做什么】
//   同步是「推给所有对端、收齐 ACK 才能裁剪 changelog」。但若某个对端长期失联（崩了、
//   下线了、网络断了），它的 acked_seq 永远不前进，会把 minAckedSeq 一直压在低位，导致
//   本节点的 changelog 永远删不掉、无限膨胀。DeadPeerEvictor 就是「健康哨兵」：根据某对端
//   的「落后程度」给它定级，并在确认「死亡」时把它驱逐——驱逐＝标记 pending_baseline=1，
//   使其暂时退出 minAckedSeq 裁剪计算（详见 OutboundAckStore.minAckedSeq 注释），从而解救
//   被它卡住的日志裁剪；待它重新入网再走基线对齐。
//
// 【三档告警级别（AlertLevel）与三类「落后维度」】
//   评估一个对端是否健康，看三个维度，各有「软阈值 soft / 硬阈值 hard」两道线：
//     · lagSeq   —— 落后的「序号数」（还差多少条 changelog 没确认）；
//     · lagBytes —— 落后的「字节数」（未确认数据的体量）；
//     · 距上次 ACK 的「毫秒数」elapsed = now - lastAckMs（多久没回过 ACK 了）。
//   级别取「三个维度里最严重的那个」：
//     Healthy（健康）  —— 三维都在各自 soft 之下；
//     Lagging（落后）  —— 至少一维越过 soft、但都没越过 hard（预警，先观察）；
//     Dead（死亡）     —— 至少一维越过 hard（判定失联 → 应驱逐）。
//
// 【这些测试在验证什么】
//   evaluate_* 系列：逐一钉住「分级阈值逻辑」这一核心契约——每个维度单独越过 hard 都应判 Dead，
//     越过 soft（未到 hard）应判 Lagging，全在 soft 下应判 Healthy；外加「时间维度需有有效
//     lastAckMs 才参与判定」这一边界（hasTimeData）。
//   evict_* ：验证「驱逐动作」确实把 OutboundAckStore 里该对端的 pending_baseline 落库为 1。
//   defaultThresholds_*：验证「不显式 configure 时」的默认阈值是合理的（轻微落后仍算健康）。
//
// 【夹具】每个测试用例前后由 init()/cleanup() 各建/拆一个建好 __sync_* 表的内存库
//   （QtTest 约定：init()/cleanup() 在「每个」测试函数前后自动跑，区别于只跑一次的
//   initTestCase()/cleanupTestCase()）。
// ============================================================================
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
    QString conn_;     // 本测试用例当前所用的连接名（每个用例唯一）
    QSqlDatabase db_;  // 当前内存库连接（init 打开、cleanup 释放）

    // makePeer —— 测试辅助：快速拼一个 PeerState（被评估对端的「落后画像」快照）。
    //   把构造 PeerState 的样板收成一行，让各测试只需关注真正要验证的那几个数值。
    //   参数：peer 对端 id；lagSeq 落后序号数；lagBytes 落后字节数；lastAckMs 最近一次 ACK 的
    //         绝对时间戳（毫秒）。evicted 一律初始化为 false（尚未被驱逐）。
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
    // init —— 每个测试函数运行「前」自动调用：开一个内存库并建好全部 __sync_* 表。
    //   连接名拼 UUID 保证唯一；逐条执行 SyncDDL 建表（DeadPeerEvictor/OutboundAckStore 要读写
    //   __sync_outbound_ack 等表，故底座必须先建好）。
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
    // cleanup —— 每个测试函数运行「后」自动调用：释放连接，避免泄漏与连接名冲突。
    //   关键顺序：先把 db_ 赋值为默认构造的空连接（释放本测试持有的活动句柄），
    //   再 close + removeDatabase——否则 Qt 会因「移除时仍有活动句柄」而告警、移除不彻底。
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase
                               // （先释放句柄，再移除连接）
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // ── evaluate：分级阈值逻辑（核心契约）─────────────────────────────────────

    // GIVEN 一组 soft/hard 阈值 + 一个「三维都远低于 soft」的对端，
    // WHEN  在 now=10000ms 评估，
    // THEN  应判 Healthy。这是「基线健康」用例，确立其它用例的对照。
    void evaluate_healthy_whenBelowAllThresholds() {
        DeadPeerEvictor ev;
        // 显式配置六个阈值：seq 软 1000/硬 10000；bytes 软 1MB/硬 10MB；ms 软 60s/硬 600s。
        ev.configure(/*softSeq=*/1000, /*hardSeq=*/10000,
                     /*softBytes=*/1024 * 1024, /*hardBytes=*/10 * 1024 * 1024,
                     /*softMs=*/60000, /*hardMs=*/600000);
        // lagSeq=5, lagBytes=100, lastAckMs 1s ago → healthy
        // 落后 5 条 / 100 字节 / 1 秒前刚 ACK 过（elapsed=1000ms<<softMs）→ 三维全在 soft 下。
        auto ps = makePeer("A", 5, 100, /*nowMs=*/10000 - 1000);
        QCOMPARE(ev.evaluate(ps, 10000), DeadPeerEvictor::AlertLevel::Healthy);
    }

    // 单独验证「序号维度」越过 soft（但未到 hard）→ Lagging（预警档）。
    // 其余两维仍健康，故级别完全由 lagSeq 这一维度决定，隔离了被测逻辑。
    void evaluate_lagging_whenAboveSoftThreshold() {
        DeadPeerEvictor ev;
        ev.configure(1000, 10000, 1024 * 1024, 10 * 1024 * 1024, 60000, 600000);
        // lagSeq > softSeq → lagging
        // lagSeq=2000 介于 softSeq(1000) 与 hardSeq(10000) 之间 → 预警，但还不到死亡。
        auto ps = makePeer("A", 2000, 100, 10000 - 1000);
        QCOMPARE(ev.evaluate(ps, 10000), DeadPeerEvictor::AlertLevel::Lagging);
    }

    // 单独验证「序号维度」越过 hard → Dead（死亡档）。
    void evaluate_dead_whenAboveHardSeq() {
        DeadPeerEvictor ev;
        ev.configure(1000, 10000, 1024 * 1024, 10 * 1024 * 1024, 60000, 600000);
        auto ps = makePeer("A", 20000, 100, 10000 - 1000);  // lagSeq > hardSeq
                                                            // （lagSeq=20000 > hardSeq=10000）
        QCOMPARE(ev.evaluate(ps, 10000), DeadPeerEvictor::AlertLevel::Dead);
    }

    // 单独验证「时间维度」越过 hard → Dead，并钉住一个边界：
    // 时间维度只有在 lastAckMs>0（hasTimeData=true，确有过 ACK 记录）时才参与判定——
    // 否则「从未 ACK 过」的对端不该仅凭「没时间数据」就被误判死亡。这里给 lastAckMs=1（>0，
    // 有效），now=700001，elapsed=700000ms 远超 hardMs=600000 → Dead。注意 seq/bytes 都为 0，
    // 确保 Dead 完全来自时间维度，而非其它维度。
    void evaluate_dead_whenAboveHardMs() {
        DeadPeerEvictor ev;
        ev.configure(1000, 10000, 1024 * 1024, 10 * 1024 * 1024, 60000, 600000);
        // lastAckMs must be > 0 for hasTimeData=true; set to 1 ms ago nowMs=700001
        // elapsed = 700001 - 1 = 700000 ms >> hardMs=600000 → Dead
        auto ps = makePeer("A", 0, 0, /*lastAckMs=*/1);
        QCOMPARE(ev.evaluate(ps, 700001), DeadPeerEvictor::AlertLevel::Dead);
    }

    // 单独验证「字节维度」越过 hard → Dead。lagBytes=20MB > hardBytes=10MB；seq/时间均健康。
    void evaluate_dead_whenAboveHardBytes() {
        DeadPeerEvictor ev;
        ev.configure(1000, 10000, 1024 * 1024, 10 * 1024 * 1024, 60000, 600000);
        // lagBytes > hardBytes
        auto ps = makePeer("A", 0, 20 * 1024 * 1024, 10000 - 1000);
        QCOMPARE(ev.evaluate(ps, 10000), DeadPeerEvictor::AlertLevel::Dead);
    }

    // ── evict：驱逐动作的副作用 ───────────────────────────────────────────────
    // GIVEN OutboundAckStore 里已有 peer1 的一行 ack 记录，
    // WHEN  对 peer1 执行 evict()，
    // THEN  ① evict 返回 true（成功）；② 直接查库确认该对端的 pending_baseline 已落库为 1。
    //   业务含义：驱逐＝把对端挂起，使其退出 minAckedSeq 裁剪计算，解救被它卡住的 changelog。
    //   本用例「绕过封装、直接 SELECT 校验列值」，验证的是「写库这一真实副作用」而非内存状态。
    void evict_setsPendingBaseline() {
        DeadPeerEvictor ev;
        ev.configure(1000, 10000, 1024 * 1024, 10 * 1024 * 1024, 60000, 600000);
        OutboundAckStore oas;
        QString err;
        oas.init(db_, &err);  // 自检表存在
        // 先制造一行 ack 记录（peer1 / origin "A" / epoch 1 / ackedSeq 5），evict 才有行可更新。
        oas.updateAcked(db_, "peer1", "A", 1, 5, &err);
        QVERIFY(ev.evict(db_, "peer1", oas, &err));  // 驱逐应成功
        // pending_baseline should be set
        // 直接查 __sync_outbound_ack：peer1 那行的 pending_baseline 应已被置为 1。
        QSqlQuery q(db_);
        q.prepare("SELECT pending_baseline FROM __sync_outbound_ack WHERE peer=?");
        q.addBindValue("peer1");
        q.exec();
        q.next();
        QCOMPARE(q.value(0).toInt(), 1);  // 1 = 已挂起（pending_baseline 置位成功）
    }

    // ── 默认阈值的合理性 ──────────────────────────────────────────────────────
    // 不显式 configure（用内置默认阈值）时，一个「仅落后 1 条、1KB、刚 100ms 前 ACK 过」的对端
    // 应判 Healthy——确保默认阈值不会过敏感，把正常运行的对端误报为落后/死亡。
    void defaultThresholds_reasonable() {
        DeadPeerEvictor ev;  // default thresholds
                             // （不调 configure，使用默认阈值）
        // With default thresholds, a peer with lagSeq=1 and recent lastAck should be Healthy
        auto ps = makePeer("A", 1, 1024, 5000 - 100);
        auto level = ev.evaluate(ps, 5000);
        QCOMPARE(level, DeadPeerEvictor::AlertLevel::Healthy);
    }
};

QTEST_APPLESS_MAIN(TstSyncDeadPeerEvictor)
#include "tst_sync_dead_peer_evictor.moc"
