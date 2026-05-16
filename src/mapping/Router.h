#pragma once
#include <QHash>
#include <QVariant>

#include "profile/ProfileSpec.h"

namespace dbridge::detail {

class Router {
   public:
    // Initialize from a Mixed ProfileSpec.
    // Validates that matchEquals values are unique.
    bool init(const ProfileSpec& profile, QString* err);

    // Match a discriminator value to a ClassSpec.
    // Returns nullptr if no match found.
    const ClassSpec* match(const QVariant& discriminator) const;

    QString discriminatorSource() const {
        return discriminatorSource_;
    }

   private:
    QString discriminatorSource_;
    QHash<QString, int> matchToClassIdx_;  // matchEquals -> index in classes_
    QVector<ClassSpec> classes_;
};

}  // namespace dbridge::detail
