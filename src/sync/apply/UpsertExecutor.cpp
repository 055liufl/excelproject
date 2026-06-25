#include "UpsertExecutor.h"

#include <QSqlError>

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

        const QString key = cacheKey(m.table, m.mode);

        // Build or reuse a prepared statement.
        // Statements are keyed by (table, mode); we assume the column list is
        // stable within a single apply() call for a given key.  If the
        // columns differ we rebuild (the old entry is replaced).
        bool needPrepare = !cache_.contains(key);
        if (!needPrepare) {
            // Sanity: if column count changed, force re-prepare.
            const QSqlQuery& cached = cache_[key];
            Q_UNUSED(cached);
            // Column count mismatch check: boundValues() is populated only
            // after exec(), so we track mismatches via the SQL string.
            // Simplest: rebuild if the cached placeholder count ≠ current.
            const QString expectedSql = buildUpsertSql(m.table, m.columns, m.pkColumns, m.mode);
            if (cache_[key].lastQuery() != expectedSql)
                needPrepare = true;
        }

        if (needPrepare) {
            const QString sql = buildUpsertSql(m.table, m.columns, m.pkColumns, m.mode);
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
    // Quote identifiers with double-quotes.
    auto quote = [](const QString& s) { return QStringLiteral("\"") + s + QStringLiteral("\""); };

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

QString UpsertExecutor::cacheKey(const QString& table, UpsertMode mode) const {
    return table + QStringLiteral(":") +
           (mode == UpsertMode::DoUpdate ? QStringLiteral("U") : QStringLiteral("N"));
}

}  // namespace dbridge::sync
