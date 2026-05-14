#pragma once
#include <QStringList>
#include <QVector>

#include <optional>

namespace dbridge::detail {

enum class ProfileMode { SingleTable, MultiTable, Mixed };

struct ColumnSpec {
    QString dbColumn;             // SQLite column name
    QString source;               // Excel header name
    QStringList validatorTokens;  // raw tokens e.g. "len<=32", "regex:^...$"
};

struct ConflictSpec {
    QStringList columns;  // must match PRIMARY KEY or UNIQUE
};

struct FkInjectSpec {
    QString fromTable;   // "orders"
    QString fromColumn;  // "order_no"
    QString toTable;     // "order_items"
    QString toColumn;    // "order_no"
};

struct RouteSpec {
    QString table;
    QString parent;  // empty = root
    ConflictSpec conflict;
    std::optional<FkInjectSpec> fkInject;
    QVector<ColumnSpec> columns;
};

struct ClassSpec {
    QString id;           // "A" / "B" / "C"
    QString matchEquals;  // MVP: equals only
    QVector<RouteSpec> routes;
};

struct ExportSpec {
    QStringList orderBy;
    QString explicitSql;  // only for singleTable/multiTable
    QString classColumn;  // mixed export: which header gets class id
};

struct ProfileSpec {
    QString name;
    QString sheet;
    int headerRow = 1;
    ProfileMode mode = ProfileMode::SingleTable;
    QVector<RouteSpec> routes;    // SingleTable / MultiTable
    QString discriminatorSource;  // Mixed
    QVector<ClassSpec> classes;   // Mixed
    ExportSpec exportSpec;
};

}  // namespace dbridge::detail
