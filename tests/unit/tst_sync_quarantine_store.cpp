// ============================================================================
// tst_sync_quarantine_store.cpp — QuarantineStore（隔离区存储）的单元测试
// ============================================================================
//
// 【被测对象在系统中的角色】
//   节点收到对端 changeset 时，可能因为「本地 schema 版本还低于该包要求的版本」而
//   暂时无法应用（对端 schema 已升级，本地尚未跟上）。直接丢弃会漏数据，立即报错又
//   不合理——正确做法是把这类「来自未来」的 payload 暂存进隔离区（__sync_quarantine
//   表），等本地 schema 升级到位后再「重放」(replay) 套用。QuarantineStore 就是这个
//   隔离区的数据访问层，提供四个核心动作：
//     · quarantine()    —— 把一个暂不可应用的 payload 连同其 (origin, seq, epoch,
//                          schemaVer) 存入隔离表；
//     · drainReady()    —— 取出「所需 schemaVer ≤ 当前 schemaVer」即此刻已可应用的条目；
//     · markReplayed()  —— 某条目重放成功后从隔离表删除（避免重复套用）；
//     · init()          —— 表健在性自检。
//
// 【本测试验证的契约（不变量）】
//   1) init 在表已建好时返回成功；
//   2) quarantine 确实落了一行；
//   3) drainReady 的「成熟度」判定正确：requiredSchemaVer ≤ currentSchemaVer 才返回，
//      高于当前版本（来自更未来的 schema）的不返回；多版本混存时只放行已成熟的；
//   4) markReplayed 删除后该条目不再被 drainReady 取到（幂等、不重放）。
//
// 【测试夹具策略】
//   每个用例用独立的「内存 SQLite 库」(:memory:)，连接名带 UUID 避免并发用例互相串库；
//   init()/cleanup() 是 QtTest 的每用例钩子（每个 slot 前后各跑一次），保证用例之间
//   完全隔离、互不影响。建表用项目权威 DDL（ddl::allCreateStatements）以贴近真实结构。
// ============================================================================

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/schema/QuarantineStore.h"

using namespace dbridge::sync;

// TstSyncQuarantineStore —— QuarantineStore 测试套件。
// 成员 conn_/db_ 是「每个用例一份」的内存库连接（由 init()/cleanup() 管理生命周期）。
class TstSyncQuarantineStore : public QObject {
    Q_OBJECT
    QString conn_;     // 本用例的 SQLite 连接名（带 UUID，全局唯一）
    QSqlDatabase db_;  // 本用例的内存数据库连接

   private slots:
    // init —— QtTest 每用例前置钩子：搭一个干净的内存库并建好全部 __sync_* 表。
    // 为什么用 :memory: + 唯一连接名：内存库无需清理磁盘、跑得快；唯一名防止多个用例
    //   （乃至并发）共用同一连接而互相污染。建表用项目真实 DDL，保证测的是真实表结构。
    void init() {
        conn_ =
            QStringLiteral("tst_qs_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        // 执行全部建表语句（含 __sync_quarantine）。这里不逐条断言成功——只要目标表建出来
        // 即可，后续用例自会通过行为验证表是否可用。
        for (const QString& s : dbridge::sync::ddl::allCreateStatements())
            q.exec(s);
    }
    // cleanup —— QtTest 每用例后置钩子：彻底释放并移除本用例的数据库连接。
    // 关键次序：必须先把 db_ 置空（让本地 QSqlDatabase 句柄析构、释放对连接的引用），
    //   再调 removeDatabase——否则 Qt 会警告「仍有打开的引用」并可能泄漏连接。
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase（先释放句柄）
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // init_ok —— 表已由夹具建好时，init() 自检应返回成功。
    // GIVEN init() 钩子已执行全部 DDL（__sync_quarantine 已存在）；
    // WHEN 调 QuarantineStore::init；THEN 返回 true（表健在、可用）。
    void init_ok() {
        QuarantineStore qs;
        QString err;
        QVERIFY(qs.init(db_, &err));
    }

    // quarantine_storesRow —— quarantine() 必须真的往隔离表写入一行。
    // GIVEN 空隔离表；WHEN 隔离一条来自 nodeA、seq=5、epoch=1、schemaVer=3 的 payload；
    // THEN 表里恰好有 1 行。这里用裸 SQL COUNT(*) 直接查表，绕过被测类自身的读取路径，
    //   以「外部独立视角」确认写入真实发生（避免用被测代码验证被测代码的循环论证）。
    void quarantine_storesRow() {
        QuarantineStore qs;
        QString err;
        qs.init(db_, &err);
        QVERIFY(qs.quarantine(db_, "nodeA", /*seq=*/5, /*epoch=*/1,
                              /*schemaVer=*/3, QByteArray("payload_data"), &err));
        QSqlQuery q(db_);
        q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_quarantine"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);  // 恰好落了一行
    }

    // drainReady_returnsMatchingSchemaVer —— 所需版本「等于」当前版本，应成熟可取。
    // GIVEN 隔离了一条 requiredSchemaVer=3 的 payload；
    // WHEN 以 currentSchemaVer=3 调 drainReady；THEN 返回它（边界：相等也算「已成熟」）。
    // 业务含义：本地 schema 已升到该包要求的版本，此刻可以安全重放。
    void drainReady_returnsMatchingSchemaVer() {
        QuarantineStore qs;
        QString err;
        qs.init(db_, &err);
        // store with schemaVer=3（存入一条要求版本 3 的条目）
        qs.quarantine(db_, "A", 1, 1, 3, QByteArray("data_v3"), &err);
        // drainReady with currentSchemaVer=3 → returns it（当前版本=3，恰好成熟）
        auto items = qs.drainReady(db_, 3);
        QCOMPARE(items.size(), 1);
    }

    // drainReady_excludesHigherSchemaVer —— 所需版本「高于」当前版本，尚不可取。
    // GIVEN 隔离了一条 requiredSchemaVer=5 的「来自更未来」的 payload；
    // WHEN 以 currentSchemaVer=3 调 drainReady；THEN 返回空（本地还没升到 5，不能套用）。
    // 这是隔离区存在的根本理由：把「来自未来」的变更挡住、原地等待，而不是丢弃或误应用。
    void drainReady_excludesHigherSchemaVer() {
        QuarantineStore qs;
        QString err;
        qs.init(db_, &err);
        qs.quarantine(db_, "A", 1, 1, 5, QByteArray("future"), &err);
        // currentSchemaVer=3 < quarantined schemaVer=5 → not ready（未成熟，不返回）
        auto items = qs.drainReady(db_, 3);
        QCOMPARE(items.size(), 0);
    }

    // drainReady_multipleVersions —— 多版本混存时，只放行「已成熟」的子集。
    // GIVEN 隔离三条，所需版本分别为 2、4、6；WHEN 以 currentSchemaVer=4 调 drainReady；
    // THEN 只返回 2 条（v2、v4 满足 ≤4；v6 仍属未来被挡）。
    // 验证「成熟度过滤」是逐条按 requiredSchemaVer ≤ currentSchemaVer 判定的，且能正确
    //   在同一批里区分「可放行」与「需继续等待」。
    void drainReady_multipleVersions() {
        QuarantineStore qs;
        QString err;
        qs.init(db_, &err);
        qs.quarantine(db_, "A", 1, 1, 2, QByteArray("v2"), &err);
        qs.quarantine(db_, "B", 2, 1, 4, QByteArray("v4"), &err);
        qs.quarantine(db_, "C", 3, 1, 6, QByteArray("v6"), &err);
        // currentSchemaVer=4 → v2 and v4 are ready, v6 not（v6 仍是未来版本）
        auto items = qs.drainReady(db_, 4);
        QCOMPARE(items.size(), 2);
    }

    // markReplayed_removesRow —— 重放成功后必须从隔离表删除，且不再被取到。
    // GIVEN 隔离一条 v3、drainReady(3) 能取到它（拿到其行 id）；
    // WHEN 对该 id 调 markReplayed；THEN 再次 drainReady(3) 返回空。
    // 业务含义：每条隔离 payload 只应被重放一次。markReplayed 即「消费完毕、退出隔离区」，
    //   防止同一变更被反复套用（幂等保证）。注意 items.first().first 取的是 pair 的 id 字段
    //   （drainReady 返回的是 <id, payload...> 形式，first 即数据库行主键）。
    void markReplayed_removesRow() {
        QuarantineStore qs;
        QString err;
        qs.init(db_, &err);
        qs.quarantine(db_, "A", 1, 1, 3, QByteArray("payload"), &err);
        auto items = qs.drainReady(db_, 3);
        QCOMPARE(items.size(), 1);
        qint64 id = items.first().first;  // 取该条目的行 id
        qs.markReplayed(db_, id);         // 标记已重放 → 删除该行
        auto items2 = qs.drainReady(db_, 3);
        QCOMPARE(items2.size(), 0);  // 已不在隔离区，不会被再次取出
    }
};

// QTEST_APPLESS_MAIN：生成 main() 并依次运行全部用例。
// 用 APPLESS（无 QApplication）版本：本测试只碰数据库与纯逻辑，不需要 GUI/事件循环，
// 省去 QApplication 启动开销。
QTEST_APPLESS_MAIN(TstSyncQuarantineStore)
#include "tst_sync_quarantine_store.moc"
