#include "ForeignKeyPreflight.h"

#include "dbridge/Errors.h"

#include <QSqlError>
#include <QSqlQuery>

#include "service/ErrorCollector.h"

namespace dbridge::detail {

bool ForeignKeyPreflight::check(const QVector<RowContext>& contexts,
                                const QVector<RouteSpec>& allRoutes, QSqlDatabase& db,
                                const QString& sheet, ErrorCollector* errors) {
    // Build a map of routeKey -> set of conflict key values present in this batch
    QHash<QString, QVector<QVariant>> batchParentKeys;
    for (const auto& ctx : contexts) {
        for (const auto& payload : ctx.payloads) {
            if (!payload.conflictVals.isEmpty() && !payload.conflictVals[0].isNull()) {
                batchParentKeys[payload.routeKey].append(payload.conflictVals[0]);
            }
        }
    }

    // Build route lookup
    QHash<QString, const RouteSpec*> routeByTable;
    for (const auto& r : allRoutes)
        routeByTable[r.table] = &r;

    bool allOk = true;
    for (const auto& ctx : contexts) {
        for (const auto& payload : ctx.payloads) {
            const RouteSpec* rs = routeByTable.value(payload.table, nullptr);
            if (!rs || !rs->fkInject.has_value())
                continue;
            if (!checkPayload(payload, *rs, batchParentKeys, db, sheet, ctx.excelRow, errors)) {
                allOk = false;
            }
        }
    }
    return allOk;
}

bool ForeignKeyPreflight::checkPayload(const RoutePayload& payload, const RouteSpec& routeSpec,
                                       const QHash<QString, QVector<QVariant>>& batchParentKeys,
                                       QSqlDatabase& db, const QString& sheet, int excelRow,
                                       ErrorCollector* errors) {
    const FkInjectSpec& fk = routeSpec.fkInject.value();

    // Find the FK value in payload
    int fkColIdx = payload.indexOf(fk.toColumn);
    if (fkColIdx < 0)
        return true;  // not yet injected, skip
    QVariant fkVal = payload.binds[fkColIdx];
    if (fkVal.isNull())
        return true;

    // Check if parent row exists in this batch
    QString parentRouteKey = fk.fromTable;
    const auto& batchKeys = batchParentKeys.value(parentRouteKey);
    for (const auto& bk : batchKeys) {
        if (bk == fkVal)
            return true;  // found in batch
    }

    // Check in DB
    QSqlQuery q(db);
    q.prepare(QStringLiteral("SELECT 1 FROM ") + fk.fromTable + QStringLiteral(" WHERE ") +
              fk.fromColumn + QStringLiteral(" = ? LIMIT 1"));
    q.addBindValue(fkVal);
    if (!q.exec()) {
        errors->add(sheet, excelRow, fk.toColumn, fkVal.toString(),
                    QString::fromLatin1(err::E_VALIDATE_FK),
                    QStringLiteral("FK check query failed: ") + q.lastError().text());
        return false;
    }
    if (!q.next()) {
        errors->add(sheet, excelRow, fk.toColumn, fkVal.toString(),
                    QString::fromLatin1(err::E_VALIDATE_FK),
                    QStringLiteral("Foreign key '") + fkVal.toString() +
                        QStringLiteral("' not found in ") + fk.fromTable + '.' + fk.fromColumn);
        return false;
    }
    return true;
}

}  // namespace dbridge::detail
