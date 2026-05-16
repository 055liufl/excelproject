#pragma once
#include <QStringList>
#include <QVariant>

#include <functional>
#include <vector>

namespace dbridge::detail {

using ValidatorFn =
    std::function<bool(const QVariant& in, QVariant* out, QString* errCode, QString* msg)>;

class ValidatorChain {
   public:
    // Compile a list of tokens into a chain of validator functions.
    // Returns false if any token is malformed (E_PROFILE_PARSE level).
    bool compile(const QStringList& tokens, QString* err);

    // Run the chain on a value. Returns false on first failure.
    // out_val receives the normalized value (may be same as in).
    bool run(const QVariant& in, QVariant* outVal, QString* errCode, QString* msg) const;

    bool isEmpty() const {
        return fns_.empty();
    }

   private:
    std::vector<ValidatorFn> fns_;
};

}  // namespace dbridge::detail
