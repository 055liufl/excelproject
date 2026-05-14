#pragma once
#include <QString>

#include "ValidatorChain.h"

namespace dbridge::detail {

// Factory functions for built-in validators.
// Each returns a ValidatorFn or empty function on error.
ValidatorFn makeNotNull();
ValidatorFn makeLenLe(int n);
ValidatorFn makeLenGe(int n);
ValidatorFn makeInt();
ValidatorFn makeIntGe(long long n);
ValidatorFn makeDecimal();
ValidatorFn makeDate(const QString& fmt);
ValidatorFn makeRegex(const QString& pattern, QString* err);
ValidatorFn makeEnum(const QString& valuesStr);

// Compile a single token string into a ValidatorFn.
// Returns empty function on error and sets *err.
ValidatorFn compileToken(const QString& token, QString* err);

}  // namespace dbridge::detail
