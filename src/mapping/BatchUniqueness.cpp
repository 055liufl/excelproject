#include "BatchUniqueness.h"

#include "dbridge/Errors.h"

#include "service/ErrorCollector.h"

namespace dbridge::detail {

QString BatchUniqueness::encodeConflictKey(const QVector<QVariant>& vals) {
    QString encoded;
    for (const auto& v : vals) {
        QString s = v.isNull() ? QStringLiteral("<null>") : v.toString();
        encoded += QString::number(s.length()) + QStringLiteral("|") + s + QStringLiteral("|");
    }
    return encoded;
}

bool BatchUniqueness::check(const RoutePayload& payload, int excelRow, bool hasChildren,
                            ErrorCollector* errors, const QString& sheet) {
    // Only check if conflict values are fully populated
    bool hasNullConflict = false;
    for (const auto& v : payload.conflictVals) {
        if (v.isNull()) {
            hasNullConflict = true;
            break;
        }
    }
    if (hasNullConflict)
        return true;  // can't deduplicate without complete key

    QString key = encodeConflictKey(payload.conflictVals);
    auto& routeMap = seen_[payload.routeKey];
    auto it = routeMap.find(key);

    if (it != routeMap.end()) {
        const SeenEntry& first = it.value();
        // Allow duplicate if this is a parent route (hasChildren) AND binds are identical
        if (hasChildren && payload.binds == first.binds) {
            return true;  // allowed parent row dedup
        }
        // Real duplicate
        errors->add(sheet, excelRow, payload.conflictKey.join(','), key,
                    QString::fromLatin1(err::E_VALIDATE_DUPLICATE),
                    QStringLiteral("Conflict key already seen at row %1").arg(first.excelRow));
        return false;
    }

    routeMap[key] = SeenEntry{excelRow, payload.binds};
    return true;
}

}  // namespace dbridge::detail
