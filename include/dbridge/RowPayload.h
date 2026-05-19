#pragma once
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
};

}  // namespace dbridge::detail
