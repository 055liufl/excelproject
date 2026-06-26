#pragma once
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

namespace dbridge::detail {

struct RoutePayload {
    QString table;
    QString routeKey;  // e.g. "orders" or "A:orders" for mixed
    QVector<QString> dbColumns;
    QVector<QVariant> binds;
    QStringList conflictKey;
    QVector<QVariant> conflictVals;
    // H-01 fix: set by Mapper when a validator or temporal-conversion error occurs for this route.
    // Allows ImportService to add only the affected routeIndex to failedRouteIndices rather than
    // marking the entire row hasNonRouteError.
    bool hasError = false;

    int indexOf(const QString& col) const {
        for (int i = 0; i < dbColumns.size(); ++i) {
            if (dbColumns[i] == col)
                return i;
        }
        return -1;
    }
};

struct RowContext {
    int excelRow = 0;
    QString classId;
    QVector<RoutePayload> payloads;
    // H-04 fix: indices into payloads[] that failed FK injection; their descendants are also
    // skipped in the write phase, while unaffected siblings still proceed.
    QSet<int> failedRouteIndices;
    // M-04 fix: true when the row has a non-route-local error (structural/type/non-binding error).
    // When this flag is set, the entire row is skipped in the write phase even if
    // failedRouteIndices is non-empty, because the payload data itself is unusable.
    bool hasNonRouteError = false;
};

}  // namespace dbridge::detail
