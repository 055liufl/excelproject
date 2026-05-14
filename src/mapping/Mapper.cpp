#include "Mapper.h"

#include "excel/ExcelReader.h"
#include "service/ErrorCollector.h"

namespace dbridge::detail {

QString Mapper::routeKey(const RouteSpec& route, const QString& classId) {
    if (classId.isEmpty())
        return route.table;
    return classId + QStringLiteral(":") + route.table;
}

bool Mapper::compileValidators(const QVector<RouteSpec>& routes, const QString& classId,
                               ValidatorMap* vm, QString* err) {
    for (const auto& route : routes) {
        QString rk = routeKey(route, classId);
        for (const auto& col : route.columns) {
            ValidatorChain chain;
            if (!chain.compile(col.validatorTokens, err))
                return false;
            (*vm)[rk][col.dbColumn] = std::move(chain);
        }
    }
    return true;
}

QVector<RoutePayload> Mapper::map(const QVector<RouteSpec>& routes, int excelRow,
                                  const QString& classId, const ExcelReader& reader,
                                  const ValidatorMap& vm, ErrorCollector* errors) const {
    QVector<RoutePayload> payloads;

    for (const auto& route : routes) {
        QString rk = routeKey(route, classId);
        RoutePayload payload;
        payload.table = route.table;
        payload.routeKey = rk;
        payload.conflictKey = route.conflict.columns;

        const auto& chainMap = vm.value(rk);

        for (const auto& col : route.columns) {
            QVariant rawVal = reader.cellBySource(excelRow, col.source);
            QVariant normalizedVal = rawVal;

            const ValidatorChain& chain = chainMap.value(col.dbColumn);
            if (!chain.isEmpty()) {
                QString errCode, errMsg;
                if (!chain.run(rawVal, &normalizedVal, &errCode, &errMsg)) {
                    errors->add(reader.headers().isEmpty() ? QString() : QString(), excelRow,
                                col.source, rawVal.toString(), errCode, errMsg);
                    normalizedVal = rawVal;  // keep original for context
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
