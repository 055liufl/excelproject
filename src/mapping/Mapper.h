#pragma once
#include <QHash>

#include "RowPayload.h"
#include "profile/ProfileSpec.h"
#include "validation/ValidatorChain.h"

namespace dbridge::detail {

class ExcelReader;
class ErrorCollector;

// Compiled validators per (routeKey, dbColumn)
using ValidatorMap = QHash<QString, QHash<QString, ValidatorChain>>;

class Mapper {
   public:
    // Compile validator chains for all routes.
    // classId is empty for non-mixed.
    bool compileValidators(const QVector<RouteSpec>& routes, const QString& classId,
                           ValidatorMap* vm, QString* err);

    // Map one Excel row to a list of RoutePayloads (one per route).
    // Returns payloads with validation results; any validation errors go to *errors.
    QVector<RoutePayload> map(const QVector<RouteSpec>& routes, int excelRow,
                              const QString& classId, const ExcelReader& reader,
                              const ValidatorMap& vm, ErrorCollector* errors) const;

   private:
    static QString routeKey(const RouteSpec& route, const QString& classId);
};

}  // namespace dbridge::detail
