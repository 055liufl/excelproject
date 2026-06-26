#include "SqlBuilder.h"

#include <QSet>

namespace dbridge::detail {

UpsertSql SqlBuilder::buildUpsert(const RoutePayload& payload) {
    UpsertSql result;

    if (payload.dbColumns.isEmpty()) {
        return result;
    }

    QSet<QString> conflictSet = QSet<QString>::fromList(payload.conflictKey);

    // Build column lists
    QStringList allCols;
    QStringList placeholders;
    QStringList updateParts;

    for (const auto& col : payload.dbColumns) {
        allCols.append(col);
        placeholders.append(QStringLiteral("?"));
        result.bindOrder.append(col);

        if (!conflictSet.contains(col)) {
            updateParts.append(col + QStringLiteral(" = excluded.") + col);
        }
    }

    QString sql = QStringLiteral("INSERT INTO ") + payload.table + QStringLiteral(" (") +
                  allCols.join(QStringLiteral(", ")) + QStringLiteral(") VALUES (") +
                  placeholders.join(QStringLiteral(", ")) + QStringLiteral(") ON CONFLICT(") +
                  payload.conflictKey.join(QStringLiteral(", ")) + QStringLiteral(") ");

    if (updateParts.isEmpty()) {
        sql += QStringLiteral("DO NOTHING");
    } else {
        sql += QStringLiteral("DO UPDATE SET ") + updateParts.join(QStringLiteral(", "));
    }

    result.sql = sql;
    return result;
}

QString SqlBuilder::buildAutoJoinSelect(const QVector<RouteSpec>& routes,
                                        const ExportSpec& exportSpec) {
    if (routes.isEmpty())
        return QString();

    QStringList selectCols;
    QString fromClause;
    QStringList joinClauses;

    const RouteSpec& root = routes[0];
    fromClause = root.table;

    for (const auto& route : routes) {
        for (const auto& col : route.columns) {
            selectCols.append(route.table + QStringLiteral(".") + col.dbColumn +
                              QStringLiteral(" AS ") + col.source);
        }

        if (&route != &routes[0] && !route.fkInject.isEmpty()) {
            // M-06 fix: iterate ALL fkInject groups (not just [0]).
            // Each group represents a distinct parent table; generate one JOIN per group.
            // Multiple groups on the same child route yield multiple JOINs (e.g. multi-parent).
            for (const FkInjectSpec& fk : route.fkInject) {
                QStringList onParts;
                for (const auto& pair : fk.pairs) {
                    onParts.append(QStringLiteral("\"%1\".\"%2\" = \"%3\".\"%4\"")
                                       .arg(route.table, pair.second, fk.fromTable, pair.first));
                }
                if (!onParts.isEmpty()) {
                    joinClauses.append(
                        QStringLiteral("LEFT JOIN \"%1\" ON %2")
                            .arg(route.table, onParts.join(QStringLiteral(" AND "))));
                }
            }
        }
    }

    QString sql = QStringLiteral("SELECT ") + selectCols.join(QStringLiteral(", ")) +
                  QStringLiteral(" FROM ") + fromClause;

    for (const auto& j : joinClauses) {
        sql += QStringLiteral(" ") + j;
    }

    if (!exportSpec.orderBy.isEmpty()) {
        QStringList orderParts;
        for (const auto& ob : exportSpec.orderBy) {
            // Convert "table.column" to proper qualified name if needed
            orderParts.append(ob);
        }
        sql += QStringLiteral(" ORDER BY ") + orderParts.join(QStringLiteral(", "));
    }

    return sql;
}

}  // namespace dbridge::detail
