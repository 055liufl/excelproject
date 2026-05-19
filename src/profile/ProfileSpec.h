#pragma once
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

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

// Multi-column / multi-parent foreign-key injection.
// A route's fkInject is a QVector<FkInjectSpec>; each entry names one parent
// table and one or more (parent_column, child_column) pairs to copy at import
// time. Legacy single-object {from:"t.c", to:"t.c"} form is no longer accepted.
struct FkInjectSpec {
    QString fromTable;                       // parent table (must be a route in this profile)
    QVector<QPair<QString, QString>> pairs;  // (parent_column, child_column)
};

// Route-level lookup: pull a set of columns from a reference table G in the
// same SQLite database, by equality on Excel-header values. The looked-up
// columns become route-local dbColumns on this route's payload.
struct LookupSpec {
    QString name;                             // non-empty, unique within route
    QString fromTable;                        // G
    QVector<QPair<QString, QString>> match;   // (G_column, Excel header)
    QVector<QPair<QString, QString>> select;  // (G_column, route-local dbColumn)
};

struct RouteSpec {
    QString table;
    QString parent;  // empty = root
    ConflictSpec conflict;
    QVector<FkInjectSpec> fkInject;  // array; may be empty
    QVector<LookupSpec> lookups;     // array; may be empty
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
