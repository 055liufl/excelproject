#include "ExportService.h"

#include "dbridge/Errors.h"

#include <QHash>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

#include "ErrorCollector.h"
#include "ExportHelpers.h"
#include "excel/ExcelWriter.h"
#include "mapping/TemporalConvert.h"
#include "mapping/TopoSorter.h"
#include "schema/SchemaCatalog.h"
#include "sql/SqlBuilder.h"
#include <algorithm>
#include <functional>

namespace dbridge::detail {

// Per-column temporal export configuration (export direction: DB → Excel).
struct TemporalColumnInfo {
    TemporalSlotKind kind;
    TemporalSideSpec db;     // V — parse the DB value
    TemporalSideSpec excel;  // U — serialize to Excel
};

// Build a flat col.source → TemporalColumnInfo map from all routes/classes in the profile.
// Keyed by col.source because SqlBuilder generates "t.dbCol AS source", so rec.fieldName(i)
// returns the source alias. For mixed mode, first-occurrence wins.
static QHash<QString, TemporalColumnInfo> buildTemporalExportMap(const ProfileSpec& profile) {
    QHash<QString, TemporalColumnInfo> map;

    auto processRoutes = [&](const QVector<RouteSpec>& routes) {
        for (const auto& route : routes) {
            for (const auto& col : route.columns) {
                if (map.contains(col.source))
                    continue;
                TemporalSlotKind kind = temporalSlotKindFor(col, profile);
                if (kind == TemporalSlotKind::None)
                    continue;
                TemporalFormatSpec eff = effectiveTemporalFor(kind, col, profile);
                if (!eff.declared)
                    continue;  // legacy date:fmt-only columns — no format layer on export
                map[col.source] = {kind, eff.db, eff.excel};
            }
        }
    };

    if (profile.mode == ProfileMode::Mixed) {
        for (const auto& cls : profile.classes)
            processRoutes(cls.routes);
    } else {
        processRoutes(profile.routes);
    }
    return map;
}

// Convert a DB cell value to its Excel display value using temporal V→U formatting.
// On parse failure, records E_TIME_PARSE_DB (non-blocking) and returns null QVariant.
static QVariant convertTemporalForExport(const QVariant& dbVal, const TemporalColumnInfo& info,
                                         const QString& sheet, const QString& dbColumn,
                                         ErrorCollector* errors) {
    // Explicit SQL NULL → empty cell, no error (spec: only isNull() is silent).
    if (dbVal.isNull())
        return QVariant();
    // H-10 fix: a non-NULL empty/blank string is NOT silently skipped — it must go through
    // the DB-side parser and produce E_TIME_PARSE_DB on failure, per time-format spec.

    QVariant structured;
    if (tconv::isStructuredTemporal(dbVal, info.kind)) {
        structured = dbVal;
    } else {
        QString errCode, errMsg;
        // For epochSec: use toStructured which handles qlonglong → QDateTime
        // For string: toStructured calls parseString with db.format
        structured = tconv::toStructured(dbVal, info.kind, info.db, &errCode, &errMsg);
        if (!structured.isValid()) {
            // Map import-side error code to DB-side
            errors->add(sheet, 0, dbColumn, dbVal.toString(),
                        QString::fromLatin1(err::E_TIME_PARSE_DB),
                        QStringLiteral("Cannot parse DB value '") + dbVal.toString() +
                            QStringLiteral("' (") + errMsg + QStringLiteral(")"));
            return QVariant();
        }
    }

    QVariant result = tconv::formatValue(structured, info.kind, info.excel);
    return (result.isValid() && !result.isNull()) ? result : QVariant();
}

// ---- Reverse-lookup helpers (add-export-reverse-lookup) ----------------------

namespace {

// Stable identity key: (from, match-pairs, select-pairs) — same format as import side.
QString buildIdentityKey(const LookupSpec& lk) {
    QStringList matchParts, selectParts;
    for (const auto& p : lk.match)
        matchParts.append(p.first + QLatin1Char('=') + p.second);
    for (const auto& p : lk.select)
        selectParts.append(p.first + QStringLiteral("->") + p.second);
    return lk.fromTable + QStringLiteral("::") + matchParts.join(QLatin1Char(',')) +
           QStringLiteral("::") + selectParts.join(QLatin1Char(','));
}

// Serialize a value tuple to a stable QHash key (unit-separator as field delimiter).
QString makeTupleKey(const QVector<QVariant>& values) {
    QStringList parts;
    for (const auto& v : values)
        parts.append(v.isNull() ? QString() : v.toString());
    return parts.join(QLatin1Char('\x1F'));
}

// Cast a QVariant to the declared type affinity of a G-table column.
QVariant castToAffinity(const QVariant& raw, const ColumnInfo& gCol) {
    QString type = gCol.declaredType.trimmed().toUpper();
    if (type.contains(QStringLiteral("INT"))) {
        bool ok;
        qlonglong iv = raw.toLongLong(&ok);
        return ok ? QVariant(iv) : QVariant();
    }
    if (type.contains(QStringLiteral("REAL")) || type.contains(QStringLiteral("FLOA")) ||
        type.contains(QStringLiteral("DOUB"))) {
        bool ok;
        double dv = raw.toDouble(&ok);
        return ok ? QVariant(dv) : QVariant();
    }
    return QVariant(raw.toString());
}

// Per-hit storage: A-column values from G (match[].G_column order) + cardinality count.
struct ReverseHit {
    QVector<QVariant> matchVals;
    int hitCount = 0;
};

// identityKey → (H-tuple-key → ReverseHit)
using ReverseCache = QHash<QString, QHash<QString, ReverseHit>>;

// Returns true if any lookup in the routes has exportRoundtrip=true.
bool hasActiveReverseLookup(const QVector<RouteSpec>& routes) {
    for (const auto& route : routes)
        for (const auto& lk : route.lookups)
            if (lk.exportRoundtrip)
                return true;
    return false;
}

// Returns true if any lookup in the routes has select columns (roundtrip=true or false).
// Used to decide whether to extend the SELECT query with H-cols even when no active roundtrip.
bool hasAnyLookupHCols(const QVector<RouteSpec>& routes) {
    for (const auto& route : routes)
        for (const auto& lk : route.lookups)
            if (!lk.select.isEmpty())
                return true;
    return false;
}

// Build the extra SQL column selections for H-cols (all lookups, roundtrip true and false).
// Returns e.g. ", "t"."h1", "t"."h2"" to be appended before " FROM " in the base SQL.
QString buildHColSelectSuffix(const QVector<RouteSpec>& routes) {
    QStringList parts;
    QSet<QString> seen;
    for (const auto& route : routes) {
        for (const auto& lk : route.lookups) {
            for (const auto& sp : lk.select) {
                // H-10 fix: quote both table and column identifiers.
                QString qualified = SqlBuilder::quoteIdent(route.table) + QLatin1Char('.') +
                                    SqlBuilder::quoteIdent(sp.second);
                if (!seen.contains(qualified)) {
                    seen.insert(qualified);
                    parts.append(qualified);
                }
            }
        }
    }
    return parts.isEmpty() ? QString() : QStringLiteral(", ") + parts.join(QStringLiteral(", "));
}

// Extend a SqlBuilder-generated SELECT SQL to also retrieve H-col values.
QString extendSqlWithHCols(const QString& baseSql, const QString& hColSuffix) {
    if (hColSuffix.isEmpty())
        return baseSql;
    int fromPos = baseSql.indexOf(QStringLiteral(" FROM "));
    if (fromPos < 0)
        return baseSql;
    return baseSql.left(fromPos) + hColSuffix + baseSql.mid(fromPos);
}

// Collect all H-dbColumn names for roundtrip=true lookups in the given routes.
QSet<QString> buildHColReplaceSet(const QVector<RouteSpec>& routes) {
    QSet<QString> hcols;
    for (const auto& route : routes)
        for (const auto& lk : route.lookups)
            if (lk.exportRoundtrip)
                for (const auto& sp : lk.select)
                    hcols.insert(sp.second);
    return hcols;
}

// Collect A-headers (match[].Excel_header) for roundtrip=true lookups in the given routes.
// Preserves first-occurrence order.
QStringList buildAHeaders(const QVector<RouteSpec>& routes) {
    QStringList aHeaders;
    QSet<QString> seen;
    for (const auto& route : routes) {
        for (const auto& lk : route.lookups) {
            if (!lk.exportRoundtrip)
                continue;
            for (const auto& mp : lk.match) {
                if (!seen.contains(mp.second)) {
                    seen.insert(mp.second);
                    aHeaders.append(mp.second);
                }
            }
        }
    }
    return aHeaders;
}

// Run batch reverse-lookup prefetch: for each identity, query G using the H-values collected
// from the main SELECT result. Builds ReverseCache. Returns false (table-level abort) on error.
bool buildReverseCache(const QVector<RouteSpec>& routes, const SchemaCatalog& catalog,
                       QSqlDatabase& db, const QString& sheet,
                       const QHash<QString, QHash<QString, QVector<QVariant>>>& hValueSets,
                       ReverseCache* cache, ErrorCollector* errors,
                       const std::function<void(const QString&)>& onPrefetch = nullptr) {
    // Collect unique identities (roundtrip=true only)
    QHash<QString, LookupSpec> identitySpecs;
    for (const auto& route : routes) {
        for (const LookupSpec& lk : route.lookups) {
            if (!lk.exportRoundtrip)
                continue;
            QString ikey = buildIdentityKey(lk);
            if (!identitySpecs.contains(ikey))
                identitySpecs[ikey] = lk;
        }
    }

    for (auto it = identitySpecs.begin(); it != identitySpecs.end(); ++it) {
        const QString& ikey = it.key();
        const LookupSpec& lk = it.value();
        const TableInfo* gTable = catalog.table(lk.fromTable);

        // H-values for this identity collected from the main SELECT
        const QHash<QString, QVector<QVariant>>& keyMap = hValueSets.value(ikey);

        if (keyMap.isEmpty()) {
            (*cache)[ikey] = {};
            continue;
        }

        // Build SELECT: SELECT <match cols>, <select cols> FROM G WHERE <select cols IN ...>
        QStringList selectColNames;
        for (const auto& mp : lk.match)
            selectColNames.append(mp.first);
        for (const auto& sp : lk.select)
            selectColNames.append(sp.first);

        int numMatchCols = lk.match.size();
        int numSelectCols = lk.select.size();

        const int maxVars = 999;
        int chunkSize = qMax(1, maxVars / numSelectCols);

        QHash<QString, ReverseHit> idCache;
        QVector<QString> keyList = keyMap.keys().toVector();

        for (int start = 0; start < keyList.size(); start += chunkSize) {
            int end = qMin(start + chunkSize, keyList.size());
            int batchSize = end - start;

            // H-10 fix: quote all identifiers in reverse lookup prefetch SQL.
            QStringList quotedSelectCols;
            for (const auto& colName : selectColNames)
                quotedSelectCols.append(SqlBuilder::quoteIdent(colName));
            QString sql = QStringLiteral("SELECT ") + quotedSelectCols.join(QStringLiteral(", ")) +
                          QStringLiteral(" FROM ") + SqlBuilder::quoteIdent(lk.fromTable) +
                          QStringLiteral(" WHERE ");

            if (numSelectCols == 1) {
                QStringList placeholders;
                for (int i = 0; i < batchSize; ++i)
                    placeholders.append(QStringLiteral("?"));
                sql += SqlBuilder::quoteIdent(lk.select[0].first) + QStringLiteral(" IN (") +
                       placeholders.join(QStringLiteral(", ")) + QStringLiteral(")");
            } else {
                QStringList orClauses;
                for (int i = 0; i < batchSize; ++i) {
                    QStringList andClauses;
                    for (const auto& sp : lk.select)
                        andClauses.append(SqlBuilder::quoteIdent(sp.first) +
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
                errors->addTable(sheet, QString::fromLatin1(err::E_REVERSE_LOOKUP_QUERY_FAILED),
                                 QStringLiteral("Reverse-lookup prefetch failed for '") +
                                     lk.fromTable + QStringLiteral("': ") + q.lastError().text());
                return false;
            }

            while (q.next()) {
                // First numMatchCols columns = match (A-values); next numSelectCols = select
                // (H-values)
                QVector<QVariant> matchVals;
                for (int i = 0; i < numMatchCols; ++i)
                    matchVals.append(q.value(i));
                QVector<QVariant> selectVals;
                for (int i = numMatchCols; i < numMatchCols + numSelectCols; ++i)
                    selectVals.append(q.value(i));

                // Apply affinity casting for H-values (used as the cache key)
                QVector<QVariant> castedHVals;
                bool castOk = true;
                for (int i = 0; i < numSelectCols; ++i) {
                    const ColumnInfo* gCol = gTable ? gTable->column(lk.select[i].first) : nullptr;
                    QVariant c = gCol ? castToAffinity(selectVals[i], *gCol)
                                      : QVariant(selectVals[i].toString());
                    if (!c.isValid()) {
                        castOk = false;
                        break;
                    }
                    castedHVals.append(c);
                }
                if (!castOk)
                    continue;

                QString tkey = makeTupleKey(castedHVals);
                auto& hit = idCache[tkey];
                if (hit.hitCount == 0)
                    hit.matchVals = matchVals;
                hit.hitCount++;
            }
        }

        (*cache)[ikey] = idCache;
    }

    return true;
}

// Collect H-value tuples from a loaded row data map, for all roundtrip=true lookups in routes.
// hValueSets: identityKey → (tupleKey → casted H-values)
void collectHValues(const QVector<RouteSpec>& routes, const SchemaCatalog& catalog,
                    const QHash<QString, QVariant>& rowData,
                    QHash<QString, QHash<QString, QVector<QVariant>>>* hValueSets) {
    for (const auto& route : routes) {
        for (const LookupSpec& lk : route.lookups) {
            if (!lk.exportRoundtrip)
                continue;
            QString ikey = buildIdentityKey(lk);
            const TableInfo* gTable = catalog.table(lk.fromTable);

            QVector<QVariant> hVals;
            bool hasNull = false;
            for (const auto& sp : lk.select) {
                QVariant dbVal = rowData.value(sp.second);
                if (dbVal.isNull() || dbVal.toString().isEmpty()) {
                    hasNull = true;
                    break;
                }
                const ColumnInfo* gCol = gTable ? gTable->column(sp.first) : nullptr;
                QVariant casted = gCol ? castToAffinity(dbVal, *gCol) : QVariant(dbVal.toString());
                if (!casted.isValid()) {
                    hasNull = true;
                    break;
                }
                hVals.append(casted);
            }
            if (hasNull)
                continue;

            QString tkey = makeTupleKey(hVals);
            auto& idMap = (*hValueSets)[ikey];
            if (!idMap.contains(tkey))
                idMap[tkey] = hVals;
        }
    }
}

// Resolve A-header values for a single row using the ReverseCache.
// Returns QHash<A-header → resolved value>. On error or miss, respects exportOnMissing.
// Returns false (skip row entirely) only when ambiguous.
// On NOT_FOUND + "error": emits error, sets rowSkip=true.
QHash<QString, QVariant> resolveAHeaders(const QVector<RouteSpec>& routes,
                                         const SchemaCatalog& catalog,
                                         const QHash<QString, QVariant>& rowData,
                                         const ReverseCache& cache, const QString& sheet,
                                         int rowIndex, const QString& classId,
                                         ErrorCollector* errors, bool* rowSkip) {
    QHash<QString, QVariant> aVals;
    *rowSkip = false;

    for (const auto& route : routes) {
        for (const LookupSpec& lk : route.lookups) {
            if (!lk.exportRoundtrip)
                continue;

            QString ikey = buildIdentityKey(lk);
            const TableInfo* gTable = catalog.table(lk.fromTable);

            // Collect H-tuple from this row
            QVector<QVariant> hVals;
            bool hasNull = false;
            for (const auto& sp : lk.select) {
                QVariant dbVal = rowData.value(sp.second);
                if (dbVal.isNull() || dbVal.toString().isEmpty()) {
                    hasNull = true;
                    break;
                }
                const ColumnInfo* gCol = gTable ? gTable->column(sp.first) : nullptr;
                QVariant casted = gCol ? castToAffinity(dbVal, *gCol) : QVariant(dbVal.toString());
                if (!casted.isValid()) {
                    hasNull = true;
                    break;
                }
                hVals.append(casted);
            }

            // NULL H-value: treat as miss (exportOnMissing applies)
            if (hasNull) {
                if (lk.exportOnMissing == QLatin1String(ExportOnMissing::kError)) {
                    QString ctx = classId.isEmpty()
                                      ? QString()
                                      : QStringLiteral("class '") + classId + QStringLiteral("' ");
                    errors->add(sheet, rowIndex, lk.name, QString(),
                                QString::fromLatin1(err::E_REVERSE_LOOKUP_NOT_FOUND),
                                ctx + QStringLiteral("route '") + route.table +
                                    QStringLiteral("' lookup '") + lk.name +
                                    QStringLiteral("': H column is NULL — treating as no match"));
                    *rowSkip = true;
                }
                // null/skip: leave A-headers absent (will write NULL)
                continue;
            }

            QString tkey = makeTupleKey(hVals);
            const auto& idCache = cache.value(ikey);
            auto it = idCache.find(tkey);

            if (it == idCache.end()) {
                // NOT_FOUND
                if (lk.exportOnMissing == QLatin1String(ExportOnMissing::kError)) {
                    QString ctx = classId.isEmpty()
                                      ? QString()
                                      : QStringLiteral("class '") + classId + QStringLiteral("' ");
                    QStringList hDesc;
                    for (int i = 0; i < lk.select.size() && i < hVals.size(); ++i)
                        hDesc.append(lk.select[i].second + QLatin1Char('=') + hVals[i].toString());
                    errors->add(sheet, rowIndex, lk.name, tkey,
                                QString::fromLatin1(err::E_REVERSE_LOOKUP_NOT_FOUND),
                                ctx + QStringLiteral("route '") + route.table +
                                    QStringLiteral("' lookup '") + lk.name +
                                    QStringLiteral("': no match in '") + lk.fromTable +
                                    QStringLiteral("' for (") + hDesc.join(QStringLiteral(", ")) +
                                    QStringLiteral(")"));
                    *rowSkip = true;
                }
                // null/skip: leave A-headers absent (will write NULL), no error
                continue;
            }

            const ReverseHit& hit = it.value();

            if (hit.hitCount > 1) {
                // AMBIGUOUS — always an error regardless of exportOnMissing
                QString ctx = classId.isEmpty()
                                  ? QString()
                                  : QStringLiteral("class '") + classId + QStringLiteral("' ");
                errors->add(sheet, rowIndex, lk.name, tkey,
                            QString::fromLatin1(err::E_REVERSE_LOOKUP_AMBIGUOUS),
                            ctx + QStringLiteral("route '") + route.table +
                                QStringLiteral("' lookup '") + lk.name + QStringLiteral("': ") +
                                QString::number(hit.hitCount) + QStringLiteral(" rows in '") +
                                lk.fromTable + QStringLiteral("' share the same H-value tuple (") +
                                tkey.replace(QLatin1Char('\x1F'), QLatin1Char(',')) +
                                QStringLiteral("); deduplicate '") + lk.fromTable +
                                QStringLiteral("'"));
                *rowSkip = true;
                continue;
            }

            // Success: store A-values
            for (int i = 0; i < lk.match.size() && i < hit.matchVals.size(); ++i)
                aVals[lk.match[i].second] = hit.matchVals[i];
        }
    }

    return aVals;
}

}  // anonymous namespace

// Streaming export (no columnOrder): write header + rows as the query iterates.
static bool execAndWrite(const QString& sql, const QString& sheet, QSqlDatabase& db,
                         ExcelWriter& writer, bool writeHeader, QStringList* outHeaders,
                         const QHash<QString, TemporalColumnInfo>& temporal, ErrorCollector* errors,
                         int* rowCount) {
    QSqlQuery q(db);
    if (!q.exec(sql)) {
        errors->addTable(sheet, QString::fromLatin1(err::E_EXPORT_QUERY),
                         QStringLiteral("Query failed: ") + q.lastError().text() +
                             QStringLiteral(" SQL: ") + sql);
        return false;
    }

    QSqlRecord rec = q.record();
    QStringList headers;
    for (int i = 0; i < rec.count(); ++i)
        headers.append(rec.fieldName(i));

    if (writeHeader) {
        writer.writeHeader(headers);
        if (outHeaders)
            *outHeaders = headers;
    }

    while (q.next()) {
        QVector<QVariant> row;
        for (int i = 0; i < rec.count(); ++i) {
            QVariant val = q.value(i);
            const QString& fieldName = headers[i];
            if (temporal.contains(fieldName))
                val = convertTemporalForExport(val, temporal[fieldName], sheet, fieldName, errors);
            row.append(val);
        }
        writer.writeRow(row);
        (*rowCount)++;
    }
    return true;
}

ExportResult ExportService::run(const ProfileSpec& profile, const SchemaCatalog& catalog,
                                const QString& xlsxPath, const ExportOptions& options,
                                QSqlDatabase& db) {
    ExportResult result;
    ErrorCollector errors;

    QString sheetName = options.sheetName.isEmpty() ? profile.sheet : options.sheetName;

    ExcelWriter writer;
    QString writerErr;
    if (!writer.open(xlsxPath, sheetName, &writerErr)) {
        errors.addTable(sheetName, QString::fromLatin1(err::E_WRITE_XLSX), writerErr);
        result.errors = errors.list();
        return result;
    }

    QHash<QString, TemporalColumnInfo> temporal = buildTemporalExportMap(profile);

    SqlBuilder sqlBuilder;
    TopoSorter topoSorter;
    int rowCount = 0;

    if (profile.mode == ProfileMode::Mixed) {
        // --- Mixed export ---
        struct MixedRow {
            QString classId;
            QHash<QString, QVariant> data;
        };
        QVector<MixedRow> allRows;
        QStringList allHeaders;  // natural SQL field names in first-appearance order
        QSet<QString> headerSet;

        // Collect sorted routes per class (needed for reverse lookup too)
        QHash<QString, QVector<RouteSpec>> classSortedRoutes;
        for (const auto& cls : profile.classes) {
            QVector<RouteSpec> sorted;
            QString topoErr;
            if (!topoSorter.sort(cls.routes, &sorted, &topoErr)) {
                errors.addTable(sheetName, QString::fromLatin1(err::E_PROFILE_TOPOLOGY_CYCLE),
                                topoErr);
                result.errors = errors.list();
                return result;
            }
            classSortedRoutes[cls.id] = sorted;
        }

        // Check if any class has reverse lookup H-cols to fetch (roundtrip=true or false).
        bool needReverseLookupMixed = false;
        for (const auto& cls : profile.classes) {
            if (hasAnyLookupHCols(classSortedRoutes[cls.id])) {
                needReverseLookupMixed = true;
                break;
            }
        }

        // Load rows per class; extend SQL with H-cols if reverse lookup is active
        QHash<QString, QHash<QString, QVector<QVariant>>> hValueSets;  // for prefetch
        for (const auto& cls : profile.classes) {
            const QVector<RouteSpec>& sorted = classSortedRoutes[cls.id];
            QString sql = sqlBuilder.buildAutoJoinSelect(sorted, profile.exportSpec);
            if (sql.isEmpty())
                continue;

            // Extend SQL with H-cols if needed (6.1: sheet-level H-value collection)
            if (needReverseLookupMixed) {
                QString hSuffix = buildHColSelectSuffix(sorted);
                sql = extendSqlWithHCols(sql, hSuffix);
            }

            QSqlQuery q(db);
            if (!q.exec(sql)) {
                errors.addTable(sheetName, QString::fromLatin1(err::E_EXPORT_QUERY),
                                QStringLiteral("Query failed: ") + q.lastError().text());
                result.errors = errors.list();
                return result;
            }

            QSqlRecord rec = q.record();
            QSet<QString> clsHCols = buildHColReplaceSet(sorted);

            // Collect natural headers (exclude H-cols-to-replace from allHeaders)
            for (int i = 0; i < rec.count(); ++i) {
                QString h = rec.fieldName(i);
                if (clsHCols.contains(h))
                    continue;  // H-cols tracked separately, not in allHeaders
                if (!headerSet.contains(h)) {
                    headerSet.insert(h);
                    allHeaders.append(h);
                }
            }

            while (q.next()) {
                MixedRow row;
                row.classId = cls.id;
                for (int i = 0; i < rec.count(); ++i)
                    row.data[rec.fieldName(i)] = q.value(i);
                if (needReverseLookupMixed)
                    collectHValues(sorted, catalog, row.data, &hValueSets);
                allRows.append(row);
            }
        }

        // 6.1: sheet-level reverse prefetch (merged across classes)
        ReverseCache revCache;
        if (needReverseLookupMixed) {
            // Collect all routes from all classes for prefetch
            QVector<RouteSpec> allClassRoutes;
            for (const auto& cls : profile.classes)
                for (const auto& r : classSortedRoutes[cls.id])
                    allClassRoutes.append(r);

            if (!buildReverseCache(allClassRoutes, catalog, db, sheetName, hValueSets, &revCache,
                                   &errors)) {
                result.errors = errors.list();
                return result;
            }
        }

        // Sort allRows by orderBy keys
        if (!profile.exportSpec.orderBy.isEmpty()) {
            QStringList sortKeys;
            for (const QString& ob : profile.exportSpec.orderBy)
                sortKeys.append(ob.contains('.') ? ob.section('.', -1) : ob);
            std::stable_sort(allRows.begin(), allRows.end(),
                             [&sortKeys](const MixedRow& a, const MixedRow& b) {
                                 for (const QString& k : sortKeys) {
                                     QString va = a.data.value(k).toString();
                                     QString vb = b.data.value(k).toString();
                                     if (va != vb)
                                         return va < vb;
                                 }
                                 return false;
                             });
        }

        // Compute effective output headers for reverse-lookup mode
        QStringList mixedAllAHeaders;
        QSet<QString> mixedAllHColReplace;
        if (needReverseLookupMixed) {
            for (const auto& cls : profile.classes) {
                for (const QString& ah : buildAHeaders(classSortedRoutes[cls.id]))
                    if (!mixedAllAHeaders.contains(ah))
                        mixedAllAHeaders.append(ah);
                for (const QString& h : buildHColReplaceSet(classSortedRoutes[cls.id]))
                    mixedAllHColReplace.insert(h);
            }
        }

        // Apply columnOrder to allHeaders + A-headers; then handle classColumn placement.
        const QStringList& colOrder = profile.exportSpec.columnOrder;
        QString classCol = profile.exportSpec.classColumn;

        QStringList effectiveHeaders = allHeaders;
        for (const QString& ah : mixedAllAHeaders)
            if (!effectiveHeaders.contains(ah))
                effectiveHeaders.append(ah);

        // Reorder according to columnOrder
        QStringList finalHeaders = reorderHeaders(effectiveHeaders, colOrder);

        // classColumn placement: prepend if not explicitly positioned
        bool classColInOrder = !classCol.isEmpty() && colOrder.contains(classCol);
        if (!classCol.isEmpty() && !classColInOrder)
            finalHeaders.prepend(classCol);

        writer.writeHeader(finalHeaders);
        for (const auto& mr : allRows) {
            // 6.2: per-row resolution uses class-specific routes
            bool rowSkip = false;
            QHash<QString, QVariant> aVals;
            if (needReverseLookupMixed) {
                const QVector<RouteSpec>& clsRoutes = classSortedRoutes[mr.classId];
                aVals = resolveAHeaders(clsRoutes, catalog, mr.data, revCache, sheetName,
                                        rowCount + 1, mr.classId, &errors, &rowSkip);
            }
            if (rowSkip)
                continue;

            QVector<QVariant> rowVals;
            for (const auto& h : finalHeaders) {
                if (h == classCol) {
                    rowVals.append(mr.classId);
                } else if (mixedAllHColReplace.contains(h)) {
                    rowVals.append(QVariant());  // should not appear in finalHeaders
                } else if (needReverseLookupMixed && mixedAllAHeaders.contains(h)) {
                    // D5: ColumnSpec.source value wins if non-NULL
                    QVariant srcVal = mr.data.value(h);
                    QVariant val = (!srcVal.isNull()) ? srcVal : aVals.value(h, QVariant());
                    if (!val.isNull() && temporal.contains(h))
                        val = convertTemporalForExport(val, temporal[h], sheetName, h, &errors);
                    rowVals.append(val);
                } else {
                    QVariant val = mr.data.value(h, QVariant());
                    if (!val.isNull() && temporal.contains(h))
                        val = convertTemporalForExport(val, temporal[h], sheetName, h, &errors);
                    rowVals.append(val);
                }
            }
            writer.writeRow(rowVals);
            rowCount++;
        }
    } else {
        // --- SingleTable / MultiTable export ---
        QVector<RouteSpec> sorted;
        QString sql;
        if (!profile.exportSpec.explicitSql.isEmpty()) {
            sql = profile.exportSpec.explicitSql;
        } else {
            QString topoErr;
            if (!topoSorter.sort(profile.routes, &sorted, &topoErr)) {
                errors.addTable(sheetName, QString::fromLatin1(err::E_PROFILE_TOPOLOGY_CYCLE),
                                topoErr);
                result.errors = errors.list();
                return result;
            }
            sql = sqlBuilder.buildAutoJoinSelect(sorted, profile.exportSpec);
        }

        const bool needReverseLookup =
            !profile.exportSpec.explicitSql.isEmpty() ? false : hasAnyLookupHCols(sorted);

        if (profile.exportSpec.columnOrder.isEmpty() && !needReverseLookup) {
            // Streaming path: no columnOrder, no reverse lookup — zero extra memory.
            QStringList headers;
            if (!execAndWrite(sql, sheetName, db, writer, true, &headers, temporal, &errors,
                              &rowCount)) {
                result.errors = errors.list();
                return result;
            }
        } else if (!needReverseLookup) {
            // columnOrder-only path: load all rows into memory, reorder columns, then write.
            QSqlQuery q(db);
            if (!q.exec(sql)) {
                errors.addTable(sheetName, QString::fromLatin1(err::E_EXPORT_QUERY),
                                QStringLiteral("Query failed: ") + q.lastError().text() +
                                    QStringLiteral(" SQL: ") + sql);
                result.errors = errors.list();
                return result;
            }

            QSqlRecord rec = q.record();
            QStringList naturalHeaders;
            for (int i = 0; i < rec.count(); ++i)
                naturalHeaders.append(rec.fieldName(i));

            QVector<QVector<QVariant>> rows;
            while (q.next()) {
                QVector<QVariant> row;
                for (int i = 0; i < rec.count(); ++i) {
                    QVariant val = q.value(i);
                    const QString& fieldName = naturalHeaders[i];
                    if (temporal.contains(fieldName))
                        val = convertTemporalForExport(val, temporal[fieldName], sheetName,
                                                       fieldName, &errors);
                    row.append(val);
                }
                rows.append(row);
            }

            QStringList finalHeaders =
                reorderHeaders(naturalHeaders, profile.exportSpec.columnOrder);
            QHash<QString, int> naturalIdx;
            for (int i = 0; i < naturalHeaders.size(); ++i)
                naturalIdx[naturalHeaders[i]] = i;

            writer.writeHeader(finalHeaders);
            for (const auto& row : rows) {
                QVector<QVariant> reordered;
                for (const QString& h : finalHeaders) {
                    int idx = naturalIdx.value(h, -1);
                    reordered.append(idx >= 0 ? row[idx] : QVariant());
                }
                writer.writeRow(reordered);
                rowCount++;
            }
        } else {
            // Reverse-lookup path: extend SQL with H-cols, full-load, prefetch, project.
            // 5.2: Extend SQL to also retrieve H-col values from lookup.select targets.
            QString hColSuffix = buildHColSelectSuffix(sorted);
            QString extSql = extendSqlWithHCols(sql, hColSuffix);

            QSqlQuery q(db);
            if (!q.exec(extSql)) {
                errors.addTable(sheetName, QString::fromLatin1(err::E_EXPORT_QUERY),
                                QStringLiteral("Query failed: ") + q.lastError().text() +
                                    QStringLiteral(" SQL: ") + extSql);
                result.errors = errors.list();
                return result;
            }

            QSqlRecord rec = q.record();
            QStringList allSqlHeaders;
            for (int i = 0; i < rec.count(); ++i)
                allSqlHeaders.append(rec.fieldName(i));

            QSet<QString> hColReplaceSet = buildHColReplaceSet(sorted);

            // 5.3: Load all rows; collect H-value sets for reverse prefetch.
            QVector<QHash<QString, QVariant>> rowDataList;
            QHash<QString, QHash<QString, QVector<QVariant>>> hValueSets;
            while (q.next()) {
                QHash<QString, QVariant> rowData;
                for (int i = 0; i < rec.count(); ++i)
                    rowData[allSqlHeaders[i]] = q.value(i);
                collectHValues(sorted, catalog, rowData, &hValueSets);
                rowDataList.append(rowData);
            }

            // 5.4: Run reverse prefetch.
            ReverseCache revCache;
            if (!buildReverseCache(sorted, catalog, db, sheetName, hValueSets, &revCache,
                                   &errors)) {
                result.errors = errors.list();
                return result;
            }

            // Compute output headers: natural cols (excluding roundtrip=true H-cols) + A-headers.
            // Natural: SQL result headers NOT in hColReplaceSet.
            QStringList naturalHeaders;
            for (const QString& h : allSqlHeaders) {
                if (!hColReplaceSet.contains(h))
                    naturalHeaders.append(h);
            }
            QStringList aHeaders = buildAHeaders(sorted);
            QStringList effectiveHeaders = naturalHeaders;
            for (const QString& ah : aHeaders) {
                if (!effectiveHeaders.contains(ah))
                    effectiveHeaders.append(ah);
            }
            QStringList finalHeaders =
                reorderHeaders(effectiveHeaders, profile.exportSpec.columnOrder);

            writer.writeHeader(finalHeaders);

            // 5.5-5.7: Project rows.
            for (const auto& rowData : rowDataList) {
                bool rowSkip = false;
                QHash<QString, QVariant> aVals =
                    resolveAHeaders(sorted, catalog, rowData, revCache, sheetName, rowCount + 1,
                                    QString(), &errors, &rowSkip);
                if (rowSkip)
                    continue;

                QVector<QVariant> outRow;
                for (const QString& h : finalHeaders) {
                    QVariant val;
                    if (hColReplaceSet.contains(h)) {
                        // H-col-to-replace: should not appear in finalHeaders, skip
                        val = QVariant();
                    } else if (aHeaders.contains(h)) {
                        // 5.6: D5 — ColumnSpec.source value wins if present and non-NULL
                        QVariant srcVal = rowData.value(h);
                        if (!srcVal.isNull())
                            val = srcVal;
                        else
                            val = aVals.value(h, QVariant());
                    } else {
                        val = rowData.value(h, QVariant());
                    }
                    if (!val.isNull() && temporal.contains(h))
                        val = convertTemporalForExport(val, temporal[h], sheetName, h, &errors);
                    outRow.append(val);
                }
                writer.writeRow(outRow);
                rowCount++;
            }
        }
    }

    if (!writer.save(&writerErr)) {
        errors.addTable(sheetName, QString::fromLatin1(err::E_WRITE_XLSX), writerErr);
        result.errors = errors.list();
        return result;
    }

    result.ok = true;
    result.writtenRows = rowCount;
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
