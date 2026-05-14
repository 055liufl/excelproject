#pragma once
#include <QHash>
#include <QString>
#include <QVector>

#include "RowPayload.h"

namespace dbridge::detail {

class ErrorCollector;

struct SeenEntry {
    int excelRow = 0;
    QVector<QVariant> binds;
};

class BatchUniqueness {
   public:
    void reset() {
        seen_.clear();
    }

    // Check if the given payload's conflict key was already seen.
    // If so, check if it's an allowed parent-row duplicate (same binds, has children).
    // Returns false if it's a real duplicate error.
    bool check(const RoutePayload& payload, int excelRow, bool hasChildren, ErrorCollector* errors,
               const QString& sheet);

   private:
    // routeKey -> conflictKeyEncoded -> SeenEntry
    QHash<QString, QHash<QString, SeenEntry>> seen_;

    static QString encodeConflictKey(const QVector<QVariant>& vals);
};

}  // namespace dbridge::detail
