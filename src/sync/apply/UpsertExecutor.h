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
    // M-04 fix: cache is now keyed by the full SQL string (not "table:mode") so different
    // column sets always produce distinct entries.
    QHash<QString, QSqlQuery> cache_;

    QString buildUpsertSql(const QString& table, const QStringList& cols, const QStringList& pkCols,
                           UpsertMode mode);
};

}  // namespace dbridge::sync
