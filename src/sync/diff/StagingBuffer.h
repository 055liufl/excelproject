#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

#include "../apply/UpsertExecutor.h"

namespace dbridge::sync {

// In-memory staging area for comparison session edits.
// save() flushes to DB via UpsertExecutor inside a WriteTxn.
class StagingBuffer {
   public:
    void stage(const QString& table, const QString& pk, const QVariantMap& row);
    void unstage(const QString& table, const QString& pk);

    // Flush all staged rows to wconn via UpsertExecutor.
    // pkCols: primary key column names (used to build RowMutation).
    bool save(QSqlDatabase& wconn, UpsertExecutor& upsert, const QStringList& pkCols, QString* err);

    void discard();
    bool isEmpty() const;

   private:
    struct StagedRow {
        QString table;
        QString pk;
        QVariantMap row;
    };
    QList<StagedRow> staged_;
};

}  // namespace dbridge::sync
