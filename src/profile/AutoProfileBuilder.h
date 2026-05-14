#pragma once
#include "ProfileSpec.h"
#include "schema/SchemaCatalog.h"

namespace dbridge::detail {

class AutoProfileBuilder {
   public:
    // Build an auto-generated single-table ProfileSpec.
    // Returns false if table has no usable conflict key.
    bool build(const TableInfo& table, ProfileSpec* out, QString* err);

    // Serialize a ProfileSpec to JSON string with fixed field order.
    QString toJson(const ProfileSpec& profile) const;
};

}  // namespace dbridge::detail
