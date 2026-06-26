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

    // H-05 fix: escape an identifier for use in SQL (double-quote + escape internal quotes).
    // All table names and column names from profiles/catalogs must pass through this.
    static QString quoteIdent(const QString& name);
};

}  // namespace dbridge::detail
