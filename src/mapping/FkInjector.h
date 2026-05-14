#pragma once
#include <QVector>

#include "RowPayload.h"

namespace dbridge::detail {

class FkInjector {
   public:
    // Inject FK business keys into child payloads from their parent payloads.
    // Modifies payloads in-place. Returns false if a required parent payload is missing.
    bool inject(QVector<RoutePayload>& payloads, QString* err);
};

}  // namespace dbridge::detail
