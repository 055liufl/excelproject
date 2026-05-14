#include "ProfileValidator.h"

#include "dbridge/Errors.h"

#include <QSet>

#include "service/ErrorCollector.h"

namespace dbridge::detail {

bool ProfileValidator::isConflictValid(const ConflictSpec& conflict, const TableInfo& table) {
    if (conflict.columns.isEmpty())
        return false;

    QSet<QString> conflictSet = QSet<QString>::fromList(conflict.columns);

    // Check if columns form a primary key set
    QStringList pkList;
    for (const auto& c : table.columns) {
        if (c.primaryKey)
            pkList.append(c.name);
    }
    QSet<QString> pkCols = QSet<QString>::fromList(pkList);
    if (!pkCols.isEmpty() && pkCols == conflictSet)
        return true;

    // Check if columns match a unique index
    for (const auto& idx : table.indexes) {
        if (!idx.unique)
            continue;
        QSet<QString> idxSet = QSet<QString>::fromList(idx.columns);
        if (idxSet == conflictSet)
            return true;
    }

    return false;
}

bool ProfileValidator::validateRoute(const RouteSpec& route, const SchemaCatalog& catalog,
                                     const QStringList& excelHeaders, const QString& sheet,
                                     ErrorCollector* errors) {
    bool ok = true;

    // Check table exists
    if (!catalog.hasTable(route.table)) {
        errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_TABLE_NOT_FOUND),
                         QStringLiteral("Table '") + route.table + QStringLiteral("' not found"));
        return false;  // can't validate columns without table
    }

    const TableInfo& tableInfo = *catalog.table(route.table);

    // Check conflict columns
    if (route.conflict.columns.isEmpty()) {
        errors->addTable(
            sheet, QString::fromLatin1(err::E_PROFILE_NO_CONFLICT_KEY),
            QStringLiteral("Route '") + route.table + QStringLiteral("' has no conflict columns"));
        ok = false;
    } else {
        // Check each conflict column exists in table
        for (const auto& cc : route.conflict.columns) {
            if (!tableInfo.column(cc)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_NO_CONFLICT_KEY),
                                 QStringLiteral("Conflict column '") + cc +
                                     QStringLiteral("' not found in ") + route.table);
                ok = false;
            }
        }
        // Check conflict matches PK or unique index
        if (ok && !isConflictValid(route.conflict, tableInfo)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_NO_CONFLICT_KEY),
                             QStringLiteral("Conflict columns in '") + route.table +
                                 QStringLiteral("' do not match any PRIMARY KEY or UNIQUE index"));
            ok = false;
        }
    }

    // Check each mapped column
    for (const auto& col : route.columns) {
        if (!tableInfo.column(col.dbColumn)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                             QStringLiteral("Column '") + col.dbColumn +
                                 QStringLiteral("' not found in table '") + route.table + '\'');
            ok = false;
        }
        // Check source header exists in Excel
        if (!excelHeaders.contains(col.source)) {
            errors->addTable(
                sheet, QString::fromLatin1(err::E_HEADER_NOT_FOUND),
                QStringLiteral("Excel header '") + col.source + QStringLiteral("' not found"));
            ok = false;
        }
    }

    // Validate fkInject if present
    if (route.fkInject.has_value()) {
        const FkInjectSpec& fk = route.fkInject.value();
        // fromTable must match route.parent (spec §5.4)
        if (!route.parent.isEmpty() && fk.fromTable != route.parent) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                             QStringLiteral("fkInject.from table '") + fk.fromTable +
                                 QStringLiteral("' must match route parent '") + route.parent +
                                 '\'');
            ok = false;
        }
        if (!catalog.hasTable(fk.fromTable)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_TABLE_NOT_FOUND),
                             QStringLiteral("fkInject.from table '") + fk.fromTable +
                                 QStringLiteral("' not found"));
            ok = false;
        } else {
            const TableInfo& fromTable = *catalog.table(fk.fromTable);
            if (!fromTable.column(fk.fromColumn)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                                 QStringLiteral("fkInject.from column '") + fk.fromColumn +
                                     QStringLiteral("' not found in '") + fk.fromTable + '\'');
                ok = false;
            }
        }
        if (!tableInfo.column(fk.toColumn)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                             QStringLiteral("fkInject.to column '") + fk.toColumn +
                                 QStringLiteral("' not found in '") + route.table + '\'');
            ok = false;
        }
    }

    return ok;
}

bool ProfileValidator::validateRoutes(const QVector<RouteSpec>& routes,
                                      const SchemaCatalog& catalog, const QStringList& excelHeaders,
                                      const QString& sheet, ErrorCollector* errors) {
    bool ok = true;
    for (const auto& route : routes) {
        if (!validateRoute(route, catalog, excelHeaders, sheet, errors))
            ok = false;
    }

    // Validate parent references
    QStringList tableNameList;
    for (const auto& r : routes)
        tableNameList.append(r.table);
    QSet<QString> tableNames = QSet<QString>::fromList(tableNameList);
    for (const auto& r : routes) {
        if (!r.parent.isEmpty() && !tableNames.contains(r.parent)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_TABLE_NOT_FOUND),
                             QStringLiteral("Route '") + r.table + QStringLiteral("' parent '") +
                                 r.parent + QStringLiteral("' is not in routes"));
            ok = false;
        }
    }

    return ok;
}

bool ProfileValidator::validate(const ProfileSpec& profile, const SchemaCatalog& catalog,
                                const QStringList& excelHeaders, ErrorCollector* errors) {
    bool ok = true;

    if (profile.name.isEmpty()) {
        errors->addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                         QStringLiteral("profileName is empty"));
        ok = false;
    }
    if (profile.sheet.isEmpty()) {
        errors->addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                         QStringLiteral("sheet is empty"));
        ok = false;
    }
    if (profile.headerRow < 1) {
        errors->addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                         QStringLiteral("headerRow must be >= 1"));
        ok = false;
    }

    if (profile.mode == ProfileMode::Mixed) {
        if (profile.discriminatorSource.isEmpty()) {
            errors->addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                             QStringLiteral("mixed mode requires discriminator.source"));
            ok = false;
        }
        // Check discriminator source in headers
        if (!profile.discriminatorSource.isEmpty() &&
            !excelHeaders.contains(profile.discriminatorSource)) {
            errors->addTable(profile.sheet, QString::fromLatin1(err::E_HEADER_NOT_FOUND),
                             QStringLiteral("Discriminator source '") +
                                 profile.discriminatorSource +
                                 QStringLiteral("' not found in Excel headers"));
            ok = false;
        }
        for (const auto& cls : profile.classes) {
            if (!validateRoutes(cls.routes, catalog, excelHeaders, profile.sheet, errors)) {
                ok = false;
            }
        }
    } else {
        if (!validateRoutes(profile.routes, catalog, excelHeaders, profile.sheet, errors)) {
            ok = false;
        }
    }

    return ok;
}

}  // namespace dbridge::detail
