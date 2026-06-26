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
            // H-05: quote the ORDER BY column identifier (single identifier only, no direction).
            // If the caller passes "table.column", split and quote each part.
            const int dot = ob.indexOf(QLatin1Char('.'));
            if (dot > 0) {
                orderParts.append(quoteIdent(ob.left(dot)) + QLatin1Char('.') +
                                  quoteIdent(ob.mid(dot + 1)));
            } else {
                orderParts.append(quoteIdent(ob));
            }
        }
        sql += QStringLiteral(" ORDER BY ") + orderParts.join(QStringLiteral(", "));
    }

    return sql;
}

}  // namespace dbridge::detail
