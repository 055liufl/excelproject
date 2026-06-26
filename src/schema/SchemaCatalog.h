#pragma once
#include <QHash>
#include <QStringList>
#include <QVector>

namespace dbridge::detail {

struct ColumnInfo {
    QString name;
    QString declaredType;
    bool notNull = false;
    bool primaryKey = false;
    int pkOrder = 0;  // composite PK order (1-based)
    QString defaultValue;
    bool autoIncrement = false;
    bool generated = false;
};

struct IndexInfo {
    QString name;
    bool unique = false;
    QStringList columns;
    // H-02 fix: partial indexes are not valid UPSERT conflict targets because SQLite requires the
    // ON CONFLICT clause to match a non-partial UNIQUE constraint or PRIMARY KEY.
    bool partial = false;
};

struct FkInfo {
    QString refTable;
    QString fromColumn;
    QString toColumn;
};

struct TableInfo {
    QString name;
    QVector<ColumnInfo> columns;
    QVector<IndexInfo> indexes;
    QVector<FkInfo> foreignKeys;

    const ColumnInfo* column(const QString& n) const {
        for (const auto& c : columns) {
            if (c.name == n)
                return &c;
        }
        return nullptr;
    }
};

class SchemaCatalog {
   public:
    void addTable(const TableInfo& t) {
        tables_[t.name] = t;
    }
    void clear() {
        tables_.clear();
    }

    bool hasTable(const QString& name) const {
        return tables_.contains(name);
    }

    const TableInfo* table(const QString& name) const {
        auto it = tables_.find(name);
        return it != tables_.end() ? &it.value() : nullptr;
    }

    QStringList allTables() const {
        return tables_.keys();
    }

   private:
    QHash<QString, TableInfo> tables_;
};

}  // namespace dbridge::detail
