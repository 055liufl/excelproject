#pragma once
#include <QStringList>

#include "ProfileSpec.h"
#include "schema/SchemaCatalog.h"

namespace dbridge::detail {

class ErrorCollector;

class ProfileValidator {
   public:
    bool validate(const ProfileSpec& profile, const SchemaCatalog& catalog,
                  const QStringList& excelHeaders, ErrorCollector* errors);

   private:
    bool validateRoutes(const QVector<RouteSpec>& routes, const SchemaCatalog& catalog,
                        const QStringList& excelHeaders, const QString& sheet,
                        ErrorCollector* errors);

    bool validateRoute(const RouteSpec& route, const QVector<RouteSpec>& allRoutes,
                       const SchemaCatalog& catalog, const QStringList& excelHeaders,
                       const QString& sheet, ErrorCollector* errors);

    bool isConflictValid(const ConflictSpec& conflict, const TableInfo& table);
};

}  // namespace dbridge::detail
