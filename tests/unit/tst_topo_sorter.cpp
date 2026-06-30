// ============================================================================
// tst_topo_sorter.cpp — TopoSorter（拓扑排序器）的单元测试
// ============================================================================
//
// 【被测对象在系统中的角色】
//   多表关联导入时，子表的外键引用父表的主键，因此「父表必须先于子表写库」。
//   TopoSorter 接收一组 RouteSpec（每个路由有 table 自身表名、parent 父表名），
//   依据 parent 形成的「子→父」依赖图，对这些路由做拓扑排序，输出一个满足
//   「父在前、子在后」的写入顺序。它还要在依赖成环（A→B→A）或引用了不存在的
//   父表时报错——这两类是配置错误，必须在写库前被挡下，否则会触发数据库外键
//   约束失败或无限循环。
//
// 【本测试验证的契约（不变量）】
//   1) 单根 / 线性链 / 孤立节点 等合法依赖 → 排序成功，且顺序满足「父先子后」；
//   2) 成环依赖 → 排序失败且给出非空错误信息（拒绝产出顺序）；
//   3) 引用不存在的父表 → 排序失败（悬空外键应在配置层被发现）。
//
// 【测试框架】Qt Test（QtTest）。每个 private slot 是一个测试用例，QTEST_MAIN
//   生成可执行入口，依次跑完所有 slot 并汇报通过/失败。
// ============================================================================

#include <QtTest>

#include "mapping/TopoSorter.h"
#include "profile/ProfileSpec.h"

using namespace dbridge::detail;

// makeRoute —— 测试夹具：用最少字段快速构造一个 RouteSpec。
// 做什么：只设 table（本表名）与可选 parent（父表名），其余字段留默认。
// 为什么：拓扑排序只关心 table 与 parent 这两列，省略无关字段让用例聚焦依赖关系本身、
//   更易读。parent 默认空串表示「无父表」（即依赖图中的根节点）。
// 参数：table 本路由的目标表名；parent 其父表名（默认空 = 根）。返回：构造好的 RouteSpec。
static RouteSpec makeRoute(const QString& table, const QString& parent = {}) {
    RouteSpec r;
    r.table = table;
    r.parent = parent;
    return r;
}

// TstTopoSorter —— TopoSorter 的测试套件。
// 每个 private slot = 一条用例，覆盖「合法依赖排序正确 / 非法依赖被拒」两类契约。
class TstTopoSorter : public QObject {
    Q_OBJECT

   private slots:
    // testSingleRoot —— 单个无父表的路由。
    // GIVEN 只有一个根节点 t1（无 parent）；
    // WHEN 排序；THEN 成功，结果就是它自己一个元素。
    // 意义：最小合法输入也要能正常通过，验证空依赖图不会被误判为出错。
    void testSingleRoot() {
        TopoSorter ts;
        QVector<RouteSpec> sorted;
        QString err;
        QVERIFY(ts.sort({makeRoute("t1")}, &sorted, &err));
        QCOMPARE(sorted.size(), 1);
        QCOMPARE(sorted[0].table, QStringLiteral("t1"));
    }

    // testLinearChain —— 三级线性依赖链：t1 ← t2 ← t3（t3 的父是 t2，t2 的父是 t1）。
    // GIVEN 输入【故意打乱】成 t3、t2、t1 的顺序（验证排序器不依赖输入顺序）；
    // WHEN 排序；THEN 输出必须严格还原成 t1、t2、t3——即每个父表都排在其子表之前。
    // 意义：这是「父先于子写库」核心契约最直接的验证；顺序错了外键约束就会失败。
    void testLinearChain() {
        TopoSorter ts;
        // t1 -> t2 -> t3（依赖方向：t2 依赖 t1，t3 依赖 t2）
        QVector<RouteSpec> routes = {makeRoute("t3", "t2"), makeRoute("t2", "t1"), makeRoute("t1")};
        QVector<RouteSpec> sorted;
        QString err;
        QVERIFY(ts.sort(routes, &sorted, &err));
        QCOMPARE(sorted.size(), 3);
        QCOMPARE(sorted[0].table, QStringLiteral("t1"));  // 根在最前
        QCOMPARE(sorted[1].table, QStringLiteral("t2"));  // 中间
        QCOMPARE(sorted[2].table, QStringLiteral("t3"));  // 叶子在最后
    }

    // testCycleDetection —— 成环依赖：t1 的父是 t2、t2 的父是 t1，互为父子（死循环）。
    // GIVEN 一个 2 节点的环；WHEN 排序；THEN 必须失败，且 err 非空（要给出可诊断的原因）。
    // 意义：环意味着「谁都得先于对方写库」，逻辑上无解。排序器必须检测出环并拒绝，
    //   而不能死循环或返回一个错误的顺序。非空 err 是给用户的配置纠错线索。
    void testCycleDetection() {
        TopoSorter ts;
        // t1 -> t2 -> t1 (cycle)（环：t1 依赖 t2，t2 又依赖 t1）
        QVector<RouteSpec> routes = {makeRoute("t1", "t2"), makeRoute("t2", "t1")};
        QVector<RouteSpec> sorted;
        QString err;
        QVERIFY(!ts.sort(routes, &sorted, &err));  // 期望失败
        QVERIFY(!err.isEmpty());                   // 且必须带错误说明
    }

    // testIsolatedNode —— 两个互不相关的孤立根节点 t1、t2（都无 parent）。
    // GIVEN 没有任何依赖边；WHEN 排序；THEN 成功，且两者都在结果中（顺序不作要求）。
    // 意义：依赖图可以是「森林」（多棵独立的树）；排序器不能因为缺少依赖关系而漏掉节点。
    void testIsolatedNode() {
        TopoSorter ts;
        QVector<RouteSpec> routes = {makeRoute("t1"), makeRoute("t2")};
        QVector<RouteSpec> sorted;
        QString err;
        QVERIFY(ts.sort(routes, &sorted, &err));
        QCOMPARE(sorted.size(), 2);  // 两个节点都应保留，不丢失
    }

    // testUnknownParent —— 引用了一个根本不存在的父表 "nonexistent"。
    // GIVEN t1 的父表在路由集合里找不到对应路由；WHEN 排序；THEN 必须失败。
    // 意义：悬空的父表引用是典型的 Profile 配置错误（拼错表名 / 漏配父路由）。
    //   在排序阶段就发现它，比拖到写库时才由数据库报外键错误要友好得多。
    void testUnknownParent() {
        TopoSorter ts;
        QVector<RouteSpec> routes = {makeRoute("t1", "nonexistent")};
        QVector<RouteSpec> sorted;
        QString err;
        QVERIFY(!ts.sort(routes, &sorted, &err));  // 悬空父引用应被拒绝
    }
};

// QTEST_MAIN：生成 main()，构造 TstTopoSorter 并依次执行其全部 private slot 用例。
// （用 QTEST_MAIN 而非 APPLESS 版，是因为它会建一个 QApplication；对纯逻辑测试影响不大。）
QTEST_MAIN(TstTopoSorter)
#include "tst_topo_sorter.moc"
