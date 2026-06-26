#include "Mapper.h"

#include "dbridge/Errors.h"

#include "TemporalConvert.h"
#include "excel/ExcelReader.h"
#include "service/ErrorCollector.h"

namespace dbridge::detail {

QString Mapper::routeKey(const RouteSpec& route, const QString& classId) {
    if (classId.isEmpty())
        return route.table;
    return classId + QStringLiteral(":") + route.table;
}

bool Mapper::compileValidators(const QVector<RouteSpec>& routes, const QString& classId,
                               const ProfileSpec& profile, ValidatorMap* vm, QString* err) {
    for (const auto& route : routes) {
        QString rk = routeKey(route, classId);
        for (const auto& col : route.columns) {
            QStringList tokens = col.validatorTokens;
            // Strip date:* tokens only for columns whose effective temporal slot is declared.
            // Legacy date:fmt-only columns (no dateFormat object declared) keep their validator.
            TemporalSlotKind kind = temporalSlotKindFor(col, profile);
            if (kind != TemporalSlotKind::None &&
                effectiveTemporalFor(kind, col, profile).declared) {
                tokens.erase(std::remove_if(tokens.begin(), tokens.end(),
                                            [](const QString& t) {
                                                return t.startsWith(QStringLiteral("date:"));
                                            }),
                             tokens.end());
            }
            ValidatorChain chain;
            if (!chain.compile(tokens, err))
                return false;
            (*vm)[rk][col.dbColumn] = std::move(chain);
        }
    }
    return true;
}

QVector<RoutePayload> Mapper::map(const QVector<RouteSpec>& routes, int excelRow,
                                  const QString& classId, const ExcelReader& reader,
                                  const ValidatorMap& vm, const ProfileSpec& profile,
                                  ErrorCollector* errors, const QString& sheetName) const {
    QVector<RoutePayload> payloads;

    for (const auto& route : routes) {
        QString rk = routeKey(route, classId);
        RoutePayload payload;
        payload.table = route.table;
        payload.routeKey = rk;
        payload.conflictKey = route.conflict.columns;

        const auto& chainMap = vm.value(rk);

        bool rowHasError = false;
        for (const auto& col : route.columns) {
            QVariant rawVal = reader.cellBySource(excelRow, col.source);
            QVariant normalizedVal = rawVal;

            const ValidatorChain& chain = chainMap.value(col.dbColumn);
            if (!chain.isEmpty()) {
                QString errCode, errMsg;
                if (!chain.run(rawVal, &normalizedVal, &errCode, &errMsg)) {
                    // M-06 fix: pass sheetName so error entries have a complete location.
                    errors->add(sheetName, excelRow, col.source, rawVal.toString(), errCode,
                                errMsg);
                    normalizedVal = rawVal;
                    rowHasError = true;
                    // H-01 fix: mark only this route's payload as failed so ImportService
                    // inserts this routeIndex into failedRouteIndices instead of setting
                    // hasNonRouteError (which skips the entire row).
                    payload.hasError = true;
                }
            }

            // Temporal conversion: parse (U → structured) → serialize (structured → V).
            // Only activates when a dateFormat/datetimeFormat/timeFormat object is declared
            // (effective.declared == true). Legacy date:fmt-only columns are left to the validator.
            TemporalSlotKind kind = temporalSlotKindFor(col, profile);
            if (kind != TemporalSlotKind::None && !rowHasError) {
                TemporalFormatSpec eff = effectiveTemporalFor(kind, col, profile);
                if (eff.excel.declared || eff.db.declared) {
                    if (tconv::isEmptyForTemporal(normalizedVal)) {
                        normalizedVal = QVariant();
                    } else if (tconv::isStructuredTemporal(normalizedVal, kind)) {
                        // Native Excel date cell: bypass U parse, serialize directly with V
                        QVariant serialized = tconv::formatValue(normalizedVal, kind, eff.db);
                        normalizedVal = (serialized.isValid() && !serialized.isNull()) ? serialized
                                                                                       : QVariant();
                    } else {
                        // String cell: parse with U (+ fallback), then serialize with V
                        QString errCode, errMsg;
                        QVariant structured =
                            tconv::toStructured(normalizedVal, kind, eff.excel, &errCode, &errMsg);
                        if (!structured.isValid()) {
                            errors->add(sheetName, excelRow, col.source, rawVal.toString(), errCode,
                                        errMsg);
                            normalizedVal = rawVal;
                            rowHasError = true;
                            // H-01 fix: temporal parse failure is route-local — only this route's
                            // payload is invalid, not the entire row.
                            payload.hasError = true;
                        } else {
                            QVariant serialized = tconv::formatValue(structured, kind, eff.db);
                            normalizedVal = (serialized.isValid() && !serialized.isNull())
                                                ? serialized
                                                : QVariant();
                        }
                    }
                }
            }

            payload.dbColumns.append(col.dbColumn);
            payload.binds.append(normalizedVal);
        }

        // Fill conflictVals from bound columns
        for (const auto& ck : route.conflict.columns) {
            int idx = payload.indexOf(ck);
            if (idx >= 0) {
                payload.conflictVals.append(payload.binds[idx]);
            } else {
                payload.conflictVals.append(QVariant());  // will be filled by FkInjector
            }
        }

        payloads.append(payload);
    }

    return payloads;
}

}  // namespace dbridge::detail
