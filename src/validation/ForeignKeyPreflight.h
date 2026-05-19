#pragma once
#include <QSqlDatabase>
#include <QVector>

#include "mapping/RowPayload.h"
#include "profile/ProfileSpec.h"
#include <functional>

namespace dbridge::detail {

class ErrorCollector;

class ForeignKeyPreflight {
   public:
    // §7.7 Optional probe counter hook — called each time a DB probe is actually executed.
    // Inject a counting lambda in tests; leave nullptr for production (noop).
    std::function<void(const QString& table)> onProbe;

    // Check that referenced parent rows exist in DB for payloads that have fkInject.
    // Parent rows that are present in the same batch are considered OK.
    bool check(const QVector<RowContext>& contexts, const QVector<RouteSpec>& allRoutes,
               QSqlDatabase& db, const QString& sheet, ErrorCollector* errors);

   private:
    bool checkPayload(const RoutePayload& payload, const RouteSpec& routeSpec,
                      const QHash<QString, QVector<RoutePayload>>& batchParentPayloads,
                      const QHash<QString, const RouteSpec*>& routeByTable, QSqlDatabase& db,
                      const QString& sheet, int excelRow, ErrorCollector* errors);
};

}  // namespace dbridge::detail
