#include <QtTest>

#include "mapping/TopoSorter.h"
#include "profile/ProfileSpec.h"

using namespace dbridge::detail;

static RouteSpec makeRoute(const QString& table, const QString& parent = {}) {
    RouteSpec r;
    r.table = table;
    r.parent = parent;
    return r;
}

class TstTopoSorter : public QObject {
    Q_OBJECT

   private slots:
    void testSingleRoot() {
        TopoSorter ts;
        QVector<RouteSpec> sorted;
        QString err;
        QVERIFY(ts.sort({makeRoute("t1")}, &sorted, &err));
        QCOMPARE(sorted.size(), 1);
        QCOMPARE(sorted[0].table, QStringLiteral("t1"));
    }

    void testLinearChain() {
        TopoSorter ts;
        // t1 -> t2 -> t3
        QVector<RouteSpec> routes = {makeRoute("t3", "t2"), makeRoute("t2", "t1"), makeRoute("t1")};
        QVector<RouteSpec> sorted;
        QString err;
        QVERIFY(ts.sort(routes, &sorted, &err));
        QCOMPARE(sorted.size(), 3);
        QCOMPARE(sorted[0].table, QStringLiteral("t1"));
        QCOMPARE(sorted[1].table, QStringLiteral("t2"));
        QCOMPARE(sorted[2].table, QStringLiteral("t3"));
    }

    void testCycleDetection() {
        TopoSorter ts;
        // t1 -> t2 -> t1 (cycle)
        QVector<RouteSpec> routes = {makeRoute("t1", "t2"), makeRoute("t2", "t1")};
        QVector<RouteSpec> sorted;
        QString err;
        QVERIFY(!ts.sort(routes, &sorted, &err));
        QVERIFY(!err.isEmpty());
    }

    void testIsolatedNode() {
        TopoSorter ts;
        QVector<RouteSpec> routes = {makeRoute("t1"), makeRoute("t2")};
        QVector<RouteSpec> sorted;
        QString err;
        QVERIFY(ts.sort(routes, &sorted, &err));
        QCOMPARE(sorted.size(), 2);
    }

    void testUnknownParent() {
        TopoSorter ts;
        QVector<RouteSpec> routes = {makeRoute("t1", "nonexistent")};
        QVector<RouteSpec> sorted;
        QString err;
        QVERIFY(!ts.sort(routes, &sorted, &err));
    }
};

QTEST_MAIN(TstTopoSorter)
#include "tst_topo_sorter.moc"
