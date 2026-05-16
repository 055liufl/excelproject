#pragma once
#include <QSqlDatabase>

#include "SchemaCatalog.h"

namespace dbridge::detail {

class SchemaIntrospector {
   public:
    bool load(QSqlDatabase& db, SchemaCatalog* out, QString* err);

   private:
    bool readTables(QSqlDatabase& db, QStringList* tables, QString* err);
    bool readColumns(QSqlDatabase& db, TableInfo* info, QString* err);
    bool readIndexes(QSqlDatabase& db, TableInfo* info, QString* err);
    bool readForeignKeys(QSqlDatabase& db, TableInfo* info, QString* err);
    bool isAutoIncrement(QSqlDatabase& db, const QString& tableName, const QString& colName);
};

}  // namespace dbridge::detail
