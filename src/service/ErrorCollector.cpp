#include "ErrorCollector.h"

namespace dbridge::detail {

void ErrorCollector::add(const QString& sheet, int row, const QString& column,
                         const QString& rawValue, const QString& code, const QString& message) {
    RowError e;
    e.sheet = sheet;
    e.row = row;
    e.column = column;
    e.rawValue = rawValue;
    e.code = code;
    e.message = message;
    errors_.append(e);
}

void ErrorCollector::addTable(const QString& sheet, const QString& code, const QString& message) {
    add(sheet, 0, QString(), QString(), code, message);
}

}  // namespace dbridge::detail
