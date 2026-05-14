#pragma once
#include "dbridge/Types.h"

#include <QSqlDatabase>

#include "profile/ProfileSpec.h"
#include "schema/SchemaCatalog.h"

namespace dbridge::detail {

class ImportService {
   public:
    ImportResult run(const ProfileSpec& profile, const SchemaCatalog& catalog,
                     const QString& xlsxPath, const ImportOptions& options, QSqlDatabase& db);
};

}  // namespace dbridge::detail
