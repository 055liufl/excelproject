#include "dbridge/Errors.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/schema/SchemaGuard.h"

// ============================================================================
// tst_sync_schema_guard.cpp — SchemaGuard（同步用「表结构守卫」）的单元测试
// ============================================================================
//
// 【被测对象是什么】
//   SchemaGuard 是「应用对端变更之前」的一致性闸门：两端必须对参与同步的表结构有相同认知，
//   否则按对端结构写本地库会列错位/破坏约束。它用两个维度把关（见 SchemaGuard.h）：
//     · schemaVersion —— 单调递增的结构「代际」整数，廉价、可快速比代；
//     · fingerprint   —— 对真实结构内容(列/类型/NOT NULL/默认值/唯一索引/外键)做 SHA-256，
//                        能抓住「版本号忘升但结构其实变了」的人为疏漏。
//   setLocal() 登记本地基线，verifyPayload() 拿来件 (ver, fp) 与基线比对，不符即拒。
//
// 【这组用例分两块】
//   (A) verifyPayload_* —— 验证闸门逻辑：版本/指纹匹配则放行；任一不符则拒；并覆盖
//       「构造时关闭指纹比对」(M-02 fix) 的降级行为（只看版本号）。
//   (B) computeFingerprint_* —— 验证指纹本身的性质：非空、对同输入稳定、对不同表相异、
//       且对「表名列表顺序」不敏感（内部先排序，消除调用方传参顺序的影响）。
//
// 【夹具】init() 建内存库并造两张结构明显不同的表 items / orders 供指纹用例对比；
//   注意本测试不依赖 __sync_* DDL（SchemaGuard 只读用户表的 PRAGMA），故只手建这两张表。
// ============================================================================

using namespace dbridge::sync;

class TstSyncSchemaGuard : public QObject {
    Q_OBJECT
    QString conn_;
    QSqlDatabase db_;

   private slots:
    // 每用例前：开唯一命名内存库，造两张结构相异的表（items 有 NOT NULL TEXT，orders 有 REAL），
    //   它们的结构差异正是 computeFingerprint_differentTables_different 要检出的。
    void init() {
        conn_ =
            QStringLiteral("tst_sg_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        q.exec(QStringLiteral("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT NOT NULL)"));
        q.exec(QStringLiteral("CREATE TABLE orders (id INTEGER PRIMARY KEY, total REAL)"));
    }
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase（先释放句柄）
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // ── verifyPayload_matchingVersionAndFp_passes —— 版本与指纹都一致 → 放行 ────
    // GIVEN 本地基线 (ver=3, fp="fp_abc")；WHEN 来件携带完全相同的 (3, "fp_abc")；
    // THEN 通过(true) 且 err 为空。这是「正常同步」的快乐路径。
    void verifyPayload_matchingVersionAndFp_passes() {
        SchemaGuard sg;
        sg.setLocal(3, QStringLiteral("fp_abc"));
        QString err;
        QVERIFY(sg.verifyPayload(3, QStringLiteral("fp_abc"), &err));
        QVERIFY(err.isEmpty());
    }

    // ── verifyPayload_versionMismatch_fails —— 版本号不符即拒（第一层闸门）──────
    // GIVEN 基线 ver=3；WHEN 来件 ver=4（即便指纹相同）；THEN 拒绝(false) 且 err 含 "mismatch"。
    //   版本是最先、最廉价的判据，代际不同直接拦下，无需再比指纹。
    void verifyPayload_versionMismatch_fails() {
        SchemaGuard sg;
        sg.setLocal(3, QStringLiteral("fp_abc"));
        QString err;
        QVERIFY(!sg.verifyPayload(4, QStringLiteral("fp_abc"), &err));
        QVERIFY(!err.isEmpty() && err.contains("mismatch"));
    }

    // ── verifyPayload_fingerprintMismatch_fails —— 版本同但指纹不符即拒（第二层）─
    // GIVEN 启用指纹比对、基线 (3,"fp_abc")；WHEN 来件 (3,"fp_XYZ")（版本对、指纹错）；
    // THEN 拒绝。这正是指纹层的价值：版本号没变但结构内容已变，仍能被抓出。
    void verifyPayload_fingerprintMismatch_fails() {
        SchemaGuard sg(/*verifyFingerprint=*/true);
        sg.setLocal(3, QStringLiteral("fp_abc"));
        QString err;
        QVERIFY(!sg.verifyPayload(3, QStringLiteral("fp_XYZ"), &err));
        QVERIFY(!err.isEmpty() && err.contains("mismatch"));
    }

    // ── verifyPayload_fingerprintDisabled_onlyChecksVersion —— 关闭指纹层的降级行为（M-02）─
    // GIVEN 构造时 verifyFingerprint=false；THEN：
    //   · 指纹不同也放行（第一个 QVERIFY，true）——指纹层被跳过；
    //   · 但版本不同仍然拒绝（第二个 !QVERIFY，false）——版本层永远生效。
    // 验证「可单独关指纹、但版本号永远把关」的设计契约（典型用于调试/容忍指纹漂移场景）。
    void verifyPayload_fingerprintDisabled_onlyChecksVersion() {
        SchemaGuard sg(/*verifyFingerprint=*/false);
        sg.setLocal(3, QStringLiteral("fp_abc"));
        QString err;
        // fingerprint mismatch ignored when verifyFingerprint=false（关指纹时忽略指纹差异）
        QVERIFY(sg.verifyPayload(3, QStringLiteral("fp_DIFFERENT"), &err));
        // but version mismatch still fails（但版本不符仍然失败）
        QVERIFY(!sg.verifyPayload(5, QStringLiteral("fp_DIFFERENT"), &err));
    }

    // ── computeFingerprint_nonEmpty —— 指纹结果非空 ───────────────────────────
    // 对一张真实存在的表计算指纹，应得到非空字符串（SHA-256 hex）。最基本的可用性断言。
    void computeFingerprint_nonEmpty() {
        QString fp = SchemaGuard::computeFingerprint(db_, {QStringLiteral("items")});
        QVERIFY(!fp.isEmpty());
    }

    // ── computeFingerprint_sameInput_same —— 同输入 → 同指纹（确定性）──────────
    // 同一张表算两次必须得到完全相同的指纹。确定性是指纹能当一致性判据的前提：
    //   只有「相同结构 → 相同字节流 → 相同哈希」，两端比对指纹才有意义。
    void computeFingerprint_sameInput_same() {
        QString fp1 = SchemaGuard::computeFingerprint(db_, {QStringLiteral("items")});
        QString fp2 = SchemaGuard::computeFingerprint(db_, {QStringLiteral("items")});
        QCOMPARE(fp1, fp2);
    }

    // ── computeFingerprint_differentTables_different —— 不同结构 → 不同指纹 ─────
    // items（含 NOT NULL TEXT 列）与 orders（含 REAL 列）结构不同，指纹必须相异。
    //   验证指纹确实「随结构内容变化」，而非对所有表都吐同一个值（否则形同虚设）。
    void computeFingerprint_differentTables_different() {
        QString fp1 = SchemaGuard::computeFingerprint(db_, {QStringLiteral("items")});
        QString fp2 = SchemaGuard::computeFingerprint(db_, {QStringLiteral("orders")});
        QVERIFY(fp1 != fp2);
    }

    // ── computeFingerprint_tableOrderInsensitive —— 指纹对表名列表顺序不敏感 ────
    // {items, orders} 与 {orders, items} 应算出相同指纹。computeFingerprint 内部先对表名
    //   排序(sorted.sort())，消除「调用方传参顺序」带来的差异——否则两端表清单顺序不同就会
    //   误判结构不一致。这条用例守的就是那处排序的必要性。
    void computeFingerprint_tableOrderInsensitive() {
        // fingerprint should be the same regardless of table list order（与表顺序无关）
        QString fp1 = SchemaGuard::computeFingerprint(
            db_, {QStringLiteral("items"), QStringLiteral("orders")});
        QString fp2 = SchemaGuard::computeFingerprint(
            db_, {QStringLiteral("orders"), QStringLiteral("items")});
        QCOMPARE(fp1, fp2);
    }

    // ── fingerprint_getter_returnsLocalFp —— getter 如实回读 setLocal 设入的指纹 ─
    // setLocal(1,"my_fp") 后 fingerprint() 应返回 "my_fp"。验证 getter 与 setLocal 的写读一致。
    void fingerprint_getter_returnsLocalFp() {
        SchemaGuard sg;
        sg.setLocal(1, QStringLiteral("my_fp"));
        QCOMPARE(sg.fingerprint(), QStringLiteral("my_fp"));
    }
};

QTEST_APPLESS_MAIN(TstSyncSchemaGuard)
#include "tst_sync_schema_guard.moc"
