#pragma once
#include "dbridge/Types.h"
#include "dbridge/sync/SyncTypes.h"

#include "CapturedWriteTemplate.h"
#include "UpsertExecutor.h"

namespace dbridge::sync {

// Applies one chunk of an inbound SelectionPush via CapturedWriteTemplate (Branch B).
class SelectionPushApplier {
   public:
    SelectionPushApplier(CapturedWriteTemplate& tpl, UpsertExecutor& upsert);

    // Apply one chunk. Internally calls tpl_.execute() with WriteKind::InboundSelectionPush.
    bool applyChunk(QSqlDatabase& wconn, const QList<RowMutation>& rows, const QString& origin,
                    qint64 epoch, qint64 seq, qint64 schemaVer, const QString& schemaFp,
                    int originRank, const QString& pushId, int chunkSeq,
                    const QStringList& syncTables, QList<dbridge::RowError>* errors, QString* err);

   private:
    CapturedWriteTemplate& tpl_;
    UpsertExecutor& upsert_;
};

}  // namespace dbridge::sync
