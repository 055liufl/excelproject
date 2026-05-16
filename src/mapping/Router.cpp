#include "Router.h"

#include "dbridge/Errors.h"

namespace dbridge::detail {

bool Router::init(const ProfileSpec& profile, QString* err) {
    discriminatorSource_ = profile.discriminatorSource;
    classes_ = profile.classes;
    matchToClassIdx_.clear();

    for (int i = 0; i < classes_.size(); ++i) {
        const auto& cls = classes_[i];
        if (matchToClassIdx_.contains(cls.matchEquals)) {
            if (err)
                *err = QStringLiteral("Duplicate matchEquals '") + cls.matchEquals +
                       QStringLiteral("' in mixed profile classes");
            return false;
        }
        matchToClassIdx_[cls.matchEquals] = i;
    }
    return true;
}

const ClassSpec* Router::match(const QVariant& discriminator) const {
    if (discriminator.isNull())
        return nullptr;
    QString val = discriminator.toString();
    auto it = matchToClassIdx_.find(val);
    if (it == matchToClassIdx_.end())
        return nullptr;
    return &classes_[it.value()];
}

}  // namespace dbridge::detail
