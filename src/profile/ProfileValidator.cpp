#include "ProfileValidator.h"

#include "dbridge/Errors.h"

#include <QSet>

#include "service/ErrorCollector.h"

namespace dbridge::detail {

bool ProfileValidator::isConflictValid(const ConflictSpec& conflict, const TableInfo& table) {
    if (conflict.columns.isEmpty())
        return false;

    QSet<QString> conflictSet = QSet<QString>::fromList(conflict.columns);

    QStringList pkList;
    for (const auto& c : table.columns) {
        if (c.primaryKey)
            pkList.append(c.name);
    }
    QSet<QString> pkCols = QSet<QString>::fromList(pkList);
    if (!pkCols.isEmpty() && pkCols == conflictSet)
        return true;

    for (const auto& idx : table.indexes) {
        if (!idx.unique)
            continue;
        QSet<QString> idxSet = QSet<QString>::fromList(idx.columns);
        if (idxSet == conflictSet)
            return true;
    }

    return false;
}

bool ProfileValidator::validateRoute(const RouteSpec& route, const QVector<RouteSpec>& allRoutes,
                                     const SchemaCatalog& catalog, const QStringList& excelHeaders,
                                     const QString& sheet, ErrorCollector* errors) {
    bool ok = true;

    if (!catalog.hasTable(route.table)) {
        errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_TABLE_NOT_FOUND),
                         QStringLiteral("Table '") + route.table + QStringLiteral("' not found"));
        return false;
    }
    const TableInfo& tableInfo = *catalog.table(route.table);

    // Conflict columns
    if (route.conflict.columns.isEmpty()) {
        errors->addTable(
            sheet, QString::fromLatin1(err::E_PROFILE_NO_CONFLICT_KEY),
            QStringLiteral("Route '") + route.table + QStringLiteral("' has no conflict columns"));
        ok = false;
    } else {
        for (const auto& cc : route.conflict.columns) {
            if (!tableInfo.column(cc)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_NO_CONFLICT_KEY),
                                 QStringLiteral("Conflict column '") + cc +
                                     QStringLiteral("' not found in ") + route.table);
                ok = false;
            }
        }
        if (ok && !isConflictValid(route.conflict, tableInfo)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_NO_CONFLICT_KEY),
                             QStringLiteral("Conflict columns in '") + route.table +
                                 QStringLiteral("' do not match any PRIMARY KEY or UNIQUE index"));
            ok = false;
        }
    }

    // §3.8 Build dbColumn source map for three-source uniqueness (§3.2, §3.11)
    // source string: "excel", "lookup:<name>", "fkInject:<from>"
    QHash<QString, QString> dbColSource;

    // Excel columns (§3.11 source 1)
    for (const auto& col : route.columns) {
        if (!tableInfo.column(col.dbColumn)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                             QStringLiteral("Column '") + col.dbColumn +
                                 QStringLiteral("' not found in table '") + route.table + '\'');
            ok = false;
        }
        if (!excelHeaders.contains(col.source)) {
            errors->addTable(
                sheet, QString::fromLatin1(err::E_HEADER_NOT_FOUND),
                QStringLiteral("Excel header '") + col.source + QStringLiteral("' not found"));
            ok = false;
        }
        if (dbColSource.contains(col.dbColumn)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                             QStringLiteral("route '") + route.table +
                                 QStringLiteral("': duplicate dbColumn '") + col.dbColumn + '\'');
            ok = false;
        } else {
            dbColSource[col.dbColumn] = QStringLiteral("excel");
        }
    }

    // Build route index for fkInject cross-route checks
    QHash<QString, const RouteSpec*> routeByTable;
    for (const auto& r : allRoutes)
        routeByTable[r.table] = &r;

    // §3.1, §3.2, §3.4, §3.9, §3.10 — Lookup validation
    QSet<QString> lookupNames;
    QSet<QString>
        allLookupTargets;  // all select.second across all lookups (for §3.4 cascade check)

    for (const LookupSpec& lk : route.lookups) {
        // §3.9 name unique within route
        if (lookupNames.contains(lk.name)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                             QStringLiteral("route '") + route.table +
                                 QStringLiteral("': duplicate lookup name '") + lk.name + '\'');
            ok = false;
            continue;
        }
        lookupNames.insert(lk.name);

        // §3.1 fromTable in catalog
        if (!catalog.hasTable(lk.fromTable)) {
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_TABLE_NOT_FOUND),
                             QStringLiteral("route '") + route.table +
                                 QStringLiteral("': lookup '") + lk.name +
                                 QStringLiteral("' from table '") + lk.fromTable +
                                 QStringLiteral("' not found in schema"));
            ok = false;
            continue;
        }
        const TableInfo& gTable = *catalog.table(lk.fromTable);

        // §3.1 match.first must be G column; match.second must be Excel header
        for (const auto& mp : lk.match) {
            if (!gTable.column(mp.first)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': lookup '") + lk.name +
                                     QStringLiteral("' match column '") + mp.first +
                                     QStringLiteral("' not found in '") + lk.fromTable + '\'');
                ok = false;
            }
            if (!excelHeaders.contains(mp.second)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_HEADER_NOT_FOUND),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': lookup '") + lk.name +
                                     QStringLiteral("' match excel header '") + mp.second +
                                     QStringLiteral("' not found"));
                ok = false;
            }
        }

        // §3.1 select.first must be G column; §3.10 select targets unique within this lookup
        QSet<QString> selectTargetsThisLookup;
        for (const auto& sp : lk.select) {
            if (!gTable.column(sp.first)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': lookup '") + lk.name +
                                     QStringLiteral("' select column '") + sp.first +
                                     QStringLiteral("' not found in '") + lk.fromTable + '\'');
                ok = false;
            }
            // §3.10 internal uniqueness
            if (selectTargetsThisLookup.contains(sp.second)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': lookup '") + lk.name +
                                     QStringLiteral("' has duplicate select target '") + sp.second +
                                     '\'');
                ok = false;
            } else {
                selectTargetsThisLookup.insert(sp.second);
            }
            // §3.2+§3.11 cross-source uniqueness
            if (dbColSource.contains(sp.second)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': lookup '") + lk.name +
                                     QStringLiteral("' select target '") + sp.second +
                                     QStringLiteral("' conflicts with ") + dbColSource[sp.second] +
                                     QStringLiteral(" source"));
                ok = false;
            } else {
                dbColSource[sp.second] = QStringLiteral("lookup:") + lk.name;
                allLookupTargets.insert(sp.second);
            }
        }
    }

    // §3.4 No lookup cascading: match.second must not be a lookup output target
    for (const LookupSpec& lk : route.lookups) {
        for (const auto& mp : lk.match) {
            if (allLookupTargets.contains(mp.second)) {
                errors->addTable(
                    sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                    QStringLiteral("route '") + route.table + QStringLiteral("': lookup '") +
                        lk.name + QStringLiteral("' match header '") + mp.second +
                        QStringLiteral("' is a lookup output — cascading not allowed"));
                ok = false;
            }
        }
    }

    // §3.3, §3.5, §3.6, §3.11 — fkInject validation
    QSet<QString> fkChildCols;
    for (const FkInjectSpec& fk : route.fkInject) {
        // §3.3 from must be a declared route in this profile
        if (!routeByTable.contains(fk.fromTable)) {
            QString msg = QStringLiteral("route '") + route.table +
                          QStringLiteral("': fkInject from '") + fk.fromTable + '\'';
            if (catalog.hasTable(fk.fromTable)) {
                msg += QStringLiteral(
                    " exists in schema but is not a route in this profile; use lookups instead");
            } else {
                msg += QStringLiteral(" not found");
            }
            errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_TABLE_NOT_FOUND), msg);
            ok = false;
            continue;
        }
        const RouteSpec* parentRoute = routeByTable[fk.fromTable];

        // Build parent route's declared column sets (Excel + lookup outputs)
        QSet<QString> parentExcelCols;
        for (const auto& col : parentRoute->columns)
            parentExcelCols.insert(col.dbColumn);
        QSet<QString> parentLookupTargets;
        for (const LookupSpec& lk : parentRoute->lookups) {
            for (const auto& sp : lk.select)
                parentLookupTargets.insert(sp.second);
        }

        // §3.5 All pairs must be either all lookup-derived or all Excel-derived
        bool groupIsLookup = false;
        bool firstPair = true;
        bool mixedGroup = false;
        for (const auto& pair : fk.pairs) {
            bool pairIsLookup = parentLookupTargets.contains(pair.first);
            if (firstPair) {
                groupIsLookup = pairIsLookup;
                firstPair = false;
            } else if (pairIsLookup != groupIsLookup) {
                mixedGroup = true;
                break;
            }
        }
        if (mixedGroup) {
            errors->addTable(
                sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                QStringLiteral("route '") + route.table + QStringLiteral("': fkInject from='") +
                    fk.fromTable +
                    QStringLiteral("' mixes lookup-derived and Excel-derived parent_columns; "
                                   "split into two separate groups"));
            ok = false;
        }

        for (const auto& pair : fk.pairs) {
            // §3.3 pair.first in parent route's Excel columns or lookup outputs
            if (!parentExcelCols.contains(pair.first) &&
                !parentLookupTargets.contains(pair.first)) {
                errors->addTable(
                    sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                    QStringLiteral("route '") + route.table + QStringLiteral("': fkInject from='") +
                        fk.fromTable + QStringLiteral("' parent_column '") + pair.first +
                        QStringLiteral("' not found in parent route columns or lookup outputs"));
                ok = false;
            }
            // §3.3 pair.second in target table
            if (!tableInfo.column(pair.second)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': fkInject child_column '") + pair.second +
                                     QStringLiteral("' not found in table '") + route.table + '\'');
                ok = false;
            }
            // §3.6 child_column unique across all fkInject groups on this route
            if (fkChildCols.contains(pair.second)) {
                errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                                 QStringLiteral("route '") + route.table +
                                     QStringLiteral("': fkInject child_column '") + pair.second +
                                     QStringLiteral("' appears in multiple injection groups"));
                ok = false;
            } else {
                fkChildCols.insert(pair.second);
                // §3.11 cross-source uniqueness
                if (dbColSource.contains(pair.second)) {
                    errors->addTable(sheet, QString::fromLatin1(err::E_PROFILE_COLUMN_NOT_FOUND),
                                     QStringLiteral("route '") + route.table +
                                         QStringLiteral("': fkInject child_column '") +
                                         pair.second + QStringLiteral("' conflicts with ") +
                                         dbColSource[pair.second] + QStringLiteral(" source"));
                    ok = false;
                } else {
                    dbColSource[pair.second] = QStringLiteral("fkInject:") + fk.fromTable;
                }
            }
        }
    }

    return ok;
}

bool ProfileValidator::validateRoutes(const QVector<RouteSpec>& routes,
                                      const SchemaCatalog& catalog, const QStringList& excelHeaders,
                                      const QString& sheet, ErrorCollector* errors) {
    bool ok = true;
    for (const auto& route : routes) {
        if (!validateRoute(route, routes, catalog, excelHeaders, sheet, errors))
            ok = false;
    }

    // Parent references
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

bool ProfileValidator::validateForExport(const ProfileSpec& profile, const SchemaCatalog& catalog,
                                         ErrorCollector* errors) {
    // H-03 fix: build a synthetic excelHeaders list that contains every column source
    // declared in the profile so that header-existence checks always pass.
    // Discriminator and Excel-header checks are import-only concerns — skip them here.
    QStringList syntheticHeaders;
    auto collectHeaders = [&](const QVector<RouteSpec>& routes) {
        for (const auto& route : routes) {
            for (const auto& col : route.columns)
                syntheticHeaders.append(col.source);
            for (const auto& lk : route.lookups) {
                for (const auto& mp : lk.match)
                    syntheticHeaders.append(mp.second);
            }
        }
    };
    if (profile.mode == ProfileMode::Mixed) {
        for (const auto& cls : profile.classes)
            collectHeaders(cls.routes);
        if (!profile.discriminatorSource.isEmpty())
            syntheticHeaders.append(profile.discriminatorSource);
    } else {
        collectHeaders(profile.routes);
    }
    return validate(profile, catalog, syntheticHeaders, errors, /*importMode=*/false);
}

bool ProfileValidator::validate(const ProfileSpec& profile, const SchemaCatalog& catalog,
                                const QStringList& excelHeaders, ErrorCollector* errors,
                                bool importMode) {
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
        // M-05 fix: discriminator.source is only required for import; skip for export.
        if (importMode && profile.discriminatorSource.isEmpty()) {
            errors->addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE),
                             QStringLiteral("mixed mode requires discriminator.source"));
            ok = false;
        }
        if (importMode && !profile.discriminatorSource.isEmpty() &&
            !excelHeaders.contains(profile.discriminatorSource)) {
            errors->addTable(profile.sheet, QString::fromLatin1(err::E_HEADER_NOT_FOUND),
                             QStringLiteral("Discriminator source '") +
                                 profile.discriminatorSource +
                                 QStringLiteral("' not found in Excel headers"));
            ok = false;
        }
        for (const auto& cls : profile.classes) {
            if (!validateRoutes(cls.routes, catalog, excelHeaders, profile.sheet, errors))
                ok = false;
        }
    } else {
        if (!validateRoutes(profile.routes, catalog, excelHeaders, profile.sheet, errors))
            ok = false;
    }

    // Check orderBy columns with non-sortable temporal dbFormat.
    // A dbFormat is dict-sort-safe only when it starts with "yyyy" (year is the MSB).
    if (!profile.exportSpec.orderBy.isEmpty()) {
        // Build a flat dbColumn → ColumnSpec pointer map across all routes.
        QHash<QString, const ColumnSpec*> colByDb;
        auto indexRoutes = [&](const QVector<RouteSpec>& routes) {
            for (const auto& route : routes) {
                for (const auto& col : route.columns) {
                    if (!colByDb.contains(col.dbColumn))
                        colByDb[col.dbColumn] = &col;
                }
            }
        };
        if (profile.mode == ProfileMode::Mixed) {
            for (const auto& cls : profile.classes)
                indexRoutes(cls.routes);
        } else {
            indexRoutes(profile.routes);
        }

        for (const QString& ob : profile.exportSpec.orderBy) {
            QString colName = ob.contains('.') ? ob.section('.', -1) : ob;
            auto it = colByDb.find(colName);
            if (it == colByDb.end())
                continue;
            const ColumnSpec& col = *it.value();
            TemporalSlotKind kind = temporalSlotKindFor(col, profile);
            if (kind == TemporalSlotKind::None)
                continue;
            TemporalFormatSpec eff = effectiveTemporalFor(kind, col, profile);
            // Skip non-string types (e.g. epochSec): INTEGER columns sort numerically, always safe.
            if (eff.db.type != TemporalPhysType::String)
                continue;
            if (!eff.db.format.startsWith(QStringLiteral("yyyy"))) {
                errors->addTableWarning(
                    profile.sheet, QString::fromLatin1(err::W_TIME_ORDERBY_NONSORTABLE),
                    QStringLiteral("orderBy column '") + colName +
                        QStringLiteral("' has db.format '") + eff.db.format +
                        QStringLiteral("' which does not start with 'yyyy' — dictionary sort order "
                                       "may not match chronological order"));
            }
        }
    }

    // add-export-column-order: validate columnOrder field.
    if (!profile.exportSpec.columnOrder.isEmpty()) {
        // 3.4: mutually exclusive with explicitSql
        if (!profile.exportSpec.explicitSql.isEmpty()) {
            errors->addTable(
                profile.sheet, QString::fromLatin1(err::E_EXPORT_ORDER_WITH_RAW_SQL),
                QStringLiteral("export.columnOrder cannot be used together with export.sql; "
                               "raw SQL owns column ordering"));
            ok = false;
        }

        // 3.1: build known-header set (all ColumnSpec.source values across all routes)
        // add-export-reverse-lookup 3.2: A headers (match[].Excel_header for roundtrip=true
        // lookups) are added; H dbColumn names for roundtrip=true are removed from accepted set;
        // H dbColumn names for roundtrip=false are added (they appear in Excel output as-is).
        QSet<QString> knownHeaders;
        auto collectSources = [&](const QVector<RouteSpec>& routes) {
            for (const auto& route : routes) {
                for (const auto& col : route.columns)
                    knownHeaders.insert(col.source);
                // Add/remove lookup-related headers to reflect post-substitution output set
                for (const auto& lk : route.lookups) {
                    if (lk.exportRoundtrip) {
                        // A headers appear in output; H dbColumns do not
                        for (const auto& mp : lk.match)
                            knownHeaders.insert(mp.second);  // A = match[].Excel_header
                        for (const auto& sp : lk.select)
                            knownHeaders.remove(sp.second);  // H = select[].dbColumn
                    } else {
                        // H dbColumns appear in output verbatim
                        for (const auto& sp : lk.select)
                            knownHeaders.insert(sp.second);  // H = select[].dbColumn
                    }
                }
            }
        };
        if (profile.mode == ProfileMode::Mixed) {
            for (const auto& cls : profile.classes)
                collectSources(cls.routes);
            // classColumn is a synthetic header — valid in columnOrder for Mixed mode
            if (!profile.exportSpec.classColumn.isEmpty())
                knownHeaders.insert(profile.exportSpec.classColumn);
        } else {
            collectSources(profile.routes);
        }

        // Hint: first up-to-5 known headers for error messages
        auto knownHint = [&]() -> QString {
            QStringList sample = knownHeaders.values().mid(0, 5);
            return QStringLiteral(" (known headers: ") + sample.join(QStringLiteral(", ")) +
                   QStringLiteral(")");
        };

        // 3.2 + 3.3: unknown header and duplicate checks
        QSet<QString> seen;
        for (const QString& h : profile.exportSpec.columnOrder) {
            if (seen.contains(h)) {
                errors->addTable(profile.sheet, QString::fromLatin1(err::E_EXPORT_DUPLICATE_ORDER),
                                 QStringLiteral("export.columnOrder contains duplicate entry '") +
                                     h + QStringLiteral("'"));
                ok = false;
                continue;
            }
            seen.insert(h);
            if (!knownHeaders.contains(h)) {
                errors->addTable(profile.sheet, QString::fromLatin1(err::E_EXPORT_UNKNOWN_HEADER),
                                 QStringLiteral("export.columnOrder entry '") + h +
                                     QStringLiteral("' does not match any column source") +
                                     knownHint());
                ok = false;
            }
        }
    }

    return ok;
}

}  // namespace dbridge::detail
