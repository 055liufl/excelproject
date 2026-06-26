#pragma once
#include "dbridge/Types.h"

#include <QSqlDatabase>

#include "profile/ProfileSpec.h"
#include "schema/SchemaCatalog.h"
#include <functional>

namespace dbridge::detail {

class ImportService {
   public:
    // §4.11 Optional prefetch query counter hook — called once per actual SELECT batch.
    // Inject a counting lambda in tests; leave nullptr for production (noop).
    std::function<void(const QString& identityKey)> onPrefetch;

    ImportResult run(const ProfileSpec& profile, const SchemaCatalog& catalog,
                     const QString& xlsxPath, const ImportOptions& options, QSqlDatabase& db,
                     bool manageTransaction = true);
};

}  // namespace dbridge::detail
