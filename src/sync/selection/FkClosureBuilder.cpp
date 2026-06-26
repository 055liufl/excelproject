#include "FkClosureBuilder.h"

#include "dbridge/Errors.h"

#include <QCryptographicHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

#include "sql/SqlBuilder.h"

namespace dbridge::sync {

// Stable key for dedup set: "table\x1fpk"
static QString entryKey(const QString& table, const QString& pk) {
    return table + QLatin1Char('\x1f') + pk;
}

// Rough fingerprint for a row (used for consistency check when localFp is unknown).
static QByteArray rowFingerprint(const QVariantMap& row) {
    QByteArray buf;
    for (auto it = row.constBegin(); it != row.constEnd(); ++it) {
        buf += it.key().toUtf8();
        buf += '\0';
        buf += it.value().toString().toUtf8();
        buf += '\0';
    }
    return QCryptographicHash::hash(buf, QCryptographicHash::Sha1);
}

QString FkClosureBuilder::getPkColumn(QSqlDatabase& rconn, const QString& table) {
    auto it = pkColCache_.constFind(table);
    if (it != pkColCache_.constEnd())
        return *it;

    QSqlQuery q(rconn);
    // M-11 fix: quote identifier (escapes embedded double-quotes).
    q.prepare(QStringLiteral("PRAGMA table_info(") + detail::SqlBuilder::quoteIdent(table) +
              QLatin1Char(')'));
    if (!q.exec())
        return {};
    while (q.next()) {
        if (q.value(5).toInt() == 1) {
            const QString col = q.value(1).toString();
            pkColCache_.insert(table, col);
            return col;
        }
    }
    pkColCache_.insert(table, {});
    return {};
}

bool FkClosureBuilder::fetchRow(QSqlDatabase& rconn, const QString& table, const QString& pk,
                                QVariantMap* row, QString* err) {
    const QString pkCol = getPkColumn(rconn, table);
    if (pkCol.isEmpty()) {
        if (err)
            *err = QStringLiteral("No PK column for table '%1'").arg(table);
        return false;
    }

    QSqlQuery q(rconn);
    // M-11 fix: quote table and column identifiers.
    q.prepare(QStringLiteral("SELECT * FROM ") + detail::SqlBuilder::quoteIdent(table) +
              QStringLiteral(" WHERE ") + detail::SqlBuilder::quoteIdent(pkCol) +
              QStringLiteral(" = ? LIMIT 1"));
    q.addBindValue(pk);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }

    if (!q.next()) {
        row->clear();
        return true;  // not found — caller checks
    }

    const QSqlRecord rec = q.record();
    for (int i = 0; i < rec.count(); ++i)
        row->insert(rec.fieldName(i), q.value(i));
    return true;
}

bool FkClosureBuilder::buildClosure(QSqlDatabase& rconn,
                                    const dbridge::detail::SchemaCatalog& catalog,
                                    ConsistencyCache& cache, bool pruneConsistent,
                                    QList<Entry>& work, QSet<QString>& seen, QString* err) {
    // Iterative BFS to avoid deep recursion.
    int cursor = 0;
    while (cursor < work.size()) {
        const Entry& e = work[cursor++];

        const dbridge::detail::TableInfo* ti = catalog.table(e.table);
        if (!ti)
            continue;  // table not in catalog, skip FK expansion

        for (const dbridge::detail::FkInfo& fk : ti->foreignKeys) {
            // Get the FK column value from this row.
            auto colIt = e.row.constFind(fk.fromColumn);
            if (colIt == e.row.constEnd() || colIt->isNull())
                continue;

            const QString refPk = colIt->toString();
            const QString key = entryKey(fk.refTable, refPk);
            if (seen.contains(key))
                continue;
            seen.insert(key);

            QVariantMap depRow;
            if (!fetchRow(rconn, fk.refTable, refPk, &depRow, err))
                return false;

            if (depRow.isEmpty()) {
                if (err)
                    *err = QLatin1String(dbridge::err::E_SYNC_FK_CLOSURE_MISSING);
                return false;
            }

            // Prune if consistent.
            if (pruneConsistent) {
                const QByteArray fp = rowFingerprint(depRow);
                if (cache.isConsistent(fk.refTable, refPk, fp))
                    continue;
            }

            Entry dep;
            dep.table = fk.refTable;
            dep.pk = refPk;
            dep.row = std::move(depRow);
            dep.isSelected = false;
            work.append(std::move(dep));
        }
    }
    return true;
}

bool FkClosureBuilder::topoSort(QList<Entry>& entries,
                                const dbridge::detail::SchemaCatalog& catalog, QString* err) {
    const int n = entries.size();

    // Map "table:pk" -> index in entries.
    QHash<QString, int> idx;
    idx.reserve(n);
    for (int i = 0; i < n; ++i)
        idx.insert(entryKey(entries[i].table, entries[i].pk), i);

    // Build in-degree and adjacency (dep -> dependant).
    QVector<int> inDegree(n, 0);
    QVector<QVector<int>> adj(n);

    for (int i = 0; i < n; ++i) {
        const dbridge::detail::TableInfo* ti = catalog.table(entries[i].table);
        if (!ti)
            continue;
        for (const dbridge::detail::FkInfo& fk : ti->foreignKeys) {
            auto colIt = entries[i].row.constFind(fk.fromColumn);
            if (colIt == entries[i].row.constEnd() || colIt->isNull())
                continue;
            const QString refKey = entryKey(fk.refTable, colIt->toString());
            auto refIt = idx.constFind(refKey);
            if (refIt == idx.constEnd())
                continue;
            const int refIdx = *refIt;
            // entries[i] depends on entries[refIdx] => refIdx must come first.
            adj[refIdx].push_back(i);
            inDegree[i]++;
        }
    }

    // Kahn's algorithm.
    QList<int> queue;
    for (int i = 0; i < n; ++i)
        if (inDegree[i] == 0)
            queue.append(i);

    int order = 0;
    while (!queue.isEmpty()) {
        int cur = queue.takeFirst();
        entries[cur].topoIndex = order++;
        for (int next : adj[cur]) {
            if (--inDegree[next] == 0)
                queue.append(next);
        }
    }

    if (order != n) {
        if (err)
            *err = QString::fromLatin1(dbridge::err::E_SYNC_FK_CYCLE_UNSUPPORTED);
        return false;
    }

    // Sort entries by topoIndex.
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.topoIndex < b.topoIndex; });
    return true;
}

bool FkClosureBuilder::build(QSqlDatabase& rconn,
                             const QList<SelectionResolver::ResolveResult>& selected,
                             const dbridge::detail::SchemaCatalog& catalog, ConsistencyCache& cache,
                             qint64 maxSize, QList<Entry>* out, QString* err) {
    QList<Entry> work;
    work.reserve(selected.size());
    QSet<QString> seen;

    for (const auto& r : selected) {
        const QString key = entryKey(r.table, r.pk);
        if (seen.contains(key))
            continue;
        seen.insert(key);

        Entry e;
        e.table = r.table;
        e.pk = r.pk;
        e.row = r.row;
        e.isSelected = true;
        work.append(std::move(e));
    }

    // Expand FK closure (pruneConsistent=true by default; caller controls via
    // SyncSelection but we receive already-resolved rows, so we always expand here).
    if (!buildClosure(rconn, catalog, cache, /*pruneConsistent=*/true, work, seen, err))
        return false;

    if (static_cast<qint64>(work.size()) > maxSize) {
        if (err)
            *err = QString::fromLatin1(dbridge::err::E_SYNC_SELECTION_TOO_LARGE);
        return false;
    }

    if (!topoSort(work, catalog, err))
        return false;

    *out = std::move(work);
    return true;
}

}  // namespace dbridge::sync
