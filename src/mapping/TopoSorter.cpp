// ============================================================================
// TopoSorter.cpp — 路由拓扑排序的实现（Kahn 算法）
// ============================================================================
//
// 本文件实现 TopoSorter::sort()，用 Kahn 算法对路由按 parent 依赖定序。
//
// 【Kahn 算法回顾（为什么这样写）】
//   把表当节点、parent→child 当有向边。
//   1) 统计每个节点的「入度」（指向它的边数，即「它有几个父」——本模型里至多一个父，但
//      统一按入度处理更通用）。
//   2) 把所有入度为 0 的节点（无父的根）入队。
//   3) 不断出队一个节点、追加到拓扑序，并把它所有子节点的入度减 1；某子节点入度减到 0
//      （父都已就位）就入队。
//   4) 若最终拓扑序长度 == 节点数 → 无环、定序成功；否则有节点始终入度 > 0 → 存在环。
// ============================================================================

#include "TopoSorter.h"

#include "dbridge/Errors.h"

#include <QHash>
#include <QQueue>

namespace dbridge::detail {

bool TopoSorter::sort(const QVector<RouteSpec>& routes, QVector<RouteSpec>* sorted, QString* err) {
    if (routes.isEmpty()) {
        sorted->clear();  // 空输入：直接成功，输出空序列
        return true;
    }

    // Build adjacency list and in-degree map
    // 译：构建邻接表与入度表。
    QHash<QString, int> inDeg;             // 表名 → 入度（有几个父指向它）
    QHash<QString, QVector<QString>> adj;  // parent -> children（父表 → 其所有子表）

    // 第一遍：为每个出现的表名建立 inDeg / adj 条目，初值 0 / 空（确保所有节点都登记）。
    for (const auto& r : routes) {
        if (!inDeg.contains(r.table))
            inDeg[r.table] = 0;
        if (!adj.contains(r.table))
            adj[r.table] = {};
    }

    // 第二遍：根据每条路由的 parent 连边、累加子节点入度。
    for (const auto& r : routes) {
        if (!r.parent.isEmpty()) {
            // 防御：父引用了一个根本不在路由集合里的表 → 配置错误，立即报错。
            if (!inDeg.contains(r.parent)) {
                if (err)
                    *err = QStringLiteral("route '") + r.table +
                           QStringLiteral("' references unknown parent '") + r.parent + '\'';
                return false;
            }
            adj[r.parent].append(r.table);  // 加边 parent → table
            inDeg[r.table]++;               // table 的入度 +1
        }
    }

    // Kahn's algorithm
    // 译：Kahn 算法主体。
    // 初始队列：所有入度为 0 的节点（无父的根路由）。
    QQueue<QString> queue;
    for (auto it = inDeg.begin(); it != inDeg.end(); ++it) {
        if (it.value() == 0)
            queue.enqueue(it.key());
    }

    QStringList topoOrder;  // 产出的拓扑序（表名序列）
    while (!queue.isEmpty()) {
        QString t = queue.dequeue();
        topoOrder.append(t);  // 取出一个「父都已就位」的节点，确定其次序
        // 它的每个子节点少了一个未满足的依赖；入度减到 0 即可入队。
        for (const auto& child : adj[t]) {
            if (--inDeg[child] == 0)
                queue.enqueue(child);
        }
    }

    // 环检测：若有节点始终入度 > 0，它永远进不了队，topoOrder 就装不下全部节点。
    if (topoOrder.size() != routes.size()) {
        if (err)
            *err = QString::fromLatin1(err::E_PROFILE_TOPOLOGY_CYCLE) +
                   QStringLiteral(": cycle detected in route parent dependencies");
        return false;
    }

    // Reconstruct in topo order
    // 译：按拓扑序重建 RouteSpec 列表。
    // 先建「表名 → RouteSpec」映射，再按 topoOrder 顺序取出，填入 *sorted。
    sorted->clear();
    QHash<QString, RouteSpec> byTable;
    for (const auto& r : routes)
        byTable[r.table] = r;
    for (const auto& t : topoOrder) {
        sorted->append(byTable[t]);
    }
    return true;
}

}  // namespace dbridge::detail
