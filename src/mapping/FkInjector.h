#pragma once
#include <QSet>
#include <QVector>

#include "RowPayload.h"
#include "profile/ProfileSpec.h"

namespace dbridge {
struct RowError;
}

namespace dbridge::detail {
class ErrorCollector;
}

namespace dbridge::detail {

class FkInjector {
   public:
    // Inject FK business keys into child payloads from their parent payloads.
    // Modifies payloads in-place. Returns false if a required parent payload is missing.
    bool inject(QVector<RoutePayload>& payloads, QString* err);

    QSet<int> inject(QVector<RoutePayload>& payloads, const QVector<RouteSpec>& routes,
                     int excelRow, const QString& sheet, ErrorCollector* errors,
                     QSet<int> initialFailed = {});
};

}  // namespace dbridge::detail
