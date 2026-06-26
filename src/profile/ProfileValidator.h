#pragma once
#include <QStringList>

#include "ProfileSpec.h"
#include "schema/SchemaCatalog.h"

namespace dbridge::detail {

class ErrorCollector;

class ProfileValidator {
   public:
    bool validate(const ProfileSpec& profile, const SchemaCatalog& catalog,
                  const QStringList& excelHeaders, ErrorCollector* errors, bool importMode = true);

    // H-03 fix: export-mode validation — skips discriminator.source and Excel header checks
    // (those are import-only concerns) but still validates columnOrder, rawSql, table/column
    // existence, conflict keys, and reverse-lookup references.
    bool validateForExport(const ProfileSpec& profile, const SchemaCatalog& catalog,
                           ErrorCollector* errors);

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
