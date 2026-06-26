#include "UpsertExecutor.h"

#include <QSqlError>

#include "sql/SqlBuilder.h"

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

bool UpsertExecutor::apply(QSqlDatabase& db, const QList<RowMutation>& rows,
                           QList<dbridge::RowError>* errors, QString* err) {
    if (!db.isOpen()) {
        if (err)
            *err = QStringLiteral("database is not open");
        return false;
    }

    for (const RowMutation& m : rows) {
        if (m.columns.isEmpty()) {
            dbridge::RowError re;
            re.sheet = m.table;
            re.row = 0;
            re.code = QStringLiteral("E_SYNC_APPLY_CONSTRAINT");
            re.message = QStringLiteral("empty columns for table %1").arg(m.table);
            if (errors)
                errors->append(re);
            continue;
        }

        // M-04 fix: use the full SQL string as cache key so different column sets or PK sets
        // for the same table always produce distinct cache entries and never share a wrong
        // prepared statement. The old (table:mode) key required a fragile lastQuery() check.
        const QString sql = buildUpsertSql(m.table, m.columns, m.pkColumns, m.mode);
        const QString key = sql;

        bool needPrepare = !cache_.contains(key);
        if (needPrepare) {
            QSqlQuery q(db);
            if (!q.prepare(sql)) {
                // Prepare failure is fatal — the table likely doesn't exist.
                if (err)
                    *err = q.lastError().text();
                return false;
            }
            cache_.insert(key, q);
        }

        QSqlQuery& q = cache_[key];
        // Re-bind values for this row.
        for (const QVariant& v : m.values)
            q.addBindValue(v);

        if (!q.exec()) {
            dbridge::RowError re;
            re.sheet = m.table;
            re.row = 0;
            re.code = QStringLiteral("E_SYNC_APPLY_CONSTRAINT");
            re.message = q.lastError().text();
            // Populate rawValue from originMeta if present.
            if (!m.originMeta.isEmpty())
                re.rawValue = m.originMeta.value(QStringLiteral("rawValue")).toString();
            if (errors)
                errors->append(re);
            // Continue to next row — per-row failure is non-fatal.
            continue;
        }
    }

    return true;
}

void UpsertExecutor::clearPreparedCache() {
    cache_.clear();
}

// ---------------------------------------------------------------------------
// private
// ---------------------------------------------------------------------------

QString UpsertExecutor::buildUpsertSql(const QString& table, const QStringList& cols,
                                       const QStringList& pkCols, UpsertMode mode) {
    // M-05 fix: use SqlBuilder::quoteIdent so embedded double-quotes are properly escaped.
    auto quote = [](const QString& s) { return detail::SqlBuilder::quoteIdent(s); };

    // Build column list: "c1","c2",...
    QStringList quotedCols;
    quotedCols.reserve(cols.size());
    for (const QString& c : cols)
        quotedCols << quote(c);
    const QString colList = quotedCols.join(QStringLiteral(", "));

    // Build placeholder list: ?,?,...
    QStringList placeholders;
    placeholders.reserve(cols.size());
    for (int i = 0; i < cols.size(); ++i)
        placeholders << QStringLiteral("?");
    const QString phList = placeholders.join(QStringLiteral(", "));

    if (mode == UpsertMode::DoNothing) {
        return QStringLiteral("INSERT OR IGNORE INTO %1 (%2) VALUES (%3)")
            .arg(quote(table), colList, phList);
    }

    // DoUpdate: build conflict target and SET clauses (non-pk columns only).
    QStringList quotedPk;
    quotedPk.reserve(pkCols.size());
    for (const QString& pk : pkCols)
        quotedPk << quote(pk);
    const QString pkConflict = quotedPk.join(QStringLiteral(", "));

    QStringList setClauses;
    for (const QString& c : cols) {
        if (!pkCols.contains(c))
            setClauses << QStringLiteral("%1=excluded.%2").arg(quote(c), quote(c));
    }

    if (setClauses.isEmpty()) {
        // All columns are PK columns — fall back to DO NOTHING.
        return QStringLiteral("INSERT OR IGNORE INTO %1 (%2) VALUES (%3)")
            .arg(quote(table), colList, phList);
    }

    return QStringLiteral(
               "INSERT INTO %1 (%2) VALUES (%3) "
               "ON CONFLICT (%4) DO UPDATE SET %5")
        .arg(quote(table), colList, phList, pkConflict, setClauses.join(QStringLiteral(", ")));
}

// cacheKey() is no longer used (M-04 fix replaced it with SQL-as-key). Removed.

}  // namespace dbridge::sync
