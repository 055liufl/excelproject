#pragma once
#include <QVector>

#include "profile/ProfileSpec.h"

namespace dbridge::detail {

class TopoSorter {
   public:
    // Sort routes by parent dependency (Kahn's algorithm).
    // Returns false and sets *err = E_PROFILE_TOPOLOGY_CYCLE if a cycle is detected.
    bool sort(const QVector<RouteSpec>& routes, QVector<RouteSpec>* sorted, QString* err);
};

}  // namespace dbridge::detail
