#include "ImportService.h"

#include "dbridge/Errors.h"

#include <QSqlError>
#include <QSqlQuery>

#include "ErrorCollector.h"
#include "excel/ExcelReader.h"
#include "mapping/BatchUniqueness.h"
#include "mapping/Mapper.h"
#include "mapping/Router.h"
#include "mapping/TopoSorter.h"
#include "profile/ProfileValidator.h"
#include "sql/SqlBuilder.h"
#include "validation/ForeignKeyPreflight.h"

namespace dbridge::detail {

// Inject FK values from parent payloads into child payloads.
static void doFkInject(QVector<RoutePayload>& payloads, const QVector<RouteSpec>& routes) {
    QHash<QString, int> tableToPayloadIdx;
    for (int i = 0; i < payloads.size(); ++i) {
        tableToPayloadIdx[payloads[i].table] = i;
    }

    for (int i = 0; i < routes.size(); ++i) {
        const RouteSpec& route = routes[i];
        if (!route.fkInject.has_value())
            continue;
        const FkInjectSpec& fk = route.fkInject.value();

        // Find parent payload
        auto parentIt = tableToPayloadIdx.find(fk.fromTable);
        if (parentIt == tableToPayloadIdx.end())
            continue;

        const RoutePayload& parentPayload = payloads[parentIt.value()];
        int fromIdx = parentPayload.indexOf(fk.fromColumn);
        if (fromIdx < 0)
            continue;
        QVariant fkVal = parentPayload.binds[fromIdx];

        // Inject into child payload
        RoutePayload& childPayload = payloads[i];
        int toIdx = childPayload.indexOf(fk.toColumn);
        if (toIdx >= 0) {
            childPayload.binds[toIdx] = fkVal;  // overwrite
        } else {
            childPayload.dbColumns.append(fk.toColumn);
            childPayload.binds.append(fkVal);
            toIdx = childPayload.dbColumns.size() - 1;
        }

        // Update conflictVals if the FK column is a conflict column
        for (int ci = 0; ci < childPayload.conflictKey.size(); ++ci) {
            if (childPayload.conflictKey[ci] == fk.toColumn) {
                if (ci < childPayload.conflictVals.size()) {
                    childPayload.conflictVals[ci] = fkVal;
                }
            }
        }
    }
}

// Determine if a route has children in the routes list
static bool routeHasChildren(const QString& table, const QVector<RouteSpec>& routes) {
    for (const auto& r : routes) {
        if (r.parent == table)
            return true;
    }
    return false;
}

ImportResult ImportService::run(const ProfileSpec& profile, const SchemaCatalog& catalog,
                                const QString& xlsxPath, const ImportOptions& options,
                                QSqlDatabase& db) {
    ImportResult result;
    ErrorCollector errors;

    // --- Phase A: Open and read xlsx header ---
    ExcelReader reader;
    QString xlsxErr;
    if (!reader.open(xlsxPath, &xlsxErr)) {
        errors.addTable(profile.sheet, QString::fromLatin1(err::E_OPEN_XLSX), xlsxErr);
        result.errors = errors.list();
        return result;
    }

    QString sheetName = options.sheetName.isEmpty() ? profile.sheet : options.sheetName;
    if (!reader.selectSheet(sheetName, &xlsxErr)) {
        errors.addTable(profile.sheet, QString::fromLatin1(err::E_OPEN_XLSX), xlsxErr);
        result.errors = errors.list();
        return result;
    }

    if (!reader.readHeader(profile.headerRow, &xlsxErr)) {
        errors.addTable(profile.sheet, QString::fromLatin1(err::E_HEADER_NOT_FOUND), xlsxErr);
        result.errors = errors.list();
        return result;
    }

    // --- Phase B: Profile validation ---
    ProfileValidator validator;
    if (!validator.validate(profile, catalog, reader.headers(), &errors)) {
        result.errors = errors.list();
        return result;
    }

    // --- Prepare routes and validators ---
    TopoSorter topoSorter;
    Mapper mapper;
    Router router;

    // For Mixed mode, initialize router
    if (profile.mode == ProfileMode::Mixed) {
        QString routerErr;
        if (!router.init(profile, &routerErr)) {
            errors.addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE), routerErr);
            result.errors = errors.list();
            return result;
        }
    }

    // Compile validators: for non-mixed, all routes; for mixed, each class
    ValidatorMap validatorMap;
    if (profile.mode == ProfileMode::Mixed) {
        for (const auto& cls : profile.classes) {
            QString vErr;
            if (!mapper.compileValidators(cls.routes, cls.id, &validatorMap, &vErr)) {
                errors.addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE), vErr);
                result.errors = errors.list();
                return result;
            }
        }
    } else {
        QString vErr;
        if (!mapper.compileValidators(profile.routes, QString(), &validatorMap, &vErr)) {
            errors.addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE), vErr);
            result.errors = errors.list();
            return result;
        }
    }

    // Topo-sort all route sets
    QHash<QString, QVector<RouteSpec>> topoRoutes;  // classId -> sorted routes
    if (profile.mode == ProfileMode::Mixed) {
        for (const auto& cls : profile.classes) {
            QVector<RouteSpec> sorted;
            QString topoErr;
            if (!topoSorter.sort(cls.routes, &sorted, &topoErr)) {
                errors.addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_TOPOLOGY_CYCLE),
                                topoErr);
                result.errors = errors.list();
                return result;
            }
            topoRoutes[cls.id] = sorted;
        }
    } else {
        QVector<RouteSpec> sorted;
        QString topoErr;
        if (!topoSorter.sort(profile.routes, &sorted, &topoErr)) {
            errors.addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_TOPOLOGY_CYCLE),
                            topoErr);
            result.errors = errors.list();
            return result;
        }
        topoRoutes[QString()] = sorted;
    }

    // --- Phase C: Row mapping + validation ---
    QVector<RowContext> contexts;
    BatchUniqueness batchUniq;

    for (int r = reader.firstDataRow(); r <= reader.lastRow(); ++r) {
        result.readRows++;

        RowContext ctx;
        ctx.excelRow = r;

        QVector<RouteSpec>* routesPtr = nullptr;
        if (profile.mode == ProfileMode::Mixed) {
            QVariant discVal = reader.cellBySource(r, router.discriminatorSource());
            const ClassSpec* cls = router.match(discVal);
            if (!cls) {
                errors.add(sheetName, r, router.discriminatorSource(), discVal.toString(),
                           QString::fromLatin1(err::E_ROUTE_UNMATCHED),
                           QStringLiteral("Row does not match any class discriminator value '") +
                               discVal.toString() + '\'');
                continue;
            }
            ctx.classId = cls->id;
            routesPtr = &topoRoutes[cls->id];
        } else {
            routesPtr = &topoRoutes[QString()];
        }

        // Map row to payloads
        ctx.payloads = mapper.map(*routesPtr, r, ctx.classId, reader, validatorMap, &errors);

        // FK injection
        doFkInject(ctx.payloads, *routesPtr);

        // Batch uniqueness check
        for (int pi = 0; pi < ctx.payloads.size(); ++pi) {
            const RoutePayload& payload = ctx.payloads[pi];
            bool hasChildren = routeHasChildren(payload.table, *routesPtr);
            batchUniq.check(payload, r, hasChildren, &errors, sheetName);
        }

        contexts.append(ctx);
    }

    // FK preflight check
    if (profile.mode != ProfileMode::Mixed) {
        ForeignKeyPreflight fkPreflight;
        fkPreflight.check(contexts, topoRoutes.value(QString()), db, sheetName, &errors);
    } else {
        // For mixed, check each class separately
        for (const auto& cls : profile.classes) {
            QVector<RowContext> clsContexts;
            for (const auto& ctx : contexts) {
                if (ctx.classId == cls.id)
                    clsContexts.append(ctx);
            }
            ForeignKeyPreflight fkPreflight;
            fkPreflight.check(clsContexts, topoRoutes.value(cls.id), db, sheetName, &errors);
        }
    }

    // If any errors, do NOT write
    if (!errors.empty()) {
        result.errors = errors.list();
        return result;
    }

    // --- Phase D: Write (single transaction) ---
    if (!db.transaction()) {
        errors.addTable(sheetName, QString::fromLatin1(err::E_DB_UPSERT),
                        QStringLiteral("Failed to start transaction: ") + db.lastError().text());
        result.errors = errors.list();
        return result;
    }

    SqlBuilder sqlBuilder;
    QHash<QString, QSqlQuery> preparedQueries;  // sql -> prepared query

    bool writeOk = true;
    for (const auto& ctx : contexts) {
        for (const auto& payload : ctx.payloads) {
            if (payload.dbColumns.isEmpty())
                continue;

            UpsertSql upsert = sqlBuilder.buildUpsert(payload);
            if (upsert.sql.isEmpty())
                continue;

            QSqlQuery* qptr = nullptr;
            auto it = preparedQueries.find(upsert.sql);
            if (it == preparedQueries.end()) {
                QSqlQuery q(db);
                if (!q.prepare(upsert.sql)) {
                    errors.add(sheetName, ctx.excelRow, QString(), QString(),
                               QString::fromLatin1(err::E_DB_UPSERT),
                               QStringLiteral("Failed to prepare SQL: ") + q.lastError().text() +
                                   QStringLiteral(" SQL: ") + upsert.sql);
                    writeOk = false;
                    break;
                }
                preparedQueries[upsert.sql] = std::move(q);
                qptr = &preparedQueries[upsert.sql];
            } else {
                qptr = &it.value();
            }

            // Bind values
            for (int bi = 0; bi < payload.binds.size(); ++bi) {
                qptr->addBindValue(payload.binds[bi]);
            }

            if (!qptr->exec()) {
                errors.add(sheetName, ctx.excelRow, QString(), QString(),
                           QString::fromLatin1(err::E_DB_UPSERT),
                           QStringLiteral("Upsert failed: ") + qptr->lastError().text());
                writeOk = false;
                break;
            }
        }
        if (!writeOk)
            break;
        result.writtenRows++;
    }

    if (writeOk) {
        if (!db.commit()) {
            db.rollback();
            errors.addTable(sheetName, QString::fromLatin1(err::E_DB_UPSERT),
                            QStringLiteral("Commit failed: ") + db.lastError().text());
            result.writtenRows = 0;
        } else {
            result.ok = true;
        }
    } else {
        db.rollback();
        result.writtenRows = 0;
    }

    result.errors = errors.list();
    return result;
}

}  // namespace dbridge::detail
