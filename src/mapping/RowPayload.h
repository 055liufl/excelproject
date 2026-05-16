#pragma once
#include <QHash>
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
    QStringList conflictKey;         // actual conflict column names
    QVector<QVariant> conflictVals;  // values for conflict columns (batch dedup / FK inject)

    // Return index of column in dbColumns, or -1
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
    QString classId;                 // empty for non-mixed
    QVector<RoutePayload> payloads;  // already in topo order
};

}  // namespace dbridge::detail
