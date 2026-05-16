#pragma once
#include <QString>
#include <QVector>

#include "mapping/RowPayload.h"
#include "profile/ProfileSpec.h"

namespace dbridge::detail {

struct UpsertSql {
    QString sql;
    QVector<QString> bindOrder;  // column names in bind order
};

class SqlBuilder {
   public:
    UpsertSql buildUpsert(const RoutePayload& payload);

    QString buildAutoJoinSelect(const QVector<RouteSpec>& routes, const ExportSpec& exportSpec);
};

}  // namespace dbridge::detail
