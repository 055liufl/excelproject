// ============================================================================
// tst_write_txn.cpp — WriteTxn（写事务 RAII 封装）的单元测试
// ============================================================================
//
// 【被测对象是什么】
//   WriteTxn（见 src/sync/WriteTxn.h）把 SQLite 的 BEGIN IMMEDIATE / COMMIT /
//   ROLLBACK 封进一个栈对象。它最核心的契约有两条：
//     1) 显式控制：begin() 开事务、commit() 提交、rollback() 回滚；isActive() 反映状态。
//     2) RAII 兜底：对象若在事务仍“开着”时被析构，析构函数自动 ROLLBACK——这是为了
//        杜绝 early-return / 抛异常导致事务被永久悬挂、进而把写连接锁死的经典 bug。
//
// 【这组测试在守护什么不变量】
//   逐一钉死上面两条契约的每个面：begin 置活、commit 落盘且转非活、rollback 撤销且转非活、
//   析构自动回滚、对未开启事务的 rollback 必须是安全 no-op、isActive 全程如实跟踪状态。
//
// 【测试夹具与隔离手法（贯穿全文件）】
//   · 每个测试用例运行前 init()、运行后 cleanup()（QtTest 约定的 per-test 钩子）。
//   · 每次都新建一个【全新的内存库】(":memory:")，连接名用 UUID 保证唯一——这样用例之间
//     完全隔离、互不污染，且无需清理磁盘文件。
//   · 验证“数据是否真的落盘/被撤销”，统一用 SELECT COUNT(*) 去查那一行在不在，
//     而不是去读 WriteTxn 的内部状态——直接检验“对数据库的可观察效果”。
//
// 【框架】Qt Test：private slots 里每个无参方法即一个测试用例；QVERIFY/QCOMPARE 是断言；
//   QTEST_APPLESS_MAIN 生成不依赖 GUI 事件循环的 main()。
// ============================================================================

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/WriteTxn.h"

using namespace dbridge::sync;

// TstWriteTxn —— WriteTxn 的测试套件。每个 private slot 是一个独立用例。
class TstWriteTxn : public QObject {
    Q_OBJECT
    QString conn_;  // 本用例专属的连接名（UUID 派生，保证用例间不撞名）
    // store the db handle as a member so we can bind non-const lvalue refs
    // 【译】把 db 句柄存成成员，是因为 WriteTxn 的构造形参是「非常量左值引用」(QSqlDatabase&)，
    //   必须有一个具名的左值成员才能绑定（临时量绑不上非常量引用）。
    QSqlDatabase db_;

   private slots:
    // init —— 每个用例运行【前】调用（QtTest per-test 钩子）：建一个全新内存库 + 建表 t。
    // 做什么：生成唯一连接名 → addDatabase 注册 QSQLITE 连接 → 指向 :memory: → open →
    //   建一张最简单的表 t(id PK, v TEXT) 供各用例写入/查询。
    // 为什么用 :memory: + 唯一连接名：保证用例之间彻底隔离、零磁盘副作用、无需手工清表。
    void init() {
        conn_ = QStringLiteral("tst_wtxn_") +
                QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        QVERIFY(q.exec(QStringLiteral("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)")));
    }
    // cleanup —— 每个用例运行【后】调用：关闭并移除连接，释放内存库。
    // 注意顺序：必须先把成员 db_ 置为空 QSqlDatabase（释放本地句柄对连接的引用），
    //   再 removeDatabase——否则 Qt 会警告“连接仍在使用中”而拒绝移除。
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase
                               // 【译】移除连接前先释放句柄引用
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // begin_setsActive —— 验证：begin() 成功后事务进入“活动”态。
    // GIVEN 一个刚构造、尚未开事务的 WriteTxn；WHEN 调用 begin()；
    // THEN begin() 返回 true（取写锁成功），且 isActive() 为 true（事务进行中）。
    void begin_setsActive() {
        WriteTxn txn(db_);
        QString err;
        QVERIFY(txn.begin(&err));
        QVERIFY(txn.isActive());
    }

    // commit_persists —— 验证：commit() 把事务内的写真正落盘，并把事务转为非活动态。
    // GIVEN 已 begin 的事务里插入了一行；WHEN commit()；
    // THEN ① commit 返回 true；② isActive() 转 false（事务已结束）；
    //      ③ 该行随后可被查询到（COUNT==1）——即写入对后续查询可见 = 已持久化。
    void commit_persists() {
        WriteTxn txn(db_);
        QString err;
        txn.begin(&err);
        QSqlQuery q(db_);
        q.exec(QStringLiteral("INSERT INTO t VALUES (1, 'hello')"));
        QVERIFY(txn.commit(&err));
        QVERIFY(!txn.isActive());
        // verify row is visible
        // 【译】验证该行可见（已落盘）：在事务外查询仍能查到，证明 COMMIT 生效。
        QSqlQuery s(db_);
        s.exec(QStringLiteral("SELECT COUNT(*) FROM t WHERE id=1"));
        QVERIFY(s.next());
        QCOMPARE(s.value(0).toInt(), 1);
    }

    // rollback_discards —— 验证：rollback() 撤销事务内的全部写，并转为非活动态。
    // GIVEN 已 begin 的事务里插入了一行；WHEN 显式 rollback()；
    // THEN ① isActive() 转 false；② 该行不存在（COUNT==0）——插入被回滚撤销。
    void rollback_discards() {
        WriteTxn txn(db_);
        QString err;
        txn.begin(&err);
        QSqlQuery q(db_);
        q.exec(QStringLiteral("INSERT INTO t VALUES (2, 'gone')"));
        txn.rollback();
        QVERIFY(!txn.isActive());
        QSqlQuery s(db_);
        s.exec(QStringLiteral("SELECT COUNT(*) FROM t WHERE id=2"));
        QVERIFY(s.next());
        QCOMPARE(s.value(0).toInt(), 0);
    }

    // destructor_rollsBackIfActive —— 验证 RAII 兜底：事务仍活动时对象析构会自动回滚。
    // 这是 WriteTxn 最重要的安全保证（防 early-return/异常导致事务悬挂）。
    // GIVEN 一个内嵌作用域 {}：begin 后插入一行，但【既不 commit 也不 rollback】；
    // WHEN 离开作用域 → txn 析构（此时 active_ 仍为真）；
    // THEN 析构函数自动 ROLLBACK，该行不存在（COUNT==0）。
    void destructor_rollsBackIfActive() {
        {
            WriteTxn txn(db_);
            QString err;
            txn.begin(&err);
            QSqlQuery q(db_);
            q.exec(QStringLiteral("INSERT INTO t VALUES (3, 'raii')"));
        }  // destructor should rollback
           // 【译】离开作用域 → 析构函数应自动回滚（因为事务还开着）
        QSqlQuery s(db_);
        s.exec(QStringLiteral("SELECT COUNT(*) FROM t WHERE id=3"));
        QVERIFY(s.next());
        QCOMPARE(s.value(0).toInt(), 0);
    }

    // rollback_whenInactive_noop —— 验证：对“未开启事务”的对象调 rollback() 是安全 no-op。
    // GIVEN 一个从未 begin 的 WriteTxn（active_==false）；WHEN rollback()；
    // THEN 不崩溃、不抛错，isActive() 仍为 false。这保证 rollback 可被无脑、重复调用。
    void rollback_whenInactive_noop() {
        WriteTxn txn(db_);
        txn.rollback();  // must not crash  【译】不得崩溃
        QVERIFY(!txn.isActive());
    }

    // isActive_tracksState —— 验证：isActive() 在事务生命周期内如实跟踪状态机。
    // 序列断言：构造后=false → begin 后=true → rollback 后=false。
    // 这把“状态查询”与“状态转换”两类操作绑在一起验证，确保读到的状态始终与实际一致。
    void isActive_tracksState() {
        WriteTxn txn(db_);
        QVERIFY(!txn.isActive());  // 刚构造：未开事务
        QString err;
        txn.begin(&err);
        QVERIFY(txn.isActive());  // begin 后：进行中
        txn.rollback();
        QVERIFY(!txn.isActive());  // rollback 后：已结束
    }
};

// QTEST_APPLESS_MAIN：生成 main()，无需 QApplication/事件循环即可跑（纯逻辑/SQL 测试用）。
QTEST_APPLESS_MAIN(TstWriteTxn)
// moc 生成的元对象代码（Q_OBJECT 所需）；文件名须与本 .cpp 同名。
#include "tst_write_txn.moc"
