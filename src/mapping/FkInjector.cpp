#include "FkInjector.h"

#include "dbridge/Errors.h"

#include "profile/ProfileSpec.h"
#include "service/ErrorCollector.h"

namespace dbridge::detail {

bool FkInjector::inject(QVector<RoutePayload>& payloads, QString* err) {
    // Build a map of routeKey -> payload index for fast lookup
    QHash<QString, int> routeIndex;
    for (int i = 0; i < payloads.size(); ++i) {
        routeIndex[payloads[i].table] = i;
    }

    // The payloads carry fkInject info via their route key naming convention.
    // The actual FK injection info is carried by the RouteSpec, but here we work
    // with already-built payloads.
    //
    // Convention: each payload that needs FK inject has conflictKey columns that
    // may not yet have conflictVals. The FK value comes from the parent payload.
    //
    // This injector is called with profile context in ImportService which passes
    // RouteSpecs alongside payloads. Since RoutePayload doesn't store fkInject
    // directly, we use the profile information via the ImportService.
    //
    // For this standalone version, we look for payload columns that appear in
    // conflictKey but have no corresponding bind value (empty slot).
    // The actual injection is done with RouteSpec context in ImportService::run.
    Q_UNUSED(routeIndex)
    Q_UNUSED(err)
    return true;  // actual injection done in ImportService with profile context
}

QSet<int> FkInjector::inject(QVector<RoutePayload>& payloads, const QVector<RouteSpec>& routes,
                             int excelRow, const QString& sheet, ErrorCollector* errors,
                             QSet<int> initialFailed) {
    QHash<QString, int> tableToPayloadIdx;
    for (int i = 0; i < payloads.size(); ++i)
        tableToPayloadIdx[payloads[i].table] = i;

    QHash<int, int> parentIdx;
    for (int i = 0; i < routes.size(); ++i) {
        const QString& parentTable = routes[i].parent;
        if (!parentTable.isEmpty() && tableToPayloadIdx.contains(parentTable))
            parentIdx[i] = tableToPayloadIdx[parentTable];
        else
            parentIdx[i] = -1;
    }

    QSet<int> failedIdxs = std::move(initialFailed);

    for (int i = 0; i < routes.size(); ++i) {
        const RouteSpec& route = routes[i];
        if (route.fkInject.isEmpty())
            continue;

        bool ancestorFailed = false;
        int ancestor = parentIdx.value(i, -1);
        while (ancestor >= 0) {
            if (failedIdxs.contains(ancestor)) {
                ancestorFailed = true;
                break;
            }
            ancestor = parentIdx.value(ancestor, -1);
        }
        if (ancestorFailed)
            continue;

        RoutePayload& childPayload = payloads[i];
        bool rowFailed = false;

        for (const FkInjectSpec& fk : route.fkInject) {
            auto parentIt = tableToPayloadIdx.find(fk.fromTable);
            if (parentIt == tableToPayloadIdx.end())
                continue;

            const RoutePayload& parentPayload = payloads[parentIt.value()];
            for (const auto& pair : fk.pairs) {
                const QString& parentCol = pair.first;
                const QString& childCol = pair.second;

                const int fromIdx = parentPayload.indexOf(parentCol);
                if (fromIdx < 0)
                    continue;
                const QVariant fkVal = parentPayload.binds[fromIdx];

                if (fkVal.isNull()) {
                    if (errors) {
                        errors->add(sheet, excelRow, childCol, QString(),
                                    QString::fromLatin1(err::E_VALIDATE_FK),
                                    QStringLiteral("fkInject from '") + fk.fromTable +
                                        QStringLiteral("': parent column '") + parentCol +
                                        QStringLiteral("' is NULL; cannot inject into '") +
                                        childCol + '\'');
                    }
                    rowFailed = true;
                    continue;
                }

                int toIdx = childPayload.indexOf(childCol);
                if (toIdx >= 0) {
                    const QVariant current = childPayload.binds[toIdx];
                    if (!current.isNull() && current != fkVal) {
                        if (errors) {
                            errors->add(sheet, excelRow, childCol, current.toString(),
                                        QString::fromLatin1(err::E_VALIDATE_FK),
                                        QStringLiteral("fkInject conflict for '") + childCol +
                                            QStringLiteral("': child value '") +
                                            current.toString() +
                                            QStringLiteral("' does not match parent value '") +
                                            fkVal.toString() + '\'');
                        }
                        rowFailed = true;
                        continue;
                    }
                    childPayload.binds[toIdx] = fkVal;
                } else {
                    childPayload.dbColumns.append(childCol);
                    childPayload.binds.append(fkVal);
                }

                for (int ci = 0; ci < childPayload.conflictKey.size(); ++ci) {
                    if (childPayload.conflictKey[ci] == childCol &&
                        ci < childPayload.conflictVals.size()) {
                        childPayload.conflictVals[ci] = fkVal;
                    }
                }
            }
        }

        if (rowFailed)
            failedIdxs.insert(i);
    }

    return failedIdxs;
}

}  // namespace dbridge::detail
