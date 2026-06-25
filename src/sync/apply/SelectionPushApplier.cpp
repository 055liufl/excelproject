#include "SelectionPushApplier.h"

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// constructor
// ---------------------------------------------------------------------------

SelectionPushApplier::SelectionPushApplier(CapturedWriteTemplate& tpl, UpsertExecutor& upsert)
    : tpl_(tpl), upsert_(upsert) {
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

bool SelectionPushApplier::applyChunk(QSqlDatabase& wconn, const QList<RowMutation>& rows,
                                      const QString& origin, qint64 epoch, qint64 seq,
                                      qint64 schemaVer, const QString& schemaFp, int originRank,
                                      const QString& pushId, int chunkSeq,
                                      const QStringList& syncTables,
                                      QList<dbridge::RowError>* errors, QString* err) {
    Q_UNUSED(wconn)    // wconn is owned by the CapturedWriteTemplate; passed for
                       // future API symmetry and potential per-call overrides.
    Q_UNUSED(upsert_)  // UpsertExecutor is held for callers that bypass the
                       // template; the template handles its own mutation path.

    WriteParams params;
    params.kind = WriteKind::InboundSelectionPush;
    params.origin = origin;
    params.epoch = epoch;
    params.seq = seq;
    params.schemaVer = schemaVer;
    params.schemaFp = schemaFp;
    params.originRank = originRank;
    params.pushId = pushId;
    params.chunkSeq = chunkSeq;
    params.mutations = rows;
    params.syncTables = syncTables;

    WriteResult result = tpl_.execute(params);

    if (!result.ok) {
        if (err) {
            *err = result.errorMsg.isEmpty() ? result.errorCode : result.errorMsg;
        }
        return false;
    }

    // The template executes all mutations atomically; per-row constraint errors
    // surfaced by branchBC are stored via RowWinnerStore / changeset logic and
    // do not produce RowError entries at this layer.  If the caller supplied an
    // errors list we leave it untouched on success.
    Q_UNUSED(errors)

    return true;
}

}  // namespace dbridge::sync
