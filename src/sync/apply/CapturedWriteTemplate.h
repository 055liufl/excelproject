#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QByteArray>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "../WriteTxn.h"
#include "../capture/ChangelogStore.h"
#include "../capture/SessionRecorder.h"
#include "../schema/SchemaGuard.h"
#include "../schema/TableStateStore.h"
#include "AppliedVectorStore.h"
#include "ChangesetApplier.h"
#include "RowWinnerStore.h"
#include "UpsertExecutor.h"
#include <sqlite3.h>

namespace dbridge::sync {

enum class WriteKind { InboundChangeset, InboundSelectionPush, LocalWrite };

struct WriteParams {
    WriteKind kind = WriteKind::LocalWrite;

    // InboundChangeset / InboundSelectionPush:
    QString origin;
    qint64 epoch = 0;
    qint64 seq = 0;
    qint64 schemaVer = 0;
    QString schemaFp;
    int originRank = 0;

    // SelectionPush only:
    QString pushId;
    int chunkSeq = 0;
    QString checksum;

    // Branch A (InboundChangeset):
    QByteArray changesetBlob;

    // Branch B/C (SelectionPush / LocalWrite):
    QList<RowMutation> mutations;

    // Tables to track with SessionRecorder (B/C only).
    QStringList syncTables;
};

struct WriteResult {
    bool ok = false;
    qint64 localChangelogSeq = 0;
    QList<TableMutation> tableMutations;
    QString errorCode;
    QString errorMsg;
    ApplyOutcome applyOutcome;     // Branch A only
    QString tableStateStaleSince;  // M-04: non-empty when table_state update failed (non-fatal)
};

// Three-branch write template implementing G-03.
// All writes go through execute() which routes to branchA / branchBC.
class CapturedWriteTemplate {
   public:
    CapturedWriteTemplate(QSqlDatabase& wconn, sqlite3* h, AppliedVectorStore& av,
                          RowWinnerStore& rw, TableStateStore& ts, ChangelogStore& clog,
                          SessionRecorder& rec, SchemaGuard& guard, ChangesetApplier& applier,
                          const QString& nodeId, qint64 streamEpoch, const QString& schemaFp,
                          qint64 schemaVer);

    WriteResult execute(const WriteParams& params);

   private:
    WriteResult branchA(const WriteParams& p);   // InboundChangeset
    WriteResult branchBC(const WriteParams& p);  // InboundSelectionPush or LocalWrite

    // L-02 fix: execMutation() removed (dead code with unquoted identifiers).

    // Parse a changeset blob into a list of TableMutations for table_state accounting (I-07).
    // H-01 fix: syncTables is the same allow-list as filterCb; tables not in the list
    // (and __sync_* tables) are skipped so __sync_table_state is not polluted.
    QList<TableMutation> extractMutations(const QByteArray& changeset,
                                          const QStringList& syncTables);

    QSqlDatabase& wconn_;
    sqlite3* h_;
    AppliedVectorStore& av_;
    RowWinnerStore& rw_;
    TableStateStore& ts_;
    ChangelogStore& clog_;
    SessionRecorder& rec_;
    SchemaGuard& guard_;
    ChangesetApplier& applier_;
    QString nodeId_;
    qint64 streamEpoch_;
    QString schemaFp_;
    qint64 schemaVer_;
};

}  // namespace dbridge::sync
