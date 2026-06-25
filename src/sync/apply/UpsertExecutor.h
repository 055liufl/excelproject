#pragma once
#include "dbridge/Types.h"
#include "dbridge/sync/SyncTypes.h"

#include <QHash>
#include <QSqlDatabase>
#include <QSqlQuery>

namespace dbridge::sync {

class UpsertExecutor {
   public:
    // Apply a batch of RowMutations inside an already-open transaction.
    // Per-row failures are collected into errors (if non-null), not thrown.
    // Returns false only on a fatal/unrecoverable error.
    bool apply(QSqlDatabase& db, const QList<RowMutation>& rows, QList<dbridge::RowError>* errors,
               QString* err);

    void clearPreparedCache();

   private:
    QHash<QString, QSqlQuery> cache_;  // keyed by "table:DoUpdate" or "table:DoNothing"

    QString buildUpsertSql(const QString& table, const QStringList& cols, const QStringList& pkCols,
                           UpsertMode mode);
    QString cacheKey(const QString& table, UpsertMode mode) const;
};

}  // namespace dbridge::sync
