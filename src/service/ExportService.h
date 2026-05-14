#pragma once
#include "dbridge/Types.h"

#include <QSqlDatabase>

#include "profile/ProfileSpec.h"
#include "schema/SchemaCatalog.h"

namespace dbridge::detail {

class ExportService {
   public:
    ExportResult run(const ProfileSpec& profile, const SchemaCatalog& catalog,
                     const QString& xlsxPath, const ExportOptions& options, QSqlDatabase& db);
};

}  // namespace dbridge::detail
