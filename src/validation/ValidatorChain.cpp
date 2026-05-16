#include "ValidatorChain.h"

#include "dbridge/Errors.h"

#include "Validators.h"

namespace dbridge::detail {

static bool isTypeNormalizingToken(const QString& token) {
    return token == QStringLiteral("int") || token.startsWith(QStringLiteral("int>=")) ||
           token == QStringLiteral("decimal") || token.startsWith(QStringLiteral("date:"));
}

bool ValidatorChain::compile(const QStringList& tokens, QString* err) {
    fns_.clear();
    // Detect conflicting type-normalizing tokens (spec §5.7)
    int typeTokenCount = 0;
    for (const auto& token : tokens) {
        if (isTypeNormalizingToken(token))
            typeTokenCount++;
    }
    if (typeTokenCount > 1) {
        if (err)
            *err = QString::fromLatin1(err::E_PROFILE_PARSE) +
                   QStringLiteral(": multiple conflicting type tokens in validator chain");
        return false;
    }

    for (const auto& token : tokens) {
        ValidatorFn fn = compileToken(token, err);
        if (!fn)
            return false;
        fns_.push_back(std::move(fn));
    }
    return true;
}

bool ValidatorChain::run(const QVariant& in, QVariant* outVal, QString* errCode,
                         QString* msg) const {
    QVariant current = in;
    for (const auto& fn : fns_) {
        QVariant next;
        if (!fn(current, &next, errCode, msg)) {
            *outVal = in;  // return original on error
            return false;
        }
        current = next;
    }
    *outVal = current;
    return true;
}

}  // namespace dbridge::detail
