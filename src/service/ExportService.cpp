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
// M-1 fix: add outputRow parameter so temporal parse errors carry the actual Excel row number
// instead of a fixed 0. Callers that know the row pass it; those that don't pass 0.
static QVariant convertTemporalForExport(const QVariant& dbVal, const TemporalColumnInfo& info,
                                         const QString& sheet, const QString& dbColumn,
                                         ErrorCollector* errors, int outputRow = 0) {
    // Explicit SQL NULL → empty cell, no error (spec: only isNull() is silent).
    if (dbVal.isNull())
        return QVariant();

    QVariant structured;
    if (tconv::isStructuredTemporal(dbVal, info.kind)) {
        structured = dbVal;
    } else {
        QString errCode, errMsg;
        structured = tconv::toStructured(dbVal, info.kind, info.db, &errCode, &errMsg);
        if (!structured.isValid()) {
            // M-1 fix: use outputRow (actual row) instead of hardcoded 0.
            errors->add(sheet, outputRow, dbColumn, dbVal.toString(),
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

// Serialize a value tuple to a stable, type-aware QHash key (unit-separator as field delimiter).
// H-6 fix: use type-tagged keys so that 1 (int), "1" (string), 1.0 (double) are distinct,
// matching SQLite strict equality semantics for lookup/reverse-lookup matching.
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

// Cast a QVariant to the declared type affinity of a G-table column.
// H-7 fix: BLOB columns keep binary payload; don't coerce to empty string.
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
    if (type.contains(QStringLiteral("BLOB"))) {
        if (raw.type() == QVariant::ByteArray)
            return raw;
        const QByteArray ba = raw.toByteArray();
        return ba.isEmpty() ? QVariant() : QVariant(ba);
    }
    return QVariant(raw.toString());  // TEXT / NONE
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
// Returns e.g. ", "t"."h1" AS "h1", "t"."h2" AS "h2"" to be appended before " FROM ".
// H-14 fix: each H-col carries an explicit `AS "dbColumn"` alias so the result-set field name
// is exactly sp.second (the dbColumn), regardless of how Qt/SQLite would otherwise name a
// quoted table.column expression. De-dup is keyed on the alias (sp.second) so the result set
// never has two columns with the same name.
QString buildHColSelectSuffix(const QVector<RouteSpec>& routes) {
    QStringList parts;
    QSet<QString> seenAlias;
    for (const auto& route : routes) {
        for (const auto& lk : route.lookups) {
            for (const auto& sp : lk.select) {
                const QString& alias = sp.second;  // dbColumn = result-set field name
                if (seenAlias.contains(alias))
                    continue;
                seenAlias.insert(alias);
                QString qualified = SqlBuilder::quoteIdent(route.table) + QLatin1Char('.') +
                                    SqlBuilder::quoteIdent(alias) + QStringLiteral(" AS ") +
                                    SqlBuilder::quoteIdent(alias);
                parts.append(qualified);
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
                // H-8 fix: only SQL NULL is a missing H-value; empty string is a valid lookup key.
                if (dbVal.isNull()) {
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
// On NOT_FOUND + exportOnMissing=="error": emits error and sets *rowSkip=true so the entire
// row is skipped (per spec §exportOnMissing:"error" → row SHALL be skipped not written to Excel).
// AMBIGUOUS also sets rowSkip=true (data is fundamentally inconsistent).
// "null"/"skip" write empty A-cells without skipping the row.
QHash<QString, QVariant> resolveAHeaders(const QVector<RouteSpec>& routes,
                                         const SchemaCatalog& catalog,
                                         const QHash<QString, QVariant>& rowData,
                                         const ReverseCache& cache, const QString& sheet,
                                         int rowIndex, const QString& classId,
                                         ErrorCollector* errors, bool* rowSkip,
                                         QSet<QString>* failedAHeaders = nullptr) {
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
                // H-8 fix: only SQL NULL is a missing H-value; empty string is a valid lookup key.
                if (dbVal.isNull()) {
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
                    // Per spec §exportOnMissing:"error": row SHALL be skipped (not written to
                    // Excel).
                    *rowSkip = true;
                    return aVals;
                }
                // null/skip: leave A-headers absent (will write NULL), no error
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
                    // Per spec §exportOnMissing:"error": row SHALL be skipped (not written to
                    // Excel).
                    *rowSkip = true;
                    return aVals;
                }
                // null/skip: leave A-headers absent (will write NULL), no error
                continue;
            }

            const ReverseHit& hit = it.value();

            if (hit.hitCount > 1) {
                // AMBIGUOUS — always a whole-row error regardless of exportOnMissing.
                // The data is fundamentally ambiguous: we cannot pick one of multiple matches,
                // so we must skip the entire row (not just the lookup columns).
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
                *rowSkip = true;  // AMBIGUOUS: whole row must be skipped
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
                val = convertTemporalForExport(val, temporal[fieldName], sheet, fieldName, errors,
                                               *rowCount + 1);  // M-1: pass output row number
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
            QSet<QString> failedAHeaders;
            QHash<QString, QVariant> aVals;
            if (needReverseLookupMixed) {
                const QVector<RouteSpec>& clsRoutes = classSortedRoutes[mr.classId];
                // H-04 fix: collect per-lookup failures in failedAHeaders instead of whole rowSkip.
                aVals =
                    resolveAHeaders(clsRoutes, catalog, mr.data, revCache, sheetName, rowCount + 1,
                                    mr.classId, &errors, &rowSkip, &failedAHeaders);
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
                    if (failedAHeaders.contains(h)) {
                        // H-04 fix: only null out the columns whose lookup failed.
                        rowVals.append(QVariant());
                    } else {
                        // D5: ColumnSpec.source value wins if non-NULL
                        QVariant srcVal = mr.data.value(h);
                        QVariant val = (!srcVal.isNull()) ? srcVal : aVals.value(h, QVariant());
                        if (!val.isNull() && temporal.contains(h))
                            val = convertTemporalForExport(val, temporal[h], sheetName, h, &errors,
                                                           rowCount + 1);
                        rowVals.append(val);
                    }
                } else {
                    QVariant val = mr.data.value(h, QVariant());
                    if (!val.isNull() && temporal.contains(h))
                        val = convertTemporalForExport(val, temporal[h], sheetName, h, &errors,
                                                       rowCount + 1);
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

        // needReverseLookup: true if any lookup has select columns (H-cols) to retrieve.
        // hasAnyLookupHCols() covers both roundtrip=true (replacement) and roundtrip=false (raw
        // H-col output). buildReverseCache() internally skips non-roundtrip lookups so the
        // expensive prefetch is a no-op when all lookups have exportRoundtrip=false.
        // M-07 note: the real performance win (skip buildReverseCache entirely for roundtrip=false)
        // could be applied but is out of scope here since buildReverseCache is already selective.
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
            int colOrderLoadRow = 0;
            while (q.next()) {
                QVector<QVariant> row;
                ++colOrderLoadRow;
                for (int i = 0; i < rec.count(); ++i) {
                    QVariant val = q.value(i);
                    const QString& fieldName = naturalHeaders[i];
                    if (temporal.contains(fieldName))
                        val = convertTemporalForExport(val, temporal[fieldName], sheetName,
                                                       fieldName, &errors,
                                                       colOrderLoadRow);  // M-1: pass row
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
                QSet<QString> failedAHeaders;
                // H-04 fix: pass failedAHeaders to collect per-lookup failures instead of skipping
                // the whole row on NOT_FOUND.  rowSkip is only set for AMBIGUOUS results.
                QHash<QString, QVariant> aVals =
                    resolveAHeaders(sorted, catalog, rowData, revCache, sheetName, rowCount + 1,
                                    QString(), &errors, &rowSkip, &failedAHeaders);
                if (rowSkip)
                    continue;

                QVector<QVariant> outRow;
                for (const QString& h : finalHeaders) {
                    QVariant val;
                    if (hColReplaceSet.contains(h)) {
                        // H-col-to-replace: should not appear in finalHeaders, skip
                        val = QVariant();
                    } else if (aHeaders.contains(h)) {
                        if (failedAHeaders.contains(h)) {
                            // H-04 fix: this A-header's lookup failed — write NULL for this
                            // column only; other columns in the same row are unaffected.
                            val = QVariant();
                        } else {
                            // 5.6: D5 — ColumnSpec.source value wins if present and non-NULL
                            QVariant srcVal = rowData.value(h);
                            if (!srcVal.isNull())
                                val = srcVal;
                            else
                                val = aVals.value(h, QVariant());
                        }
                    } else {
                        val = rowData.value(h, QVariant());
                    }
                    if (!val.isNull() && temporal.contains(h))
                        val = convertTemporalForExport(val, temporal[h], sheetName, h, &errors,
                                                       rowCount + 1);  // M-1
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
