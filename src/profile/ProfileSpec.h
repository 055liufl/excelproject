#pragma once
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace dbridge::detail {

enum class ProfileMode { SingleTable, MultiTable, Mixed };

enum class TemporalPhysType {
    String,    // default: string + Qt format
    EpochSec,  // Unix epoch seconds (INTEGER column, db side + datetime slot only)
};

inline std::optional<TemporalPhysType> temporalPhysTypeFromString(const QString& s) {
    if (s == QLatin1String("string"))
        return TemporalPhysType::String;
    if (s == QLatin1String("epochSec"))
        return TemporalPhysType::EpochSec;
    return std::nullopt;
}

struct TemporalSideSpec {
    bool declared = false;
    TemporalPhysType type = TemporalPhysType::String;
    QString format;
    QStringList fallback;  // excel side + type=string only
};

struct TemporalFormatSpec {
    bool declared = false;
    TemporalSideSpec excel;
    TemporalSideSpec db;
};

enum class TemporalSlotKind { None, Date, DateTime, Time };

struct ColumnSpec {
    QString dbColumn;             // SQLite column name
    QString source;               // Excel header name
    QStringList validatorTokens;  // raw tokens e.g. "len<=32", "regex:^...$"

    // Per-column overrides; field-level merge against the profile-level slot.
    TemporalFormatSpec dateFormat;
    TemporalFormatSpec datetimeFormat;
    TemporalFormatSpec timeFormat;
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

// add-export-reverse-lookup: allowed values for exportOnMissing.
// isValid / allowedList are helpers for loader + validator error messages.
struct ExportOnMissing {
    static constexpr const char* kError = "error";
    static constexpr const char* kNull = "null";
    static constexpr const char* kSkip = "skip";
    static bool isValid(const QString& v) {
        return v == QLatin1String(kError) || v == QLatin1String(kNull) || v == QLatin1String(kSkip);
    }
    static QString allowedList() {
        return QStringLiteral("\"error\", \"null\", \"skip\"");
    }
};

// Route-level lookup: pull a set of columns from a reference table G in the
// same SQLite database, by equality on Excel-header values. The looked-up
// columns become route-local dbColumns on this route's payload.
struct LookupSpec {
    QString name;                             // non-empty, unique within route
    QString fromTable;                        // G
    QVector<QPair<QString, QString>> match;   // (G_column, Excel header)
    QVector<QPair<QString, QString>> select;  // (G_column, route-local dbColumn)

    // add-export-reverse-lookup: export-direction controls (do NOT affect import).
    bool exportRoundtrip = true;  // false → skip reverse-lookup, emit H cols as-is
    QString exportOnMissing;      // "error" | "null" | "skip"; default "error" (set by loader)
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
    QString explicitSql;      // only for singleTable/multiTable
    QString classColumn;      // mixed export: which header gets class id
    QStringList columnOrder;  // optional output column sequence (Excel header names)
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

    // add-time-format-profile: profile-level defaults; per-column slots override field-by-field.
    TemporalFormatSpec dateFormat;
    TemporalFormatSpec datetimeFormat;
    TemporalFormatSpec timeFormat;

    // Info-level diagnostics captured during ProfileLoader::load (e.g. "dateFormat overrides
    // date:fmt"). Not errors — non-blocking and carried for CLI/test inspection.
    QStringList loadWarnings;
};

// Side-level override: if column declares an excel/db side, it replaces the entire profile side.
// This avoids unsolvable effective specs (e.g. column type=epochSec inheriting profile format).
inline TemporalFormatSpec effectiveTemporalFor(TemporalSlotKind kind, const ColumnSpec& col,
                                               const ProfileSpec& profile) {
    auto pick =
        [](const ColumnSpec& c, const ProfileSpec& p,
           TemporalSlotKind k) -> QPair<const TemporalFormatSpec*, const TemporalFormatSpec*> {
        switch (k) {
            case TemporalSlotKind::Date:
                return {&c.dateFormat, &p.dateFormat};
            case TemporalSlotKind::DateTime:
                return {&c.datetimeFormat, &p.datetimeFormat};
            case TemporalSlotKind::Time:
                return {&c.timeFormat, &p.timeFormat};
            case TemporalSlotKind::None:
            default:
                return {nullptr, nullptr};
        }
    };
    auto pair = pick(col, profile, kind);
    TemporalFormatSpec out;
    if (!pair.first || !pair.second)
        return out;
    const TemporalFormatSpec& colSlot = *pair.first;
    const TemporalFormatSpec& profileSlot = *pair.second;
    // Legacy path: column reached here via date:xxx validator (no column-level slot declared).
    // Profile-level dateFormat must not override — the validator handles its own parsing.
    if (kind == TemporalSlotKind::Date && !colSlot.declared) {
        for (const auto& t : col.validatorTokens) {
            if (t.startsWith(QStringLiteral("date:")))
                return out;
        }
    }
    out.declared = colSlot.declared || profileSlot.declared;
    // Side-level integral overwrite: column-declared side replaces profile side entirely.
    out.excel = colSlot.excel.declared ? colSlot.excel : profileSlot.excel;
    out.db = colSlot.db.declared ? colSlot.db : profileSlot.db;
    return out;
}

// Determine which temporal slot (if any) governs a column.
// Order of resolution:
//   1. If a column-level slot is `declared`, it determines kind.
//   2. Else if the column has a `date:fmt` validator, treat as Date (legacy compat).
//   3. Else if the column has an explicitly numeric validator (int, int>=N, decimal),
//      skip profile-level temporal fallback — numeric columns are never temporal.
//   4. Else if the profile-level slot is `declared` for some kind, that kind applies.
//   5. Otherwise None.
// When multiple per-column slots happen to be declared (which is itself invalid and rejected at
// load), the first match in (Date, DateTime, Time) order wins — kept defensive.
inline TemporalSlotKind temporalSlotKindFor(const ColumnSpec& col, const ProfileSpec& profile) {
    if (col.dateFormat.declared)
        return TemporalSlotKind::Date;
    if (col.datetimeFormat.declared)
        return TemporalSlotKind::DateTime;
    if (col.timeFormat.declared)
        return TemporalSlotKind::Time;
    for (const auto& t : col.validatorTokens) {
        if (t.startsWith(QStringLiteral("date:")))
            return TemporalSlotKind::Date;
    }
    // Any validator (other than date:xxx already handled above) means the column has an
    // explicit type. Require column-level temporal declarations for such columns; skip
    // the profile-level fallback so that text/numeric columns are not misidentified.
    if (!col.validatorTokens.isEmpty())
        return TemporalSlotKind::None;
    if (profile.dateFormat.declared)
        return TemporalSlotKind::Date;
    if (profile.datetimeFormat.declared)
        return TemporalSlotKind::DateTime;
    if (profile.timeFormat.declared)
        return TemporalSlotKind::Time;
    return TemporalSlotKind::None;
}

}  // namespace dbridge::detail
