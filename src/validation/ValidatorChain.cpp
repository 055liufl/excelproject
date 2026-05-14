#include "ValidatorChain.h"

#include "Validators.h"

namespace dbridge::detail {

bool ValidatorChain::compile(const QStringList& tokens, QString* err) {
    fns_.clear();
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
