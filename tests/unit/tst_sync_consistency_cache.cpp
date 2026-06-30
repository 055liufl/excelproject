// ============================================================================
// tst_sync_consistency_cache.cpp — ConsistencyCache（一致性缓存）单元测试
// ============================================================================
//
// 【被测对象是什么】
//   ConsistencyCache 是「选择性推送」子系统里的一块加速缓存。它记录「某张表的某一行
//   （以主键 pk 定位）当前与权威源（中心节点）相比，内容指纹是不是一致」。指纹用
//   QByteArray fingerprint 表示——通常是该行内容的 checksum。
//
// 【它解决什么问题（在系统中的价值）】
//   选择性推送 / 比对时，若每次都重新比对每一行的全部字段会很贵。一致性缓存的思路是：
//   「上次我已确认这一行与权威源指纹相同了，只要这行没被改动，下次就能跳过昂贵的逐字段比对」。
//   于是它提供三个原语：
//     · stampFromAuthoritative(table, pk, fp) —— 盖章：登记「此行已与权威源对齐到指纹 fp」。
//     · isConsistent(table, pk, fp)           —— 查询：传入「当前行指纹」，命中且指纹相等 → true。
//     · invalidateTable(table)                —— 整表作废：该表所有盖章一笔勾销（表被改动后调用）。
//   关键不变量：isConsistent 只有在「该 (table,pk) 有盖章 且 盖的指纹 == 传入指纹」时才返回 true；
//   未盖章、指纹不符、被作废，都返回 false（即「不可信，须重新比对」）。
//
// 【durable（持久）vs 非持久】
//   init(db, durable, &err) 的 durable 决定缓存落在哪儿：
//     · durable=true  —— 盖章写进 db 里的持久表，跨「新建另一个 ConsistencyCache 实例 + 重新
//                        init 同一个 db」仍能读回（重启进程后仍有效）。
//     · durable=false —— 盖章只活在内存里，换一个实例 re-init 后读不回（进程级临时加速）。
//
// 【测试策略】
//   每个用例前 init() 起一个独立的 :memory: SQLite 连接，并执行 SyncDDL 的全部建表语句
//   （ConsistencyCache 的 durable 表也在其中）；cleanup() 释放连接。每个 TEST 槽各自 new 一个
//   ConsistencyCache，互不干扰。断言围绕上面那条「命中且指纹相等才 true」的核心不变量展开。
//
// 【Qt Test 框架约定】init()/cleanup() 是每个测试槽前后自动调用的夹具；其余 private slots
//   每个都是一个独立测试用例；QTEST_APPLESS_MAIN 生成不依赖 GUI 事件循环的 main。
// ============================================================================
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>

#include "sync/SyncDDL.h"
#include "sync/selection/ConsistencyCache.h"

using namespace dbridge::sync;

class TstSyncConsistencyCache : public QObject {
    Q_OBJECT
    QString conn_;     // 本测试用的 QSqlDatabase 连接名（用 UUID 保证全局唯一）
    QSqlDatabase db_;  // 指向 :memory: 库的连接句柄

   private slots:
    // init —— 每个测试槽运行前的夹具：建一个干净的内存库并铺好全部 __sync_* 表。
    // 为什么用 :memory: + 唯一连接名：每个用例都从零开始、互不污染；UUID 连接名避免与其它
    // 测试/线程的连接重名。allCreateStatements() 一次性建齐同步元数据表（含一致性缓存表）。
    void init() {
        conn_ =
            QStringLiteral("tst_cc_") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
        db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn_);
        db_.setDatabaseName(QStringLiteral(":memory:"));
        QVERIFY(db_.open());
        QSqlQuery q(db_);
        for (const QString& s : dbridge::sync::ddl::allCreateStatements())
            q.exec(s);
    }
    // cleanup —— 每个测试槽运行后的夹具：先把 db_ 句柄置空（释放对连接的引用），再移除连接。
    // 顺序要点：Qt 要求「removeDatabase 前不得有该连接的活动副本」，故必须先让 db_ 失效。
    void cleanup() {
        db_ = QSqlDatabase();  // release handle before removeDatabase（先释放句柄）
        if (QSqlDatabase::contains(conn_)) {
            QSqlDatabase::database(conn_).close();
            QSqlDatabase::removeDatabase(conn_);
        }
    }

    // init_inMemory_ok —— 契约：非持久模式下 init() 应成功。
    // WHEN durable=false 初始化  THEN 返回 true（缓存子系统就绪，不报错）。
    void init_inMemory_ok() {
        ConsistencyCache cc;
        QString err;
        QVERIFY(cc.init(db_, /*durable=*/false, &err));
    }

    // init_durable_ok —— 契约：持久模式下 init() 也应成功（持久表已由 DDL 建好）。
    void init_durable_ok() {
        ConsistencyCache cc;
        QString err;
        QVERIFY(cc.init(db_, /*durable=*/true, &err));
    }

    // isConsistent_unknownEntry_false —— 核心不变量①：从未盖章的行不可信。
    // GIVEN 一个空缓存  WHEN 查询从未 stamp 过的 (orders, pk1)  THEN 返回 false。
    // 业务含义：没有任何「已与权威源对齐」的证据时，绝不能擅自认为一致 → 必须重新比对。
    void isConsistent_unknownEntry_false() {
        ConsistencyCache cc;
        QString err;
        cc.init(db_, false, &err);
        QVERIFY(!cc.isConsistent("orders", "pk1", QByteArray("fp")));
    }

    // stampAndCheck_consistent —— 核心不变量②：盖章后、用同一指纹查，命中为 true。
    // GIVEN stamp(orders, pk1, fp)  WHEN isConsistent(orders, pk1, 同一个 fp)  THEN true。
    // 这是缓存「加速命中」的正路径：行未变（指纹相同）时可跳过昂贵比对。
    void stampAndCheck_consistent() {
        ConsistencyCache cc;
        QString err;
        cc.init(db_, false, &err);
        QByteArray fp("center_fingerprint_abc");
        cc.stampFromAuthoritative(db_, "orders", "pk1", fp);
        QVERIFY(cc.isConsistent("orders", "pk1", fp));
    }

    // isConsistent_differentFp_false —— 核心不变量③：指纹不符即视为不一致。
    // GIVEN 盖的是 fp_center  WHEN 用「不同的当前指纹 fp_local_different」查  THEN false。
    // 业务含义：本地行内容已变（指纹变了），与权威源不再对齐 → 缓存命中也不算数，须重比。
    void isConsistent_differentFp_false() {
        ConsistencyCache cc;
        QString err;
        cc.init(db_, false, &err);
        cc.stampFromAuthoritative(db_, "orders", "pk1", QByteArray("fp_center"));
        QVERIFY(!cc.isConsistent("orders", "pk1", QByteArray("fp_local_different")));
    }

    // invalidateTable_clearsEntries —— 契约：整表作废只影响该表，不波及其它表。
    // GIVEN 给 orders 的 pk1/pk2 与 items 的 pk3 都盖了章
    // WHEN invalidateTable(orders)
    // THEN orders 的两行都变 false（被清空），items 的 pk3 仍为 true（隔离不受影响）。
    // 业务含义：某表被改动后调用 invalidateTable，使该表缓存全部失效、强制重比，而无关表的
    //          加速命中不应被误伤。
    void invalidateTable_clearsEntries() {
        ConsistencyCache cc;
        QString err;
        cc.init(db_, false, &err);
        cc.stampFromAuthoritative(db_, "orders", "pk1", QByteArray("fp"));
        cc.stampFromAuthoritative(db_, "orders", "pk2", QByteArray("fp"));
        cc.stampFromAuthoritative(db_, "items", "pk3", QByteArray("fp"));
        cc.invalidateTable(db_, "orders");
        QVERIFY(!cc.isConsistent("orders", "pk1", QByteArray("fp")));
        QVERIFY(!cc.isConsistent("orders", "pk2", QByteArray("fp")));
        QVERIFY(
            cc.isConsistent("items", "pk3", QByteArray("fp")));  // other table unaffected（隔离）
    }

    // durable_persistsAcrossInit —— 契约：durable=true 的盖章跨实例/重新 init 仍能读回。
    // GIVEN 用实例 cc(durable) 盖章后销毁（离开内层作用域，cc 析构）
    // WHEN 新建另一个实例 cc2，对【同一个 db_】重新 init(durable)
    // THEN cc2 仍能读出 fp_v1 的一致性 → 证明盖章确实落进了持久表、而非仅在 cc 的内存里。
    void durable_persistsAcrossInit() {
        {
            ConsistencyCache cc;
            QString err;
            cc.init(db_, /*durable=*/true, &err);
            cc.stampFromAuthoritative(db_, "t", "pk", QByteArray("fp_v1"));
        }
        // new instance, same DB（新实例、同一个 db）
        ConsistencyCache cc2;
        QString err;
        cc2.init(db_, /*durable=*/true, &err);
        QVERIFY(cc2.isConsistent("t", "pk", QByteArray("fp_v1")));
    }

    // nonDurable_doesNotPersist —— 契约：durable=false 的盖章不跨实例（与上一用例镜像对照）。
    // GIVEN 用 cc(非持久) 盖章后销毁  WHEN 新实例 cc2 重新 init(非持久)  THEN 读不回 → false。
    // 与 durable_persistsAcrossInit 成对，共同界定 durable 标志的精确语义边界。
    void nonDurable_doesNotPersist() {
        {
            ConsistencyCache cc;
            QString err;
            cc.init(db_, /*durable=*/false, &err);
            cc.stampFromAuthoritative(db_, "t", "pk", QByteArray("fp"));
        }
        ConsistencyCache cc2;
        QString err;
        cc2.init(db_, /*durable=*/false, &err);
        QVERIFY(!cc2.isConsistent("t", "pk", QByteArray("fp")));
    }

    // multipleTablesPkIsolated —— 契约：缓存键是 (table, pk) 复合键，不同表的同名 pk 互不串。
    // GIVEN tA.pk1 盖 fp_a、tB.pk1 盖 fp_b（两表用了相同的 pk 字面量 "pk1"）
    // WHEN 分别按各自指纹查
    // THEN tA.pk1 对 fp_a 命中、对 fp_b 不命中；tB.pk1 对 fp_b 命中
    //      → 证明表名参与了键，"pk1" 在 tA 与 tB 下是两条独立记录、不会相互覆盖。
    void multipleTablesPkIsolated() {
        ConsistencyCache cc;
        QString err;
        cc.init(db_, false, &err);
        cc.stampFromAuthoritative(db_, "tA", "pk1", QByteArray("fp_a"));
        cc.stampFromAuthoritative(db_, "tB", "pk1", QByteArray("fp_b"));
        QVERIFY(cc.isConsistent("tA", "pk1", QByteArray("fp_a")));
        QVERIFY(!cc.isConsistent("tA", "pk1", QByteArray("fp_b")));
        QVERIFY(cc.isConsistent("tB", "pk1", QByteArray("fp_b")));
    }
};

// QTEST_APPLESS_MAIN：生成一个不创建 QApplication（无 GUI 事件循环）的 main，
// 依次运行本类所有 private slots 测试用例。末行 #include .moc 引入 moc 为 Q_OBJECT
// 生成的元对象代码（Qt Test 单文件测试的固定写法）。
QTEST_APPLESS_MAIN(TstSyncConsistencyCache)
#include "tst_sync_consistency_cache.moc"
