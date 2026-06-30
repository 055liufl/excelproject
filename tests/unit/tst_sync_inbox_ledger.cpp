// ============================================================================
// tst_sync_inbox_ledger.cpp — InboxLedger（入站工件幂等台账）单元测试
// ============================================================================
//
// 【被测对象是什么】
//   InboxLedger 是 __sync_inbox_ledger 表（见 SyncDDL.h，G-08 修复）的类型化读写门面。
//   同步收方每见到一个入站工件（artifact，落在 inbox 目录的一个文件），都先在台账里登记其
//   处理状态，从而保证「同一工件即便被重复投递/重复扫描，也只被真正消费一次」（artifact 级
//   幂等）。这正是不可靠传输（如本项目 UDP demo、文件重扫）下「不重复应用」的护栏。
//
// 【台账的状态机（这组测试逐一钉死它的每个转换）】
//   Unknown（从未见过） ──markSeen──▶ Seen（已发现、待处理）
//        Seen ──markConsumed──▶ Consumed（已成功消费，终态，幂等护栏的关键）
//        Seen ──markCorrupt ──▶ Corrupt（损坏，不再重试，终态）
//   关键不变量：一旦 Consumed，后续重复的 markSeen 不得把它「打回」Seen（否则会被重处理）。
//
// 【两个查询型方法】
//   · pendingSeen(db)        —— 列出「仅处于 Seen」的工件（待处理队列；不含 Consumed/Corrupt）。
//   · stalePending(db, gapMs)—— 列出「Seen 且 first_seen_ms 距今超过 gapMs」的陈旧工件
//     （用于检测「发现了却迟迟没被消费」的卡壳工件，触发补洞/告警）。须排除 Consumed。
//
// 【测试夹具（fixture）：init() / cleanup()】
//   QtTest 约定：名为 init()/cleanup() 的特殊 slot 在【每个】测试用例前后自动各跑一次。
//   本套用它们为每个用例准备一个全新的内存库（:memory:），用例间互不污染、彼此独立。
//   - init()    ：建一个唯一命名的 QSQLITE 内存连接，执行 allCreateStatements() 建齐 __sync_* 表。
//   - cleanup() ：先把 db_ 句柄置默认值释放，再 close + removeDatabase，避免 Qt 的
//                 "connection still in use" 告警与连接泄漏（每个用例独占一条连接）。
//   注意：用例本身在每次都新 new 一个 InboxLedger 并 init(db_)——因为台账状态全在库里（无实例
//   状态），新实例 + 同一张表即可复用；这也顺带验证了 InboxLedger 的「无状态、可重入」设计。
//
// 【命名空间】InboxLedger / LedgerStatus 在 dbridge::sync，故 using 之。
// 断言：QVERIFY 校验布尔；QCOMPARE 比较相等并在失败时打印两侧。
// ============================================================================
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/transport/InboxLedger.h"

using namespace dbridge::sync;

class TstSyncInboxLedger : public QObject {
    Q_OBJECT
    QString conn_;  // 本用例专用连接名（每个用例唯一，避免与其它用例/历史连接撞名）
    QSqlDatabase db_;  // 本用例专用的内存库连接

   private slots:
    // 夹具：每个用例【前】建一个干净的内存库并建齐同步元数据表。
    void init() {
        // 连接名带 UUID 片段，保证每个用例独占一条连接（QtTest 用例顺序执行但仍要彼此隔离）。
        conn_ =
            QStringLiteral("tst_il_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));  // 内存库：跑完即弃，零磁盘副作用
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        // 建齐全部 __sync_* 表（含本测试关心的 __sync_inbox_ledger）。逐条执行，全部 IF NOT
        // EXISTS。
        for (const QString& s : dbridge::sync::ddl::allCreateStatements())
            q.exec(s);
    }
    // 夹具：每个用例【后】安全拆除连接，防止句柄泄漏与 Qt 告警。
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase
                               // 先把成员句柄重置为默认（断开本地对连接的引用），再 remove。
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // 用例：init() 在已建表的库上应成功（基本可用性烟雾测试）。
    // init() 内部通常做一次「表存在性」探针；表已由夹具建好 → 应返回 true。
    void init_ok() {
        InboxLedger ledger;
        QString err;
        QVERIFY(ledger.init(db_, &err));
    }

    // 用例：从未登记过的工件，status() 应返回 Unknown。
    // 这是幂等判断的起点——只有 Unknown 的工件才会被首次处理。
    void status_unknown_whenAbsent() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        QCOMPARE(ledger.status(db_, "missing_artifact"), LedgerStatus::Unknown);
    }

    // 用例：markSeen 把一个新工件置为 Seen。验证 Unknown→Seen 转换。
    void markSeen_setsSeenStatus() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        QVERIFY(ledger.markSeen(db_, "art1.payload", &err));
        QCOMPARE(ledger.status(db_, "art1.payload"), LedgerStatus::Seen);
    }

    // 用例：markSeen 幂等——对同一工件重复 markSeen 不报错、状态仍为 Seen。
    // 现实意义：inbox 被反复扫描时，同一文件会被多次「看到」，第二次起应是无害的 no-op。
    void markSeen_idempotent() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        ledger.markSeen(db_, "art1.payload", &err);
        QVERIFY(ledger.markSeen(db_, "art1.payload", &err));  // second call is no-op
                                                              // 第二次调用为无操作（幂等）。
        QCOMPARE(ledger.status(db_, "art1.payload"), LedgerStatus::Seen);
    }

    // 用例：Seen→Consumed 转换。先 markSeen 再 markConsumed，状态应变为 Consumed。
    // Consumed 是「这个工件已成功应用完毕」的终态，是幂等护栏真正生效的标志。
    void markConsumed_transition() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        ledger.markSeen(db_, "art2.payload", &err);
        QVERIFY(ledger.markConsumed(db_, "art2.payload", &err));
        QCOMPARE(ledger.status(db_, "art2.payload"), LedgerStatus::Consumed);
    }

    // 用例：Seen→Corrupt 转换。工件校验/解码失败时标记损坏，从此不再重试。
    void markCorrupt_transition() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        ledger.markSeen(db_, "art3.payload", &err);
        QVERIFY(ledger.markCorrupt(db_, "art3.payload", &err));
        QCOMPARE(ledger.status(db_, "art3.payload"), LedgerStatus::Corrupt);
    }

    // 用例（关键不变量）：Consumed 之后再 markSeen 不得回退状态。
    // GIVEN 工件已 Seen→Consumed；
    // WHEN  再次 markSeen（模拟该文件又被扫描到了一次）；
    // THEN  状态必须仍是 Consumed —— 否则会被误判为「待处理」而重复应用，破坏幂等。
    void consumed_idempotent_skipped() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        ledger.markSeen(db_, "art4.payload", &err);
        ledger.markConsumed(db_, "art4.payload", &err);
        // re-marking seen should not change Consumed status
        // 已 Consumed 后再次 markSeen 不应改变其 Consumed 状态。
        ledger.markSeen(db_, "art4.payload", &err);
        QCOMPARE(ledger.status(db_, "art4.payload"), LedgerStatus::Consumed);
    }

    // 用例：pendingSeen() 只返回「仅 Seen」的工件，排除 Consumed 与 Corrupt。
    // GIVEN 两个停留在 Seen（seen1/seen2）、一个已 Consumed、一个已 Corrupt；
    // WHEN  查询待处理队列；
    // THEN  恰好返回 2 项且只含 seen1/seen2 —— 待处理队列不应再把已了结（消费/损坏）的工件列出。
    void pendingSeen_onlyReturnsSeen() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        ledger.markSeen(db_, "seen1.payload", &err);
        ledger.markSeen(db_, "seen2.payload", &err);
        ledger.markSeen(db_, "consumed.payload", &err);
        ledger.markConsumed(db_, "consumed.payload", &err);  // 这条将被 pendingSeen 排除
        ledger.markSeen(db_, "corrupt.payload", &err);
        ledger.markCorrupt(db_, "corrupt.payload", &err);  // 这条也将被排除

        QStringList pending = ledger.pendingSeen(db_);
        QCOMPARE(pending.size(), 2);  // 仅 seen1 + seen2
        QVERIFY(pending.contains("seen1.payload"));
        QVERIFY(pending.contains("seen2.payload"));
        QVERIFY(!pending.contains("consumed.payload"));
        QVERIFY(!pending.contains("corrupt.payload"));
    }

    // 用例：无任何 Seen 工件时 pendingSeen() 返回空（边界，防「空库误报」）。
    void pendingSeen_emptyWhenNone() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        QVERIFY(ledger.pendingSeen(db_).isEmpty());
    }

    // 用例：stalePending() 按「Seen 且 first_seen_ms 太旧」判定陈旧工件。
    // 构造手法（注意这里绕过 markSeen、直接 INSERT）：markSeen 会用「当前时刻」写
    //   first_seen_ms，无法造出「很久以前就 Seen」的样本；故直接 INSERT 一行把 first_seen_ms
    //   钉死成 1000ms（约 1970 年初，对“今天”而言极旧）来模拟一个早就发现却一直没消费的卡壳工件。
    // GIVEN 一条 first_seen_ms=1000 的 old.payload（Seen），以及一条刚 markSeen 的 fresh.payload；
    // WHEN  以 gapTimeout=1 小时查询陈旧项；
    // THEN  old 因「距今远超 1 小时」入选；fresh 因刚发现（距今 ~0）不入选。
    void stalePending_returnsOldSeen() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);

        // Directly insert a row with a very old first_seen_ms
        // 直接插入一行、把 first_seen_ms 钉成极旧值（markSeen 只能写“当下”，造不出这种样本）。
        QSqlQuery q(db_);
        q.prepare(
            QStringLiteral("INSERT INTO __sync_inbox_ledger(artifact_name,status,first_seen_ms)"
                           " VALUES(?,?,?)"));
        q.addBindValue("old.payload");
        q.addBindValue("seen");
        q.addBindValue(qint64(1000));  // epoch ms = 1000 (very old)
                                       // 纪元毫秒 = 1000（极旧，约 1970-01-01 00:00:01）。
        QVERIFY(q.exec());

        // fresh artifact
        // 一个刚刚发现的工件（first_seen_ms ≈ 当前时刻）。
        ledger.markSeen(db_, "fresh.payload", &err);

        // gapTimeout = 500ms; now - 1000 >> 500ms so "old" is stale
        // fresh was just inserted so first_seen_ms is ~now, not stale
        // 以「现在 - first_seen_ms 是否超过 gapTimeout」判定陈旧；old 远超，fresh 不超。
        qint64 gapTimeout = 1000LL * 60 * 60;  // 1 hour — "fresh" won't be stale
                                               // 阈值 1 小时——刚发现的 fresh 不会被判陈旧。
        QStringList stale = ledger.stalePending(db_, gapTimeout);
        QVERIFY(stale.contains("old.payload"));
        QVERIFY(!stale.contains("fresh.payload"));
    }

    // 用例：stalePending() 必须排除 Consumed，即使它很旧。
    // 直接 INSERT 一条很旧（first_seen_ms=1000）但已 Consumed（consumed_ms=2000）的行；
    // 即便时间上够「陈旧」，它已经处理完毕，绝不应再被当成「卡壳待补」列出 → 验证排除逻辑。
    void stalePending_excludesConsumed() {
        InboxLedger ledger;
        QString err;
        ledger.init(db_, &err);
        // old consumed
        // 一条「很旧 + 已消费」的样本：旧到足以触发陈旧，但因已 Consumed 应被排除。
        QSqlQuery q(db_);
        q.prepare(QStringLiteral(
            "INSERT INTO __sync_inbox_ledger(artifact_name,status,first_seen_ms,consumed_ms)"
            " VALUES(?,?,?,?)"));
        q.addBindValue("old_consumed.payload");
        q.addBindValue("consumed");
        q.addBindValue(qint64(1000));  // 很旧的 first_seen_ms
        q.addBindValue(qint64(2000));  // 已有 consumed_ms → 终态
        q.exec();
        QStringList stale = ledger.stalePending(db_, 1000LL * 60 * 60);
        QVERIFY(!stale.contains("old_consumed.payload"));  // 已消费 → 不在陈旧待补名单内
    }
};

QTEST_APPLESS_MAIN(TstSyncInboxLedger)
#include "tst_sync_inbox_ledger.moc"
