#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/capture/ChangelogStore.h"

// ============================================================================
// tst_sync_changelog_store.cpp — ChangelogStore（__sync_changelog 读写层）单元测试
// ============================================================================
//
// 【被测对象是什么】
//   ChangelogStore（见 src/sync/capture/ChangelogStore.h）是同步「行级变更日志」表
//   __sync_changelog 的薄读写封装。该日志逐条存档「本机捕获的变更」与「从对端转发来的
//   变更」，是后续「打包广播给各 peer」的数据源。本文件验证它的几条核心契约。
//
// 【这个测试文件守护哪些不变量（逐用例对应）】
//   1) 表能被识别（init_ok）；
//   2) 自产 append 能写入并回填自增主键 local_seq（append_basic）；
//   3) (origin, epoch, origin_seq) 唯一约束防重复入账（append_uniqueConstraint）——去重的根基；
//   4) appendForward 转发时**保留原始 origin**、不把它改写成本机（appendForward_preservesOrigin）
//      ——这是「变更溯源」正确性的关键，错了会导致回声、误判来源、水位错乱；
//   5) readRange 按 origin 锚点正确过滤、按 origin_seq 升序返回（readRange_filtersCorrectly /
//      readRange_emptyWhenNoneMatch）；
//   6) readRangeAll 广播读取时**排除指定 origin**（readRangeAll_excludesOrigin）——保证
//      「绝不把 peer 自己的变更回声给它」；
//   7) maxLocalSeq 返回最大本地序、空表返回 -1（maxLocalSeq_returnsHighestSeq）——用于初始化
//      发送水位线。
//
// 【测试框架与夹具约定】
//   Qt Test：每个 private slot 是一个用例。特殊槽 init()/cleanup() 是「每个用例前后」自动
//   运行的夹具（fixture）——注意区别于被测类的 ChangelogStore::init()（同名但语义无关）。
//   每个用例都用全新的「内存 SQLite 库(:memory:)」，彼此隔离、无副作用残留。
//   末尾 QTEST_APPLESS_MAIN：无 QApplication 的纯逻辑测试入口。
//   GIVEN/WHEN/THEN 贯穿各用例：GIVEN 建库+建表+预置数据，WHEN 调被测方法，THEN 断言结果。
// ============================================================================

using namespace dbridge::sync;

class TstSyncChangelogStore : public QObject {
    Q_OBJECT
    QString conn_;  // 本用例所用数据库连接名（每用例随机生成，避免连接重名冲突）
    QSqlDatabase db_;  // 指向该连接的句柄；所有方法在持有它的本线程内调用

   private slots:
    // ── init —— Qt Test 夹具：每个测试用例运行【前】自动调用，搭好干净的内存库 ─────
    // 做什么：生成唯一连接名 → 打开一个全新的 :memory: SQLite 库 → 执行 SyncDDL 的全部建表语句。
    // 为什么用 :memory: 且每用例新建：内存库零落盘、极快，且用例间完全隔离（一个用例写的数据
    //   不会污染下一个），是单元测试的理想夹具。连接名带 UUID 防止与其它连接撞名。
    void init() {
        conn_ =
            QStringLiteral("tst_cl_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        // 建出所有 __sync_* 元数据表（含 __sync_changelog）；ChangelogStore
        // 本身不建表，故需先建好。
        for (const QString& s : dbridge::sync::ddl::allCreateStatements())
            q.exec(s);
    }
    // ── cleanup —— Qt Test 夹具：每个测试用例运行【后】自动调用，拆掉连接 ──────────
    // 关键次序：必须先把成员句柄 db_ 置空（释放对连接的引用），再 close + removeDatabase。
    //   Qt 要求「移除连接前不得有该连接的活动副本」，否则会告警且移除不彻底（与样板里临时连接的
    //   处理同理）。内存库随连接销毁而消失，无需额外删文件。
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase（先释放句柄再移除）
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // ── init_ok —— 验证 ChangelogStore::init 能识别已建好的 __sync_changelog 表 ─────
    // GIVEN 夹具已建表；WHEN 调被测 init（内部用 `WHERE 0` 探针验结构）；THEN 返回 true。
    // 业务含义：启动期自检——表结构就绪时 init 必须成功，否则后续读写都无从谈起。
    void init_ok() {
        ChangelogStore cs;
        QString err;
        QVERIFY(cs.init(db_, &err));
    }

    // ── append_basic —— 验证「写一条自产变更」并回填自增主键 ──────────────────────
    // GIVEN 空日志；WHEN append 一条 origin=nodeA、authoritative=true 的变更；
    // THEN 返回 true 且出参 localSeq>0（说明数据库分配了有效的自增本地序）。
    // 业务含义：本地捕获到的写要能成功入账，并拿到全局 FIFO 发送序 local_seq 供后续广播定序。
    void append_basic() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        qint64 localSeq = 0;
        QVERIFY(cs.append(db_, "changeset", "nodeA", "nodeA",
                          /*originSeq=*/1, /*parentSeq=*/0,
                          /*epoch=*/100, /*schemaVer=*/1, /*schemaFp=*/"fp1", QByteArray("blob1"),
                          /*authoritative=*/true, &localSeq, &err));
        QVERIFY(localSeq > 0);  // 自增主键已分配 → 写入成功且可被定序
    }

    // ── append_uniqueConstraint —— 验证 (origin, stream_epoch, origin_seq) 唯一约束 ──
    // GIVEN 先成功写入 (A,epoch=1,originSeq=1)；WHEN 再写一条三元组完全相同（仅字节不同）的变更；
    // THEN 第二次必须失败（违反 UNIQUE 约束）。
    // 业务含义：同一来源同一纪元的同一序号只能入账一次——这是「同一变更不重复落库」的去重根基，
    //   防止重复到达的变更被广播两遍或扰乱水位计算。
    void append_uniqueConstraint() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        qint64 seq1 = 0, seq2 = 0;
        cs.append(db_, "changeset", "A", "A", 1, 0, 1, 1, "fp", QByteArray("b1"), true, &seq1,
                  &err);
        // same (origin, epoch, originSeq) must fail
        // 译：相同的 (origin, epoch, originSeq) 三元组必须失败。
        bool ok = cs.append(db_, "changeset", "A", "A", 1, 0, 1, 1, "fp", QByteArray("b2"), true,
                            &seq2, &err);
        QVERIFY(!ok);  // UNIQUE(origin, stream_epoch, origin_seq) violation（触发唯一约束冲突）
    }

    // ── appendForward_preservesOrigin —— 转发入账必须【保留原始 origin】，不改写为本机 ──
    // GIVEN 模拟收到一条来自 remoteNodeB、经 centerNode 转交的入站变更；
    // WHEN 用 appendForward 落库后，直接查 __sync_changelog 该行的 origin / source_peer；
    // THEN origin 仍是 "remoteNodeB"（最初产生者）、source_peer 是 "centerNode"（直接发件人）。
    // 业务含义（为什么是关键不变量）：origin 表示「变更最初产生于谁」，source_peer 表示「谁直接
    //   发给我」，二者语义不同绝不能混淆。若转发时把 origin 错改成本机或发件人，会破坏变更溯源、
    //   导致把变更回声给真正的来源、水位/去重全盘错乱。本用例绕过 ChangelogStore 直接读底层表
    //   断言，正是为了钉死这条持久化语义。
    void appendForward_preservesOrigin() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        qint64 localSeq = 0;
        // appendForward = inbound changeset with remote origin
        // 译：appendForward 处理的是「带远端 origin 的入站变更」（转发场景）。
        QVERIFY(cs.appendForward(db_, "remoteNodeB", "centerNode",
                                 /*originSeq=*/7, /*epoch=*/1, /*schemaVer=*/1, /*schemaFp=*/"fp",
                                 QByteArray("remote_blob"), &localSeq, &err));
        QSqlQuery q(db_);
        q.prepare(
            QStringLiteral("SELECT origin, source_peer FROM __sync_changelog WHERE local_seq=?"));
        q.addBindValue(localSeq);
        QVERIFY(q.exec() && q.next());
        QCOMPARE(q.value(0).toString(),
                 QString("remoteNodeB"));  // origin NOT recast（origin 未被改写）
        QCOMPARE(q.value(1).toString(), QString("centerNode"));  // source_peer = 直接发件人
    }

    // ── readRange_filtersCorrectly —— 验证按 origin 锚点过滤 + 按 origin_seq 升序返回 ──
    // GIVEN 同一 origin "A" 写入 originSeq 为 1/2/3 的三条；WHEN 以 afterOriginSeq=1 读取
    //   （取「序号 > 1」者）；THEN 恰返回 2 条，且顺序为 originSeq=2、3（升序）。
    // 业务含义：广播/补发时需「从某锚点之后、按序」取该 origin 的增量；过滤边界(>而非>=)与
    //   升序都必须精确，否则会漏发、重发或乱序。
    void readRange_filtersCorrectly() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        qint64 s = 0;
        cs.append(db_, "changeset", "A", "A", 1, 0, 1, 1, "fp", QByteArray("b1"), true, &s, &err);
        cs.append(db_, "changeset", "A", "A", 2, 1, 1, 1, "fp", QByteArray("b2"), true, &s, &err);
        cs.append(db_, "changeset", "A", "A", 3, 2, 1, 1, "fp", QByteArray("b3"), true, &s, &err);
        // afterOriginSeq=1 should return seq 2 and 3
        // 译：afterOriginSeq=1 应返回 originSeq 为 2 和 3 的两条（严格大于 1）。
        auto entries = cs.readRange(db_, "A", 1);
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries[0].originSeq, qint64(2));  // 升序第一条
        QCOMPARE(entries[1].originSeq, qint64(3));  // 升序第二条
    }

    // ── readRange_emptyWhenNoneMatch —— 验证无匹配来源时返回空列表（而非报错/崩溃）─────
    // GIVEN 空日志；WHEN 读取一个从不存在的 origin "unknown"；THEN 返回空 list。
    // 业务含义：查询陌生来源是常态（对端从没发过东西），必须安静返回空而非异常。
    void readRange_emptyWhenNoneMatch() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        auto entries = cs.readRange(db_, "unknown", 0);
        QVERIFY(entries.isEmpty());
    }

    // ── readRangeAll_excludesOrigin —— 验证广播读取时【排除指定 origin】(防回声) ─────
    // GIVEN 三个不同 origin A/B/C 各写一条；WHEN 以 excludeOrigin="A"、afterLocalSeq=-1
    //   （即从头读全部）调 readRangeAll；THEN 只返回 B、C 两条，绝不含 A。
    // 业务含义：向某 peer 广播时，把它自己的 origin 设为 excludeOrigin，从而「绝不把它的变更
    //   回声给它」，同时带上其它所有来源（含本机自产）的变更。这条直接对应 J-01 修复的语义。
    void readRangeAll_excludesOrigin() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        qint64 s = 0;
        cs.append(db_, "changeset", "A", "A", 1, 0, 1, 1, "fp", QByteArray("ba"), true, &s, &err);
        cs.append(db_, "changeset", "B", "B", 1, 0, 1, 1, "fp", QByteArray("bb"), true, &s, &err);
        cs.append(db_, "changeset", "C", "C", 1, 0, 1, 1, "fp", QByteArray("bc"), true, &s, &err);
        // exclude origin="A" → only B and C
        // 译：排除 origin="A" → 仅剩 B 与 C。
        auto entries = cs.readRangeAll(db_, "A", /*afterLocalSeq=*/-1);
        QCOMPARE(entries.size(), 2);
        for (const auto& e : entries)
            QVERIFY(e.origin != "A");  // 逐条断言：结果中绝无被排除的 A
    }

    // ── maxLocalSeq_returnsHighestSeq —— 验证「最大本地序」查询（空表 -1，否则取最大）──
    // GIVEN 空日志 → maxLocalSeq 应为 -1；WHEN 再写入两条；THEN maxLocalSeq 变为 2。
    // 业务含义：启动时用它初始化各 peer 的 last_sent 发送水位（J-01）。空表必须返回 -1 这个
    //   哨兵值（表示「还没有任何变更」），有数据时返回真实的最大 local_seq。
    void maxLocalSeq_returnsHighestSeq() {
        ChangelogStore cs;
        QString err;
        cs.init(db_, &err);
        QCOMPARE(cs.maxLocalSeq(db_), qint64(-1));  // empty → -1（空表哨兵值）
        qint64 s = 0;
        cs.append(db_, "changeset", "A", "A", 1, 0, 1, 1, "fp", QByteArray("b1"), true, &s, &err);
        cs.append(db_, "changeset", "A", "A", 2, 1, 1, 1, "fp", QByteArray("b2"), true, &s, &err);
        QCOMPARE(cs.maxLocalSeq(db_), qint64(2));  // 两条后最大本地序为 2
    }
};

QTEST_APPLESS_MAIN(TstSyncChangelogStore)
#include "tst_sync_changelog_store.moc"
