#pragma once
#include <QList>
#include <QSet>
#include <QSqlDatabase>
#include <QString>

#include "../../schema/SchemaCatalog.h"
#include "ConsistencyCache.h"
#include "SelectionResolver.h"

namespace dbridge::sync {

// Computes the transitive FK closure of a selection and produces a
// topologically sorted manifest.
class FkClosureBuilder {
   public:
    struct Entry {
        QString table;
        QString pk;
        QVariantMap row;
        bool isSelected = false;  // true = directly selected, false = FK dependency
        int topoIndex = 0;
    };

    // Build closure from directly-selected rows.
    // catalog provides FK graph. cache is used to prune consistent deps.
    // maxSize: fail with E_SYNC_SELECTION_TOO_LARGE if exceeded.
    // H-02 fix: includeFkDeps and pruneConsistent honour the SyncSelection flags.
    bool build(QSqlDatabase& rconn, const QList<SelectionResolver::ResolveResult>& selected,
               const dbridge::detail::SchemaCatalog& catalog, ConsistencyCache& cache,
               qint64 maxSize, QList<Entry>* out, QString* err, bool includeFkDeps = true,
               bool pruneConsistent = true);

   private:
    // Recursively expand FK dependencies into work list.
    bool buildClosure(QSqlDatabase& rconn, const dbridge::detail::SchemaCatalog& catalog,
                      ConsistencyCache& cache, bool pruneConsistent, QList<Entry>& work,
                      QSet<QString>& seen, QString* err);

    // Kahn topological sort; returns E_SYNC_FK_CYCLE_UNSUPPORTED on cycle.
    bool topoSort(QList<Entry>& entries, const dbridge::detail::SchemaCatalog& catalog,
                  QString* err);

    // Fetch a single row by PK from rconn.
    bool fetchRow(QSqlDatabase& rconn, const QString& table, const QString& pk, QVariantMap* row,
                  QString* err);

    // Get PK column name for a table via PRAGMA table_info.
    QString getPkColumn(QSqlDatabase& rconn, const QString& table);
    QHash<QString, QString> pkColCache_;
};

}  // namespace dbridge::sync
