#include "ImportService.h"

#include "dbridge/Errors.h"

#include <QSet>
#include <QSqlError>
#include <QSqlQuery>

#include "ErrorCollector.h"
#include "excel/ExcelReader.h"
#include "mapping/BatchUniqueness.h"
#include "mapping/FkInjector.h"
#include "mapping/Mapper.h"
#include "mapping/Router.h"
#include "mapping/TopoSorter.h"
#include "profile/ProfileValidator.h"
#include "schema/SchemaCatalog.h"
#include "sql/SqlBuilder.h"
#include "validation/ForeignKeyPreflight.h"
#include <functional>

namespace dbridge::detail {

namespace {

// ---- Lookup helpers --------------------------------------------------------

// Build a stable identity key for a LookupSpec: (from, match-pairs, select-pairs).
// Two lookups sharing the same identity key can reuse one prefetch result.
QString buildIdentityKey(const LookupSpec& lk) {
    QStringList matchParts, selectParts;
    for (const auto& p : lk.match)
        matchParts.append(p.first + QLatin1Char('=') + p.second);
    for (const auto& p : lk.select)
        selectParts.append(p.first + QStringLiteral("->") + p.second);
    return lk.fromTable + QStringLiteral("::") + matchParts.join(QLatin1Char(',')) +
           QStringLiteral("::") + selectParts.join(QLatin1Char(','));
}

// Cast a raw QVariant to the affinity of a G-table column.
// Returns an invalid QVariant on type-mismatch (cast failure).
// Numeric zero is a valid (non-empty) value.
QVariant castToAffinity(const QVariant& raw, const ColumnInfo& gCol) {
    // Applies SQLite column affinity coercion per row-lookup spec §affinity-coercion table.
    // Priority order matches SQLite's own affinity determination rules.
    QString type = gCol.declaredType.trimmed().toUpper();
    if (type.contains(QStringLiteral("INT"))) {
        bool ok;
        qlonglong iv = raw.toLongLong(&ok);
        if (!ok)
            return QVariant();
        return QVariant(iv);
    }
    if (type.contains(QStringLiteral("REAL")) || type.contains(QStringLiteral("FLOA")) ||
        type.contains(QStringLiteral("DOUB"))) {
        bool ok;
        double dv = raw.toDouble(&ok);
        if (!ok)
            return QVariant();
        return QVariant(dv);
    }
    if (type.contains(QStringLiteral("BLOB"))) {
        if (raw.type() == QVariant::ByteArray)
            return raw;
        const QByteArray ba = raw.toByteArray();
        return ba.isEmpty() ? QVariant() : QVariant(ba);
    }
    // TEXT affinity: declared type contains CHAR, CLOB, or TEXT.
    if (type.contains(QStringLiteral("CHAR")) || type.contains(QStringLiteral("CLOB")) ||
        type.contains(QStringLiteral("TEXT"))) {
        return QVariant(raw.toString());
    }
    // H-01 fix: empty type (BLOB/none affinity) and NUMERIC affinity (NUMERIC, DECIMAL,
    // BOOLEAN, DATE, DATETIME, etc.) must preserve the raw QVariant, not coerce to string.
    // Per row-lookup spec: "BLOB or no declared type → preserve raw QVariant".
    return raw;
}

// Serialize a match-key tuple to a stable, type-aware string for use as QHash key.
// H-5/H-6 fix: prefix each value with its type tag so that 1 (int), "1" (string) and
// 1.0 (double) produce different keys, preserving SQLite strict equality semantics.
QString makeTupleKey(const QVector<QVariant>& values) {
    QStringList parts;
    for (const auto& v : values) {
        if (v.isNull()) {
            parts.append(QStringLiteral("\x00null"));
        } else {
            switch (v.type()) {
                case QVariant::LongLong:
                case QVariant::Int:
                case QVariant::ULongLong:
                case QVariant::UInt:
                    parts.append(QStringLiteral("i:") + QString::number(v.toLongLong()));
                    break;
                case QVariant::Double:
                    parts.append(QStringLiteral("d:") + QString::number(v.toDouble(), 'g', 17));
                    break;
                case QVariant::ByteArray:
                    parts.append(QStringLiteral("b:") +
                                 QString::fromLatin1(v.toByteArray().toHex()));
                    break;
                default:
                    parts.append(QStringLiteral("s:") + v.toString());
                    break;
            }
        }
    }
    return parts.join(QLatin1Char('\x1F'));
}

struct LookupHit {
    QVector<QVariant> values;  // select column values in lk.select order
    int hitCount = 0;
};

// identityKey -> (tupleKey -> hit)
using LookupCache = QHash<QString, QHash<QString, LookupHit>>;

}  // anonymous namespace

// ---- Lookup application (Phase B) -----------------------------------------

// Returns the set of route indices where any lookup failed (used to seed cascade suppression).
static QSet<int> applyLookups(QVector<RoutePayload>& payloads, const QVector<RouteSpec>& routes,
                              const LookupCache& cache, const SchemaCatalog& catalog,
                              const ExcelReader& reader, int row, const QString& sheet,
                              ErrorCollector* errors) {
    QSet<int> failedRoutes;
    for (int i = 0; i < routes.size(); ++i) {
        const RouteSpec& route = routes[i];
        if (route.lookups.isEmpty())
            continue;

        RoutePayload& payload = payloads[i];
        bool routeFailed = false;  // true if any lookup on this route erred this row

        for (const LookupSpec& lk : route.lookups) {
            const TableInfo* gTable = catalog.table(lk.fromTable);
            QString identityKey = buildIdentityKey(lk);

            // Build match key with affinity cast
            QVector<QVariant> matchVals;
            bool hasError = false;
            QStringList matchHeaders;

            for (const auto& mp : lk.match) {
                matchHeaders.append(mp.second);
                QVariant raw = reader.cellBySource(row, mp.second);

                // §5.2 empty: null or trimmed-empty; numeric zero is NOT empty
                bool isEmpty = raw.isNull();
                if (!isEmpty && raw.toString().trimmed().isEmpty())
                    isEmpty = true;

                if (isEmpty) {
                    errors->add(sheet, row, mp.second, raw.toString(),
                                QString::fromLatin1(err::E_LOOKUP_KEY_EMPTY),
                                QStringLiteral("route '") + route.table +
                                    QStringLiteral("' lookup '") + lk.name +
                                    QStringLiteral("': match key '") + mp.second +
                                    QStringLiteral("' is empty"));
                    hasError = true;
                    break;
                }

                // §5.3 cast to G column affinity
                const ColumnInfo* gCol = gTable ? gTable->column(mp.first) : nullptr;
                QVariant casted = gCol ? castToAffinity(raw, *gCol) : QVariant(raw.toString());
                if (!casted.isValid()) {
                    errors->add(sheet, row, mp.second, raw.toString(),
                                QString::fromLatin1(err::E_LOOKUP_KEY_INVALID),
                                QStringLiteral("route '") + route.table +
                                    QStringLiteral("' lookup '") + lk.name +
                                    QStringLiteral("': match key '") + mp.second +
                                    QStringLiteral("' type cast failed (expected ") +
                                    (gCol ? gCol->declaredType : QStringLiteral("?")) +
                                    QStringLiteral(")"));
                    hasError = true;
                    break;
                }
                matchVals.append(casted);
            }

            if (hasError) {
                routeFailed = true;
                continue;
            }

            // §5.4 look up in prefetch cache
            QString tkey = makeTupleKey(matchVals);
            const auto& idCache = cache.value(identityKey);
            auto it = idCache.find(tkey);

            if (it == idCache.end()) {
                QStringList keyParts;
                for (int ki = 0; ki < lk.match.size() && ki < matchVals.size(); ++ki)
                    keyParts.append(lk.match[ki].second + QLatin1Char('=') +
                                    matchVals[ki].toString());
                errors->add(sheet, row, matchHeaders.join(QLatin1Char(',')), tkey,
                            QString::fromLatin1(err::E_LOOKUP_NOT_FOUND),
                            QStringLiteral("route '") + route.table + QStringLiteral("' lookup '") +
                                lk.name + QStringLiteral("': no match for (") +
                                keyParts.join(QStringLiteral(", ")) + QStringLiteral(") in ") +
                                lk.fromTable);
                routeFailed = true;
                continue;
            }

            const LookupHit& hit = it.value();

            // §5.5 ambiguous
            if (hit.hitCount > 1) {
                errors->add(sheet, row, matchHeaders.join(QLatin1Char(',')), tkey,
                            QString::fromLatin1(err::E_LOOKUP_AMBIGUOUS),
                            QStringLiteral("route '") + route.table + QStringLiteral("' lookup '") +
                                lk.name + QStringLiteral("': found ") +
                                QString::number(hit.hitCount) + QStringLiteral(" rows in ") +
                                lk.fromTable + QStringLiteral("; consider deduplicating ") +
                                lk.fromTable + QStringLiteral(" on match columns"));
                routeFailed = true;
                continue;
            }

            // §5.6 append select results (NULL transparent)
            for (int si = 0; si < lk.select.size(); ++si) {
                const QString& targetCol = lk.select[si].second;
                const QVariant& val = hit.values[si];

                int existingIdx = payload.indexOf(targetCol);
                if (existingIdx >= 0) {
                    payload.binds[existingIdx] = val;
                } else {
                    payload.dbColumns.append(targetCol);
                    payload.binds.append(val);
                }

                // Update conflictVals if this target is a conflict column
                for (int ci = 0; ci < payload.conflictKey.size(); ++ci) {
                    if (payload.conflictKey[ci] == targetCol && ci < payload.conflictVals.size()) {
                        payload.conflictVals[ci] = val;
                    }
                }
            }
        }

        // §D11: any lookup failure on this route → seed cascade suppression
        if (routeFailed)
            failedRoutes.insert(i);
    }
    return failedRoutes;
}

// ---- Lookup prefetch (Phase A.5) ------------------------------------------

// §4.11 Prefetch query counter hook — called once per actual SELECT executed.
// Nullptr → noop (production). Inject a counting lambda in tests.
static bool buildLookupCache(const ProfileSpec& profile, const SchemaCatalog& catalog,
                             const ExcelReader& reader, QSqlDatabase& db, const QString& sheetName,
                             ErrorCollector* errors, LookupCache* cache,
                             const std::function<void(const QString&)>& onPrefetch = nullptr) {
    // Collect all unique lookup identities across all routes (and all classes for Mixed mode)
    QHash<QString, LookupSpec> identitySpecs;  // identityKey -> representative LookupSpec

    auto collectLookups = [&](const QVector<RouteSpec>& routes) {
        for (const RouteSpec& route : routes) {
            for (const LookupSpec& lk : route.lookups) {
                QString ikey = buildIdentityKey(lk);
                if (!identitySpecs.contains(ikey))
                    identitySpecs[ikey] = lk;
            }
        }
    };

    if (profile.mode == ProfileMode::Mixed) {
        for (const auto& cls : profile.classes)
            collectLookups(cls.routes);
    } else {
        collectLookups(profile.routes);
    }

    // For each identity: pre-scan Excel, batch SELECT, populate cache
    for (auto it = identitySpecs.begin(); it != identitySpecs.end(); ++it) {
        const QString& ikey = it.key();
        const LookupSpec& lk = it.value();

        const TableInfo* gTable = catalog.table(lk.fromTable);

        // Pre-scan: collect distinct match key tuples (skip empty/invalid at row-time)
        QHash<QString, QVector<QVariant>> keyMap;  // tupleKey -> casted values

        for (int r = reader.firstDataRow(); r <= reader.lastRow(); ++r) {
            QVector<QVariant> matchVals;
            bool skip = false;
            for (const auto& mp : lk.match) {
                QVariant raw = reader.cellBySource(r, mp.second);
                if (raw.isNull() || raw.toString().trimmed().isEmpty()) {
                    skip = true;
                    break;
                }
                const ColumnInfo* gCol = gTable ? gTable->column(mp.first) : nullptr;
                QVariant casted = gCol ? castToAffinity(raw, *gCol) : QVariant(raw.toString());
                if (!casted.isValid()) {
                    skip = true;
                    break;
                }
                matchVals.append(casted);
            }
            if (skip)
                continue;

            QString tkey = makeTupleKey(matchVals);
            if (!keyMap.contains(tkey))
                keyMap[tkey] = matchVals;
        }

        // §4.5 K == 0: skip SELECT entirely
        if (keyMap.isEmpty()) {
            (*cache)[ikey] = {};
            continue;
        }

        // Build batch SELECT: SELECT <match cols>, <select cols> FROM G WHERE ...
        QStringList selectColNames;
        for (const auto& mp : lk.match)
            selectColNames.append(mp.first);
        for (const auto& sp : lk.select)
            selectColNames.append(sp.first);

        int numMatchCols = lk.match.size();
        int numSelectCols = lk.select.size();

        // §4.8 chunk by SQLITE_MAX_VARIABLE_NUMBER = 999
        const int maxVars = 999;
        int chunkSize = qMax(1, maxVars / numMatchCols);

        QHash<QString, LookupHit> idCache;
        QVector<QString> keyList = keyMap.keys().toVector();

        for (int start = 0; start < keyList.size(); start += chunkSize) {
            int end = qMin(start + chunkSize, keyList.size());
            int batchSize = end - start;

            // §4.6/4.7 Single column: use IN; multi-column: OR-join AND-clauses
            // H-04 fix: quote all identifiers using SqlBuilder::quoteIdent.
            QStringList quotedSelectCols;
            for (const auto& c : selectColNames)
                quotedSelectCols.append(SqlBuilder::quoteIdent(c));
            QString sql = QStringLiteral("SELECT ") + quotedSelectCols.join(QStringLiteral(", ")) +
                          QStringLiteral(" FROM ") + SqlBuilder::quoteIdent(lk.fromTable) +
                          QStringLiteral(" WHERE ");

            if (numMatchCols == 1) {
                QStringList placeholders;
                for (int i = 0; i < batchSize; ++i)
                    placeholders.append(QStringLiteral("?"));
                sql += SqlBuilder::quoteIdent(lk.match[0].first) + QStringLiteral(" IN (") +
                       placeholders.join(QStringLiteral(", ")) + QStringLiteral(")");
            } else {
                QStringList orClauses;
                for (int i = 0; i < batchSize; ++i) {
                    QStringList andClauses;
                    for (const auto& mp : lk.match)
                        andClauses.append(SqlBuilder::quoteIdent(mp.first) +
                                          QStringLiteral(" = ?"));
                    orClauses.append(QStringLiteral("(") +
                                     andClauses.join(QStringLiteral(" AND ")) +
                                     QStringLiteral(")"));
                }
                sql += orClauses.join(QStringLiteral(" OR "));
            }

            if (onPrefetch)
                onPrefetch(ikey);

            QSqlQuery q(db);
            q.prepare(sql);
            for (int i = start; i < end; ++i) {
                const QVector<QVariant>& vals = keyMap[keyList[i]];
                for (const auto& v : vals)
                    q.addBindValue(v);
            }

            if (!q.exec()) {
                errors->addTable(sheetName, QString::fromLatin1(err::E_LOOKUP_QUERY_FAILED),
                                 QStringLiteral("Lookup prefetch failed for '") + lk.fromTable +
                                     QStringLiteral("': ") + q.lastError().text());
                return false;  // §4.10 fatal
            }

            while (q.next()) {
                // Re-build match key from result row
                QVector<QVariant> resultMatchVals;
                for (int i = 0; i < numMatchCols; ++i)
                    resultMatchVals.append(q.value(i));
                QString tkey = makeTupleKey(resultMatchVals);

                // Select values follow match columns in the result
                QVector<QVariant> selectVals;
                for (int i = numMatchCols; i < numMatchCols + numSelectCols; ++i)
                    selectVals.append(q.value(i));

                auto& hit = idCache[tkey];
                if (hit.hitCount == 0)
                    hit.values = selectVals;
                hit.hitCount++;
            }
        }

        (*cache)[ikey] = idCache;
    }

    return true;
}

// ---- routeHasChildren ------------------------------------------------------

static bool routeHasChildren(const QString& table, const QVector<RouteSpec>& routes) {
    for (const auto& r : routes) {
        if (r.parent == table)
            return true;
    }
    return false;
}

// ---- ImportService::run ----------------------------------------------------

ImportResult ImportService::run(const ProfileSpec& profile, const SchemaCatalog& catalog,
                                const QString& xlsxPath, const ImportOptions& options,
                                QSqlDatabase& db, bool manageTransaction) {
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

    if (profile.mode == ProfileMode::Mixed) {
        QString routerErr;
        if (!router.init(profile, &routerErr)) {
            errors.addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE), routerErr);
            result.errors = errors.list();
            return result;
        }
    }

    ValidatorMap validatorMap;
    if (profile.mode == ProfileMode::Mixed) {
        for (const auto& cls : profile.classes) {
            QString vErr;
            if (!mapper.compileValidators(cls.routes, cls.id, profile, &validatorMap, &vErr)) {
                errors.addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE), vErr);
                result.errors = errors.list();
                return result;
            }
        }
    } else {
        QString vErr;
        if (!mapper.compileValidators(profile.routes, QString(), profile, &validatorMap, &vErr)) {
            errors.addTable(profile.sheet, QString::fromLatin1(err::E_PROFILE_PARSE), vErr);
            result.errors = errors.list();
            return result;
        }
    }

    QHash<QString, QVector<RouteSpec>> topoRoutes;
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

    // --- Phase A.5: Lookup prefetch ---
    LookupCache lookupCache;
    if (!buildLookupCache(profile, catalog, reader, db, sheetName, &errors, &lookupCache,
                          onPrefetch)) {
        result.errors = errors.list();
        return result;
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

        // Map Excel columns to payloads
        // M-06 fix: pass sheetName so Mapper error entries carry the correct sheet location.
        ctx.payloads = mapper.map(*routesPtr, r, ctx.classId, reader, validatorMap, profile,
                                  &errors, sheetName);
        // H-01 fix: validator and temporal-conversion failures are route-local (Mapper sets
        // payload.hasError for the affected route).  Seed failedRouteIndices from payload.hasError
        // so only the affected route (and its descendants) is skipped, while sibling routes whose
        // payloads are valid still proceed.  hasNonRouteError is reserved for structural errors
        // (missing columns, codec failures) that render the entire row unusable.
        for (int pi = 0; pi < ctx.payloads.size(); ++pi) {
            if (ctx.payloads[pi].hasError)
                ctx.failedRouteIndices.insert(pi);
        }

        // Apply lookups (Phase A.5 cache → row payload); failures seed cascade suppression (§D11)
        QSet<int> lookupFailed = applyLookups(ctx.payloads, *routesPtr, lookupCache, catalog,
                                              reader, r, sheetName, &errors);

        // H-01 fix: merge Mapper-level failures (ctx.failedRouteIndices) into the seed set
        // passed to FkInjector so both failure sources are preserved.  Previously the inject()
        // return value overwrote ctx.failedRouteIndices, discarding the Mapper failures.
        // H-04 fix: capture failedRouteIndices so write phase skips only affected payloads
        // (and their descendants) rather than the entire Excel row.
        QSet<int> initialFailed = ctx.failedRouteIndices;  // save Mapper failures
        initialFailed |= lookupFailed;
        FkInjector fkInjector;
        ctx.failedRouteIndices = fkInjector.inject(ctx.payloads, *routesPtr, r, sheetName, &errors,
                                                   std::move(initialFailed));

        // Batch uniqueness check — only on payloads that did not fail injection
        for (int pi = 0; pi < ctx.payloads.size(); ++pi) {
            if (ctx.failedRouteIndices.contains(pi))
                continue;
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
        // H-01 fix: build an excelRow→index map before iterating by class, so failed
        // failedRouteIndices written into the per-class copy can be merged back into the
        // original contexts vector (the write phase only sees contexts, not clsContexts).
        QHash<int, int> excelRowToIdx;
        for (int i = 0; i < contexts.size(); ++i)
            excelRowToIdx[contexts[i].excelRow] = i;

        for (const auto& cls : profile.classes) {
            QVector<RowContext> clsContexts;
            for (const auto& ctx : contexts) {
                if (ctx.classId == cls.id)
                    clsContexts.append(ctx);
            }
            ForeignKeyPreflight fkPreflight;
            fkPreflight.check(clsContexts, topoRoutes.value(cls.id), db, sheetName, &errors);

            // Merge failedRouteIndices from the temporary copy back into original contexts.
            for (const auto& clsCtx : clsContexts) {
                if (!clsCtx.failedRouteIndices.isEmpty()) {
                    auto it = excelRowToIdx.find(clsCtx.excelRow);
                    if (it != excelRowToIdx.end())
                        contexts[it.value()].failedRouteIndices |= clsCtx.failedRouteIndices;
                }
            }
        }
    }

    // §8.4 dryRun: skip write, populate dryRunPayloads
    if (options.dryRun) {
        result.dryRunPayloads = contexts;
        result.errors = errors.list();
        return result;
    }

    // C-2 fix: implement ImportOptions::abortOnError.
    // - Table-level errors (row == 0) always abort (no partial write makes sense).
    // - Row-level errors (row > 0): abort when abortOnError=true (MVP all-or-nothing default);
    //   skip failing row and continue when abortOnError=false (time-format OpenSpec mode).
    {
        bool hasTableErrors = false;
        bool hasRowErrors = false;
        for (const auto& e : errors.list()) {
            if (e.row == 0)
                hasTableErrors = true;
            else
                hasRowErrors = true;
        }
        if (hasTableErrors || (options.abortOnError && hasRowErrors)) {
            result.errors = errors.list();
            return result;
        }
    }

    // --- Phase D: Write (single transaction) ---
    if (manageTransaction && !db.transaction()) {
        errors.addTable(sheetName, QString::fromLatin1(err::E_DB_UPSERT),
                        QStringLiteral("Failed to start transaction: ") + db.lastError().text());
        result.errors = errors.list();
        return result;
    }

    // Collect rows that had mapping / validation / preflight errors; they are skipped.
    QSet<int> failedExcelRows;
    for (const auto& e : errors.list())
        if (e.row > 0)
            failedExcelRows.insert(e.row);

    SqlBuilder sqlBuilder;
    QHash<QString, QSqlQuery> preparedQueries;

    // H-04 fix: compute descendant closure so that failing an FK-inject on a parent also skips
    // its children in the write phase.
    auto buildDescendantFailSet = [](const QVector<RoutePayload>& payloads,
                                     const QVector<RouteSpec>& routes,
                                     const QSet<int>& directFailed) -> QSet<int> {
        if (directFailed.isEmpty())
            return {};
        QHash<QString, int> tableToIdx;
        for (int i = 0; i < payloads.size(); ++i)
            tableToIdx[payloads[i].table] = i;
        QSet<int> failed = directFailed;
        bool changed = true;
        while (changed) {
            changed = false;
            for (int i = 0; i < routes.size(); ++i) {
                if (failed.contains(i))
                    continue;
                if (!routes[i].parent.isEmpty()) {
                    const int parentIdx = tableToIdx.value(routes[i].parent, -1);
                    if (parentIdx >= 0 && failed.contains(parentIdx)) {
                        failed.insert(i);
                        changed = true;
                    }
                }
            }
        }
        return failed;
    };

    bool writeOk = true;
    for (const auto& ctx : contexts) {
        // H-02 / M-04 fix: skip the entire Excel row only when the row has a non-route-local error
        // (structural/type/mapping failure that renders payload data unusable).  If the row only
        // has route-local errors (tracked in failedRouteIndices), proceed to route-level filtering
        // below so that sibling routes whose payloads are valid can still be written.
        // M-04: hasNonRouteError covers the case where both kinds of errors coexist — the
        // presence of non-route errors means the whole row must be skipped.
        if (failedExcelRows.contains(ctx.excelRow) &&
            (ctx.hasNonRouteError || ctx.failedRouteIndices.isEmpty()))
            continue;
        // Rows with only failedRouteIndices fall through; skipPayloadIndices below handles them.

        // H-04 fix: determine the routes for this context.
        const QVector<RouteSpec>* routesForCtx = nullptr;
        if (profile.mode == ProfileMode::Mixed) {
            auto it = topoRoutes.find(ctx.classId);
            if (it != topoRoutes.end())
                routesForCtx = &it.value();
        } else {
            auto it = topoRoutes.find(QString());
            if (it != topoRoutes.end())
                routesForCtx = &it.value();
        }

        QSet<int> skipPayloadIndices;
        if (routesForCtx && !ctx.failedRouteIndices.isEmpty())
            skipPayloadIndices =
                buildDescendantFailSet(ctx.payloads, *routesForCtx, ctx.failedRouteIndices);

        // H-01 fix: track whether any payload was actually upserted for this Excel row so that
        // writtenRows is only incremented when at least one route's data reaches the DB.  Rows
        // whose all payloads are suppressed by skipPayloadIndices (route-local failures) must not
        // count as written.
        bool anyUpserted = false;

        for (int pi = 0; pi < ctx.payloads.size(); ++pi) {
            if (skipPayloadIndices.contains(pi))
                continue;
            const auto& payload = ctx.payloads[pi];
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

            for (const auto& v : payload.binds)
                qptr->addBindValue(v);

            if (!qptr->exec()) {
                errors.add(sheetName, ctx.excelRow, QString(), QString(),
                           QString::fromLatin1(err::E_DB_UPSERT),
                           QStringLiteral("Upsert failed: ") + qptr->lastError().text());
                writeOk = false;
                break;
            }
            anyUpserted = true;
        }
        if (!writeOk)
            break;
        // M-05 fix: writtenRows counts Excel rows successfully written (i.e. rows where at least
        // one payload upsert succeeded). The previous semantics were identical — but the field name
        // previously suggested "number of DB rows inserted/updated", which is ctx.payloads.size()
        // times per Excel row (one upsert per route). The current semantics are Excel rows; callers
        // that need exact DB upsert counts should sum payloads.size() for non-empty payloads.
        // H-01 fix: only count the row when at least one payload was actually written.
        if (anyUpserted)
            result.writtenRows++;
    }

    if (writeOk) {
        if (manageTransaction && !db.commit()) {
            if (manageTransaction)
                db.rollback();
            errors.addTable(sheetName, QString::fromLatin1(err::E_DB_UPSERT),
                            QStringLiteral("Commit failed: ") + db.lastError().text());
            result.writtenRows = 0;
        } else {
            result.ok = true;
        }
    } else {
        if (manageTransaction)
            db.rollback();
        result.writtenRows = 0;
    }

    result.errors = errors.list();
    // Merge runtime warnings with profile load-time diagnostics
    result.warnings = errors.warnings();
    for (const QString& w : profile.loadWarnings) {
        RowError re;
        re.sheet = profile.sheet;
        re.code = QStringLiteral("W_PROFILE_LOAD");
        re.message = w;
        result.warnings.append(re);
    }
    return result;
}

}  // namespace dbridge::detail
