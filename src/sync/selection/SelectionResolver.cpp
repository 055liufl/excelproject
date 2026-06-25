#include "SelectionResolver.h"

#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

namespace dbridge::sync {

// Returns the pk column name for a table via PRAGMA table_info.
// Returns empty string on failure.
static QString pkColumnForTable(const QString& table, QSqlDatabase& rconn) {
    QSqlQuery q(rconn);
    q.prepare(QStringLiteral("PRAGMA table_info(\"%1\")").arg(table));
    if (!q.exec())
        return {};
    while (q.next()) {
        // pk column: pragma column index 5 (pk), nonzero means primary key
        int pkOrder = q.value(5).toInt();
        if (pkOrder == 1) {
            return q.value(1).toString();  // column name at index 1
        }
    }
    return {};
}

// Converts a QSqlQuery's current record to a QVariantMap.
static QVariantMap recordToMap(const QSqlQuery& q) {
    QVariantMap row;
    const QSqlRecord rec = q.record();
    for (int i = 0; i < rec.count(); ++i)
        row.insert(rec.fieldName(i), q.value(i));
    return row;
}

QString SelectionResolver::rowToPk(const QVariantMap& row, const QString& table,
                                   QSqlDatabase& rconn) {
    const QString pkCol = pkColumnForTable(table, rconn);
    if (pkCol.isEmpty())
        return {};
    return row.value(pkCol).toString();
}

bool SelectionResolver::resolveRecord(QSqlDatabase& rconn, const QString& table, const QString& pk,
                                      QList<ResolveResult>* out, QString* err) {
    const QString pkCol = pkColumnForTable(table, rconn);
    if (pkCol.isEmpty()) {
        if (err)
            *err = QStringLiteral("Cannot determine PK column for table '%1'").arg(table);
        return false;
    }

    QSqlQuery q(rconn);
    q.prepare(QStringLiteral("SELECT * FROM \"%1\" WHERE \"%2\" = ? LIMIT 1").arg(table, pkCol));
    q.addBindValue(pk);

    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }

    if (q.next()) {
        ResolveResult r;
        r.table = table;
        r.pk = pk;
        r.row = recordToMap(q);
        out->append(std::move(r));
    }
    return true;
}

bool SelectionResolver::resolveWhere(QSqlDatabase& rconn, const QString& table,
                                     const QString& whereExpr, QList<ResolveResult>* out,
                                     QString* err) {
    const QString pkCol = pkColumnForTable(table, rconn);
    if (pkCol.isEmpty()) {
        if (err)
            *err = QStringLiteral("Cannot determine PK column for table '%1'").arg(table);
        return false;
    }

    QSqlQuery q(rconn);
    const QString sql = QStringLiteral("SELECT * FROM \"%1\" WHERE %2").arg(table, whereExpr);
    if (!q.exec(sql)) {
        if (err)
            *err = q.lastError().text();
        return false;
    }

    while (q.next()) {
        QVariantMap row = recordToMap(q);
        ResolveResult r;
        r.table = table;
        r.pk = row.value(pkCol).toString();
        r.row = std::move(row);
        out->append(std::move(r));
    }
    return true;
}

bool SelectionResolver::resolvePk(QSqlDatabase& rconn, const SyncSelection& sel,
                                  QList<ResolveResult>* out, QString* err) {
    for (const auto& rec : sel.records()) {
        if (!resolveRecord(rconn, rec.table, rec.primaryKey, out, err))
            return false;
    }
    for (const auto& wc : sel.whereClauses()) {
        if (!resolveWhere(rconn, wc.table, wc.whereExpr, out, err))
            return false;
    }
    return true;
}

}  // namespace dbridge::sync
