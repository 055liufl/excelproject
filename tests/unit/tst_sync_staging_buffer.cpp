// ============================================================================
// tst_sync_staging_buffer.cpp — StagingBuffer（合并暂存区）单元测试
// ============================================================================
//
// 【被测对象是什么】
//   StagingBuffer 是「比对会话」的内存暂存层（见场景2 演示）。用户在 GUI 里做出「采用对端
//   某行/某列」的合并决策时，决策【先暂存在内存里】，并不立刻落库；直到点「保存」才一次性写盘。
//   它就是这块「未落库的待写行集合」，核心接口：
//     · stage(table, pk, row)   —— 把「表 table 主键 pk 的整行 row（列名→值的 QVariantMap）」暂存。
//     · unstage(table, pk)      —— 撤销某行的暂存。
//     · getRow(table, pk)       —— 读回某行的暂存内容（未暂存返回空 map）。
//     · isEmpty()               —— 暂存区是否为空。
//     · toMutations(pkMap)      —— 把所有暂存行翻译成 RowMutation 列表（供写盘 / 同步广播）。
//     · save(db, executor, pk..) —— 经 UpsertExecutor 把暂存行 UPSERT 进 db。
//     · discard()               —— 丢弃全部暂存、不写盘。
//
// 【关键设计点（测试要锁死的不变量）】
//   · save() 写盘后【刻意不清空 staged_】（见 save_writesToDb 注释）——清空与否由上层决定。
//   · discard() 必须「零落库」：丢弃后暂存区空，且数据库一行都没多出来。
//   · toMutations 产出的每条变更默认是
//   UpsertMode::DoUpdate（有则更新、无则插，符合「采用对端」语义）。
//
// 【测试策略】每个用例前 init() 起独立 :memory: 库并建一张 orders(id PK, name) 真表；
//   StagingBuffer 是纯内存对象，只有 save()/真实写盘的用例才真正碰 db_。辅助 count() 直接
//   SELECT COUNT(*) 读真表行数，用于验证「是否真的写进了库」。
// ============================================================================
#include "dbridge/sync/SyncTypes.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/apply/UpsertExecutor.h"
#include "sync/diff/StagingBuffer.h"

using namespace dbridge::sync;

class TstSyncStagingBuffer : public QObject {
    Q_OBJECT
    QString conn_;     // 唯一连接名（UUID）
    QSqlDatabase db_;  // :memory: 库连接

    // exec —— 测试辅助：执行一条 SQL，失败则直接以数据库错误文本让用例 fail。
    void exec(const QString& sql) {
        QSqlQuery q(db_);
        QVERIFY2(q.exec(sql), qPrintable(q.lastError().text()));
    }
    // count —— 测试辅助：返回表 t 的实际行数（用于验证写盘效果）；查询失败返回 -1。
    int count(const QString& t) {
        QSqlQuery q(db_);
        q.exec("SELECT COUNT(*) FROM " + t);
        return q.next() ? q.value(0).toInt() : -1;
    }

   private slots:
    // init —— 夹具：建干净内存库 + 建被写入的目标真表 orders(id 主键, name)。
    void init() {
        conn_ =
            QStringLiteral("tst_sb_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        exec("CREATE TABLE orders (id INTEGER PRIMARY KEY NOT NULL, name TEXT)");
    }
    // cleanup —— 夹具：先释放 db_ 句柄，再移除连接（顺序同其它测试，见 ConsistencyCache 注释）。
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase（先释放句柄）
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // isEmpty_initially —— 契约：刚构造的暂存区为空。
    void isEmpty_initially() {
        StagingBuffer buf;
        QVERIFY(buf.isEmpty());
    }

    // stage_notEmpty —— 契约：stage 一行后暂存区非空。
    // 注意此处只动内存、没碰 db_：暂存是「未落库」的，这正是它存在的意义。
    void stage_notEmpty() {
        StagingBuffer buf;
        QVariantMap row;
        row["id"] = 1;
        row["name"] = "Alice";
        buf.stage("orders", "1", row);
        QVERIFY(!buf.isEmpty());
    }

    // unstage_removesEntry —— 契约：stage 后再 unstage 同一 (table,pk)，暂存区回到空。
    // 业务含义：用户「采用对端某行」后又「撤销」，该决策应被干净移除。
    void unstage_removesEntry() {
        StagingBuffer buf;
        QVariantMap row;
        row["id"] = 1;
        row["name"] = "A";
        buf.stage("orders", "1", row);
        buf.unstage("orders", "1");
        QVERIFY(buf.isEmpty());
    }

    // getRow_returnsStaged —— 契约：getRow 能原样读回先前 stage 的整行内容。
    // 这里验证 name 字段读回为 "Bob"，确认暂存的是「值」而非仅「键」。
    void getRow_returnsStaged() {
        StagingBuffer buf;
        QVariantMap row;
        row["id"] = 42;
        row["name"] = "Bob";
        buf.stage("orders", "42", row);
        QVariantMap got = buf.getRow("orders", "42");
        QCOMPARE(got["name"].toString(), QString("Bob"));
    }

    // getRow_missing_returnsEmpty —— 契约：读未暂存的行返回空 map（而非崩溃/脏数据）。
    void getRow_missing_returnsEmpty() {
        StagingBuffer buf;
        QVERIFY(buf.getRow("orders", "99").isEmpty());
    }

    // discard_zeroPersist —— 核心不变量：discard() 必须「零落库」。
    // GIVEN stage 了一行  WHEN discard()
    // THEN 暂存区空，且【数据库里 orders 仍是 0 行】——证明暂存全程只在内存、丢弃不会污染库。
    void discard_zeroPersist() {
        StagingBuffer buf;
        QVariantMap row;
        row["id"] = 7;
        row["name"] = "Carol";
        buf.stage("orders", "7", row);
        buf.discard();
        QVERIFY(buf.isEmpty());
        QCOMPARE(count("orders"), 0);  // nothing written to DB（一行都没落库）
    }

    // save_writesToDb —— 核心正路径：save() 经 UpsertExecutor 把暂存行真正写进库。
    // GIVEN stage 一行 + 指定主键列 {"id"}  WHEN save(db, ue, {"id"})
    // THEN 返回 true 且 orders 实际多了 1 行。
    // 重要设计：save() 写盘后【刻意不清空 staged_】（见末行注释）——是否清空交由上层（如比对
    //          会话保存后重建）决定，StagingBuffer 不替上层做这个语义假设。
    void save_writesToDb() {
        StagingBuffer buf;
        QVariantMap row;
        row["id"] = 10;
        row["name"] = "Dave";
        buf.stage("orders", "10", row);
        UpsertExecutor ue;
        QString err;
        QVERIFY(buf.save(db_, ue, QStringList{"id"}, &err));
        QCOMPARE(count("orders"), 1);
        // Note: save() does NOT clear staged_ (by design)（按设计，save 不清空暂存）
    }

    // save_empty_succeeds —— 边界：空暂存区 save() 也应成功（no-op），且不写任何行。
    // 业务含义：用户没做任何决策就点保存，不应报错、也不该改库。
    void save_empty_succeeds() {
        StagingBuffer buf;
        UpsertExecutor ue;
        QString err;
        QVERIFY(buf.save(db_, ue, QStringList{"id"}, &err));
        QCOMPARE(count("orders"), 0);
    }

    // toMutations_buildsCorrectly —— 契约：暂存行能正确翻译成 RowMutation。
    // pkMap 告诉 toMutations「每张表的主键列是哪些」。断言产出的单条变更：
    //   表名=orders、主键列={id}、模式=DoUpdate（有则更新无则插，契合「采用对端」语义）。
    // 这是 StagingBuffer 与同步写入管线（apply/广播）之间的数据契约。
    void toMutations_buildsCorrectly() {
        StagingBuffer buf;
        QVariantMap row;
        row["id"] = 5;
        row["name"] = "Eve";
        buf.stage("orders", "5", row);
        QHash<QString, QStringList> pkMap;
        pkMap["orders"] = QStringList{"id"};
        auto muts = buf.toMutations(pkMap);
        QCOMPARE(muts.size(), 1);
        QCOMPARE(muts[0].table, QString("orders"));
        QCOMPARE(muts[0].pkColumns, QStringList({"id"}));
        QCOMPARE(muts[0].mode, UpsertMode::DoUpdate);
    }

    // stage_multipleRows_allSaved —— 契约：暂存多行后 save 应全部写入。
    // GIVEN 暂存 id=1..3 三行  WHEN save  THEN orders 恰好 3 行，无遗漏。
    void stage_multipleRows_allSaved() {
        StagingBuffer buf;
        for (int i = 1; i <= 3; ++i) {
            QVariantMap row;
            row["id"] = i;
            row["name"] = QString("N%1").arg(i);
            buf.stage("orders", QString::number(i), row);
        }
        UpsertExecutor ue;
        QString err;
        QVERIFY(buf.save(db_, ue, QStringList{"id"}, &err));
        QCOMPARE(count("orders"), 3);
    }
};

// 无 GUI 的测试入口；末行引入 moc 生成代码（Qt Test 单文件固定写法）。
QTEST_APPLESS_MAIN(TstSyncStagingBuffer)
#include "tst_sync_staging_buffer.moc"
