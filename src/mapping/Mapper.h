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
    // date:* tokens are stripped for columns whose effective temporal slot is declared (those
    // columns are handled by the temporal conversion layer in map()).
    bool compileValidators(const QVector<RouteSpec>& routes, const QString& classId,
                           const ProfileSpec& profile, ValidatorMap* vm, QString* err);

    // Map one Excel row to a list of RoutePayloads (one per route).
    // Returns payloads with validation results; any validation/temporal errors go to *errors.
    // M-06 fix: sheetName is propagated into error entries so callers can pinpoint which sheet
    // the error came from. Default is empty for backwards-compatibility.
    QVector<RoutePayload> map(const QVector<RouteSpec>& routes, int excelRow,
                              const QString& classId, const ExcelReader& reader,
                              const ValidatorMap& vm, const ProfileSpec& profile,
                              ErrorCollector* errors, const QString& sheetName = QString()) const;

   private:
    static QString routeKey(const RouteSpec& route, const QString& classId);
};

}  // namespace dbridge::detail
