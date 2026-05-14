#include "TopoSorter.h"

#include "dbridge/Errors.h"

#include <QHash>
#include <QQueue>

namespace dbridge::detail {

bool TopoSorter::sort(const QVector<RouteSpec>& routes, QVector<RouteSpec>* sorted, QString* err) {
    if (routes.isEmpty()) {
        sorted->clear();
        return true;
    }

    // Build adjacency list and in-degree map
    QHash<QString, int> inDeg;
    QHash<QString, QVector<QString>> adj;  // parent -> children

    for (const auto& r : routes) {
        if (!inDeg.contains(r.table))
            inDeg[r.table] = 0;
        if (!adj.contains(r.table))
            adj[r.table] = {};
    }

    for (const auto& r : routes) {
        if (!r.parent.isEmpty()) {
            if (!inDeg.contains(r.parent)) {
                if (err)
                    *err = QStringLiteral("route '") + r.table +
                           QStringLiteral("' references unknown parent '") + r.parent + '\'';
                return false;
            }
            adj[r.parent].append(r.table);
            inDeg[r.table]++;
        }
    }

    // Kahn's algorithm
    QQueue<QString> queue;
    for (auto it = inDeg.begin(); it != inDeg.end(); ++it) {
        if (it.value() == 0)
            queue.enqueue(it.key());
    }

    QStringList topoOrder;
    while (!queue.isEmpty()) {
        QString t = queue.dequeue();
        topoOrder.append(t);
        for (const auto& child : adj[t]) {
            if (--inDeg[child] == 0)
                queue.enqueue(child);
        }
    }

    if (topoOrder.size() != routes.size()) {
        if (err)
            *err = QString::fromLatin1(err::E_PROFILE_TOPOLOGY_CYCLE) +
                   QStringLiteral(": cycle detected in route parent dependencies");
        return false;
    }

    // Reconstruct in topo order
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
