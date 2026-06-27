#include "ForeignKeyPreflight.h"

#include "dbridge/Errors.h"

#include <QSet>
#include <QSqlError>
#include <QSqlQuery>

#include "service/ErrorCollector.h"
#include "sql/SqlBuilder.h"

namespace dbridge::detail {

bool ForeignKeyPreflight::check(QVector<RowContext>& contexts, const QVector<RouteSpec>& allRoutes,
                                QSqlDatabase& db, const QString& sheet, ErrorCollector* errors) {
    // M-02 fix: exclude already-failed routes from the in-batch parent cache.
    // Including failed payloads would let a child route incorrectly pass FK preflight
    // even when its parent will not actually be written (because it failed earlier).
    QHash<QString, QVector<RoutePayload>> batchParentPayloads;
    for (const auto& ctx : contexts) {
        for (int pi = 0; pi < ctx.payloads.size(); ++pi) {
            if (!ctx.failedRouteIndices.contains(pi))
                batchParentPayloads[ctx.payloads[pi].table].append(ctx.payloads[pi]);
        }
    }

    QHash<QString, const RouteSpec*> routeByTable;
    for (const auto& r : allRoutes)
        routeByTable[r.table] = &r;

    bool allOk = true;
    // H-02 fix: iterate with index so we can write back to ctx.failedRouteIndices.
    for (auto& ctx : contexts) {
        for (int pi = 0; pi < ctx.payloads.size(); ++pi) {
            const RoutePayload& payload = ctx.payloads[pi];
            const RouteSpec* rs = routeByTable.value(payload.table, nullptr);
            if (!rs || rs->fkInject.isEmpty())
                continue;
            if (!checkPayload(payload, *rs, batchParentPayloads, routeByTable, db, sheet,
                              ctx.excelRow, errors)) {
                allOk = false;
                // H-02 fix: record which payload (route index) failed FK preflight so the write
                // phase skips only that route (and its descendants), not the entire Excel row.
                ctx.failedRouteIndices.insert(pi);
            }
        }
    }
    return allOk;
}

bool ForeignKeyPreflight::checkPayload(
    const RoutePayload& payload, const RouteSpec& routeSpec,
    const QHash<QString, QVector<RoutePayload>>& batchParentPayloads,
    const QHash<QString, const RouteSpec*>& routeByTable, QSqlDatabase& db, const QString& sheet,
    int excelRow, ErrorCollector* errors) {
    bool ok = true;

    for (const FkInjectSpec& fk : routeSpec.fkInject) {
        // §7.5 Group-level skip for lookup-derived groups.
        // If every pair.first in this group is a lookup select target of the parent route,
        // the parent values were validated at prefetch time — skip the DB probe entirely.
        const RouteSpec* parentSpec = routeByTable.value(fk.fromTable, nullptr);
        if (parentSpec && !parentSpec->lookups.isEmpty()) {
            QSet<QString> parentLookupTargets;
            for (const LookupSpec& lk : parentSpec->lookups) {
                for (const auto& sp : lk.select)
                    parentLookupTargets.insert(sp.second);
            }
            bool allLookupDerived = true;
            for (const auto& pair : fk.pairs) {
                if (!parentLookupTargets.contains(pair.first)) {
                    allLookupDerived = false;
                    break;
                }
            }
            if (allLookupDerived)
                continue;  // validated at prefetch; no SQL probe needed
        }

        // Build the child-side tuple and the SQL column list from pair.second (child cols).
        QVector<QVariant> childTuple;
        QStringList childCols;
        QStringList parentCols;
        bool anyMissing = false;
        bool anyNull = false;

        for (const auto& pair : fk.pairs) {
            int idx = payload.indexOf(pair.second);
            if (idx < 0) {
                anyMissing = true;
                break;
            }
            QVariant v = payload.binds[idx];
            if (v.isNull()) {
                anyNull = true;
                break;
            }
            childTuple.append(v);
            childCols.append(pair.second);
            parentCols.append(pair.first);
        }

        if (anyMissing || anyNull)
            continue;  // not yet injected or null — upstream reports the error

        // Check in-batch: compare tuple against parent payload binds by parent column name
        const auto& batchPayloads = batchParentPayloads.value(fk.fromTable);
        bool foundInBatch = false;
        for (const RoutePayload& parentPayload : batchPayloads) {
            bool match = true;
            for (int i = 0; i < fk.pairs.size(); ++i) {
                int pIdx = parentPayload.indexOf(fk.pairs[i].first);
                if (pIdx < 0 || parentPayload.binds[pIdx] != childTuple[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                foundInBatch = true;
                break;
            }
        }
        if (foundInBatch)
            continue;

        // Not in batch — probe the DB with a composite WHERE clause
        if (onProbe)
            onProbe(fk.fromTable);

        // H-05 fix: quote all identifiers (table name and column names).
        QStringList conditions;
        for (const auto& pc : parentCols)
            conditions.append(detail::SqlBuilder::quoteIdent(pc) + QStringLiteral(" = ?"));
        QString sql = QStringLiteral("SELECT 1 FROM ") +
                      detail::SqlBuilder::quoteIdent(fk.fromTable) + QStringLiteral(" WHERE ") +
                      conditions.join(QStringLiteral(" AND ")) + QStringLiteral(" LIMIT 1");

        QSqlQuery q(db);
        q.prepare(sql);
        for (const auto& v : childTuple)
            q.addBindValue(v);

        if (!q.exec()) {
            errors->add(sheet, excelRow, childCols.join(QLatin1Char(',')), childTuple[0].toString(),
                        QString::fromLatin1(err::E_VALIDATE_FK),
                        QStringLiteral("FK check query failed: ") + q.lastError().text());
            ok = false;
            continue;
        }

        if (!q.next()) {
            QStringList tupleParts;
            QStringList tupleVals;
            for (int i = 0; i < parentCols.size(); ++i) {
                tupleParts.append(parentCols[i] + QLatin1Char('=') + childTuple[i].toString());
                tupleVals.append(childTuple[i].toString());
            }
            errors->add(sheet, excelRow, childCols.join(QLatin1Char(',')),
                        tupleVals.join(QLatin1Char(',')), QString::fromLatin1(err::E_VALIDATE_FK),
                        QStringLiteral("Foreign key (") + tupleParts.join(QStringLiteral(", ")) +
                            QStringLiteral(") not found in ") + fk.fromTable);
            ok = false;
        }
    }

    return ok;
}

}  // namespace dbridge::detail
