#pragma once
#include "dbridge/Types.h"

#include <QList>

namespace dbridge::detail {

class ErrorCollector {
   public:
    void add(const QString& sheet, int row, const QString& column, const QString& rawValue,
             const QString& code, const QString& message);

    void addTable(const QString& sheet, const QString& code, const QString& message);

    bool empty() const {
        return errors_.isEmpty();
    }
    const QList<RowError>& list() const {
        return errors_;
    }
    QList<RowError>& list() {
        return errors_;
    }

    void clear() {
        errors_.clear();
    }

   private:
    QList<RowError> errors_;
};

}  // namespace dbridge::detail
