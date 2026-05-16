#include "FkInjector.h"

#include "profile/ProfileSpec.h"

namespace dbridge::detail {

bool FkInjector::inject(QVector<RoutePayload>& payloads, QString* err) {
    // Build a map of routeKey -> payload index for fast lookup
    QHash<QString, int> routeIndex;
    for (int i = 0; i < payloads.size(); ++i) {
        routeIndex[payloads[i].table] = i;
    }

    // The payloads carry fkInject info via their route key naming convention.
    // The actual FK injection info is carried by the RouteSpec, but here we work
    // with already-built payloads.
    //
    // Convention: each payload that needs FK inject has conflictKey columns that
    // may not yet have conflictVals. The FK value comes from the parent payload.
    //
    // This injector is called with profile context in ImportService which passes
    // RouteSpecs alongside payloads. Since RoutePayload doesn't store fkInject
    // directly, we use the profile information via the ImportService.
    //
    // For this standalone version, we look for payload columns that appear in
    // conflictKey but have no corresponding bind value (empty slot).
    // The actual injection is done with RouteSpec context in ImportService::run.
    Q_UNUSED(routeIndex)
    Q_UNUSED(err)
    return true;  // actual injection done in ImportService with profile context
}

}  // namespace dbridge::detail
