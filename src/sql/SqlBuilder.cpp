#include "SqlBuilder.h"

#include <QSet>

namespace dbridge::detail {

// H-05 fix: double-quote identifier, escaping any embedded double-quotes per SQL standard.
QString SqlBuilder::quoteIdent(const QString& name) {
    return QLatin1Char('"') + QString(name).replace(QLatin1Char('"'), QLatin1String("\"\"")) +
           QLatin1Char('"');
}

UpsertSql SqlBuilder::buildUpsert(const RoutePayload& payload) {
    UpsertSql result;

    if (payload.dbColumns.isEmpty()) {
        return result;
    }

    QSet<QString> conflictSet = QSet<QString>::fromList(payload.conflictKey);

    // H-05 fix: quote all identifiers so reserved words and special chars are safe.
    QStringList allCols;
    QStringList placeholders;
    QStringList updateParts;

    for (const auto& col : payload.dbColumns) {
        allCols.append(quoteIdent(col));
        placeholders.append(QStringLiteral("?"));
        result.bindOrder.append(col);

        if (!conflictSet.contains(col)) {
            updateParts.append(quoteIdent(col) + QStringLiteral(" = excluded.") + quoteIdent(col));
        }
    }

    QStringList quotedConflict;
    for (const auto& ck : payload.conflictKey)
        quotedConflict.append(quoteIdent(ck));

    QString sql = QStringLiteral("INSERT INTO ") + quoteIdent(payload.table) +
                  QStringLiteral(" (") + allCols.join(QStringLiteral(", ")) +
                  QStringLiteral(") VALUES (") + placeholders.join(QStringLiteral(", ")) +
                  QStringLiteral(") ON CONFLICT(") + quotedConflict.join(QStringLiteral(", ")) +
                  QStringLiteral(") ");

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
    fromClause = quoteIdent(root.table);

    for (const auto& route : routes) {
        for (const auto& col : route.columns) {
            // H-05: quote table and column names; use column source (Excel header) as alias
            selectCols.append(quoteIdent(route.table) + QStringLiteral(".") +
                              quoteIdent(col.dbColumn) + QStringLiteral(" AS ") +
                              quoteIdent(col.source));
        }

        if (&route != &routes[0] && !route.fkInject.isEmpty()) {
            // H-06 fix: use quoteIdent() for all identifiers in JOIN ON clauses.
            // M-06 fix: iterate ALL fkInject groups (not just [0]).
            // Multiple FK inject groups on the same child route share ONE JOIN with all ON
            // conditions AND-ed together (all FK conditions must hold simultaneously).
            // Using one combined JOIN avoids duplicate LEFT JOIN on the same table, which would
            // require aliases that break the SELECT clause's unaliased table.column references.
            QStringList allOnParts;
            for (const FkInjectSpec& fk : route.fkInject) {
                for (const auto& pair : fk.pairs) {
                    allOnParts.append(quoteIdent(route.table) + QStringLiteral(".") +
                                      quoteIdent(pair.second) + QStringLiteral(" = ") +
                                      quoteIdent(fk.fromTable) + QStringLiteral(".") +
                                      quoteIdent(pair.first));
                }
            }
            if (!allOnParts.isEmpty()) {
                joinClauses.append(QStringLiteral("LEFT JOIN ") + quoteIdent(route.table) +
                                   QStringLiteral(" ON ") +
                                   allOnParts.join(QStringLiteral(" AND ")));
            }
        }
    }

    QString sql = QStringLiteral("SELECT ") + selectCols.join(QStringLiteral(", ")) +
                  QStringLiteral(" FROM ") + fromClause;

    for (const auto& j : joinClauses) {
        sql += QStringLiteral(" ") + j;
    }

    if (!exportSpec.orderBy.isEmpty()) {
        // H-06 fix: qualify unqualified ORDER BY columns with their owning table name to avoid
        // "ambiguous column name" errors when the same column name appears in multiple joined
        // tables (e.g. both orders.order_no and order_items.order_no are in scope after a JOIN).
        QHash<QString, QString> dbColToTable;
        for (const auto& route : routes) {
            for (const auto& col : route.columns) {
                if (!dbColToTable.contains(col.dbColumn))
                    dbColToTable.insert(col.dbColumn, route.table);
            }
        }

        QStringList orderParts;
        for (const auto& ob : exportSpec.orderBy) {
            const int dot = ob.indexOf(QLatin1Char('.'));
            if (dot > 0) {
                // Already qualified by the caller — just quote each part.
                orderParts.append(quoteIdent(ob.left(dot)) + QLatin1Char('.') +
                                  quoteIdent(ob.mid(dot + 1)));
            } else if (dbColToTable.contains(ob)) {
                // Qualify with the owning table so the identifier is unambiguous across joins.
                orderParts.append(quoteIdent(dbColToTable[ob]) + QLatin1Char('.') + quoteIdent(ob));
            } else {
                orderParts.append(quoteIdent(ob));
            }
        }
        sql += QStringLiteral(" ORDER BY ") + orderParts.join(QStringLiteral(", "));
    }

    return sql;
}

}  // namespace dbridge::detail
