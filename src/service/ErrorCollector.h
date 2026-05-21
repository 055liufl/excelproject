#pragma once
#include "dbridge/Types.h"

#include <QList>

namespace dbridge::detail {

class ErrorCollector {
   public:
    void add(const QString& sheet, int row, const QString& column, const QString& rawValue,
             const QString& code, const QString& message);

    void addTable(const QString& sheet, const QString& code, const QString& message);

    // Non-blocking warnings (e.g. W_TIME_ORDERBY_NONSORTABLE). Stored in a separate
    // list so the existing "if errors then abort" callers behave unchanged.
    void addWarning(const QString& sheet, int row, const QString& column, const QString& rawValue,
                    const QString& code, const QString& message);
    void addTableWarning(const QString& sheet, const QString& code, const QString& message);

    bool empty() const {
        return errors_.isEmpty();
    }
    const QList<RowError>& list() const {
        return errors_;
    }
    QList<RowError>& list() {
        return errors_;
    }
    const QList<RowError>& warnings() const {
        return warnings_;
    }
    QList<RowError>& warnings() {
        return warnings_;
    }

    void clear() {
        errors_.clear();
        warnings_.clear();
    }

   private:
    QList<RowError> errors_;
    QList<RowError> warnings_;
};

}  // namespace dbridge::detail
