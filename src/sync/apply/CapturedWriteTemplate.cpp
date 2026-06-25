#include "CapturedWriteTemplate.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// constructor
// ---------------------------------------------------------------------------

CapturedWriteTemplate::CapturedWriteTemplate(QSqlDatabase& wconn, sqlite3* h,
                                             AppliedVectorStore& av, RowWinnerStore& rw,
                                             TableStateStore& ts, ChangelogStore& clog,
                                             SessionRecorder& rec, SchemaGuard& guard,
                                             ChangesetApplier& applier, const QString& nodeId,
                                             qint64 streamEpoch, const QString& schemaFp,
                                             qint64 schemaVer)
    : wconn_(wconn),
      h_(h),
      av_(av),
      rw_(rw),
      ts_(ts),
      clog_(clog),
      rec_(rec),
      guard_(guard),
      applier_(applier),
      nodeId_(nodeId),
      streamEpoch_(streamEpoch),
      schemaFp_(schemaFp),
      schemaVer_(schemaVer) {
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

WriteResult CapturedWriteTemplate::execute(const WriteParams& params) {
    switch (params.kind) {
        case WriteKind::InboundChangeset:
            return branchA(params);
        case WriteKind::InboundSelectionPush:
        case WriteKind::LocalWrite:
            return branchBC(params);
    }
    WriteResult r;
    r.errorCode = QStringLiteral("UNKNOWN_KIND");
    return r;
}

// ---------------------------------------------------------------------------
// Branch A: InboundChangeset
// ---------------------------------------------------------------------------

WriteResult CapturedWriteTemplate::branchA(const WriteParams& p) {
    WriteResult result;

    WriteTxn txn(wconn_);
    QString err;
    if (!txn.begin(&err)) {
        result.errorCode = QStringLiteral("TXN_BEGIN");
        result.errorMsg = err;
        return result;
    }

    // 1. Check applied vector
    SeqCheckResult sc = av_.check(wconn_, p.origin, p.epoch, p.seq, &err);
    if (sc == SeqCheckResult::NoOp) {
        txn.rollback();
        result.ok = true;  // already applied – idempotent success
        return result;
    }
    if (sc == SeqCheckResult::Gap) {
        txn.rollback();
        result.errorCode = QStringLiteral("SEQ_GAP");
        result.errorMsg = QStringLiteral("gap for origin=%1 seq=%2").arg(p.origin).arg(p.seq);
        return result;
    }

    // 2. Verify schema
    if (!guard_.verifyPayload(p.schemaVer, p.schemaFp, &err)) {
        txn.rollback();
        result.errorCode = QStringLiteral("SCHEMA_MISMATCH");
        result.errorMsg = err;
        return result;
    }

    // 3. Apply changeset (no SessionRecorder – we store the raw blob)
    ApplyOptions opts;
    opts.authoritative = false;
    if (!applier_.apply(h_, wconn_, p.changesetBlob, p.origin, p.originRank, p.seq, rw_, opts,
                        &result.applyOutcome, &err)) {
        txn.rollback();
        result.errorCode = QStringLiteral("APPLY_FAILED");
        result.errorMsg = err;
        return result;
    }

    // 4. Advance applied vector
    if (!av_.advance(wconn_, p.origin, p.epoch, p.seq, &err)) {
        txn.rollback();
        result.errorCode = QStringLiteral("AV_ADVANCE");
        result.errorMsg = err;
        return result;
    }

    // 5. Store raw blob in changelog (appendForward)
    qint64 localSeq = 0;
    if (!clog_.appendForward(wconn_, p.origin, nodeId_, p.seq, p.epoch, p.schemaVer, p.schemaFp,
                             p.changesetBlob, &localSeq, &err)) {
        txn.rollback();
        result.errorCode = QStringLiteral("CLOG_FORWARD");
        result.errorMsg = err;
        return result;
    }

    if (!txn.commit(&err)) {
        result.errorCode = QStringLiteral("TXN_COMMIT");
        result.errorMsg = err;
        return result;
    }

    result.ok = true;
    result.localChangelogSeq = localSeq;
    return result;
}

// ---------------------------------------------------------------------------
// Branch B/C: InboundSelectionPush or LocalWrite
// ---------------------------------------------------------------------------

WriteResult CapturedWriteTemplate::branchBC(const WriteParams& p) {
    WriteResult result;
    QString err;

    const bool isInbound = (p.kind == WriteKind::InboundSelectionPush);

    WriteTxn txn(wconn_);
    if (!txn.begin(&err)) {
        result.errorCode = QStringLiteral("TXN_BEGIN");
        result.errorMsg = err;
        return result;
    }

    // Inbound: check push_chunk_progress idempotency
    if (isInbound && !p.pushId.isEmpty()) {
        QSqlQuery chk(wconn_);
        chk.prepare(
            QStringLiteral("SELECT status FROM __sync_push_chunk_progress "
                           "WHERE push_id = ? AND chunk_seq = ?"));
        chk.addBindValue(p.pushId);
        chk.addBindValue(p.chunkSeq);
        if (chk.exec() && chk.next()) {
            const QString st = chk.value(0).toString();
            if (st == QLatin1String("applied")) {
                txn.rollback();
                result.ok = true;  // idempotent skip
                return result;
            }
        }
    }

    // Inbound: schema guard
    if (isInbound) {
        if (!guard_.verifyPayload(p.schemaVer, p.schemaFp, &err)) {
            txn.rollback();
            result.errorCode = QStringLiteral("SCHEMA_MISMATCH");
            result.errorMsg = err;
            return result;
        }
    }

    // Begin fresh session capture
    if (!rec_.begin(h_, p.syncTables, &err)) {
        txn.rollback();
        result.errorCode = QStringLiteral("SESSION_BEGIN");
        result.errorMsg = err;
        return result;
    }

    // Execute row mutations
    for (const RowMutation& m : p.mutations) {
        if (!execMutation(m, &err)) {
            rec_.abort();
            txn.rollback();
            result.errorCode = QStringLiteral("MUTATION");
            result.errorMsg = err;
            return result;
        }
    }

    // Seal changeset into changelog
    qint64 localSeq = 0;
    qint64 parentSeq = 0;  // caller may enrich later
    qint64 originSeq = isInbound ? p.seq : 0;
    const QString kind = isInbound ? QStringLiteral("selection_push") : QStringLiteral("local");
    const QString origin = isInbound ? p.origin : nodeId_;
    const qint64 epoch = isInbound ? p.epoch : streamEpoch_;

    if (!rec_.sealInto(h_, clog_, wconn_, txn, origin, epoch, isInbound ? p.schemaVer : schemaVer_,
                       isInbound ? p.schemaFp : schemaFp_, parentSeq, originSeq, &localSeq, &err)) {
        txn.rollback();
        result.errorCode = QStringLiteral("SEAL");
        result.errorMsg = err;
        return result;
    }

    // Mark push chunk applied
    if (isInbound && !p.pushId.isEmpty()) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        QSqlQuery upsert(wconn_);
        upsert.prepare(
            QStringLiteral("INSERT INTO __sync_push_chunk_progress "
                           "(push_id, chunk_seq, status, checksum, applied_ms) "
                           "VALUES (?, ?, 'applied', '', ?) "
                           "ON CONFLICT(push_id, chunk_seq) DO UPDATE SET "
                           "  status = 'applied', applied_ms = excluded.applied_ms"));
        upsert.addBindValue(p.pushId);
        upsert.addBindValue(p.chunkSeq);
        upsert.addBindValue(nowMs);
        upsert.exec();
    }

    if (!txn.commit(&err)) {
        result.errorCode = QStringLiteral("TXN_COMMIT");
        result.errorMsg = err;
        return result;
    }

    result.ok = true;
    result.localChangelogSeq = localSeq;
    return result;
}

// ---------------------------------------------------------------------------
// private: execMutation
// ---------------------------------------------------------------------------

bool CapturedWriteTemplate::execMutation(const RowMutation& m, QString* err) {
    if (m.columns.isEmpty()) {
        if (err)
            *err = QStringLiteral("empty columns in mutation for table %1").arg(m.table);
        return false;
    }

    // Build "col1=?,col2=?,..." update list (all non-pk columns).
    QStringList updateClauses;
    for (int i = 0; i < m.columns.size(); i++) {
        if (!m.pkColumns.contains(m.columns[i]))
            updateClauses << QStringLiteral("%1=excluded.%1").arg(m.columns[i]);
    }

    const QString colList = m.columns.join(QStringLiteral(", "));
    QStringList phList;
    phList.reserve(m.columns.size());
    for (int i = 0; i < m.columns.size(); ++i)
        phList << QStringLiteral("?");
    const QString placeholders = phList.join(QStringLiteral(", "));
    QString sql;
    if (m.mode == UpsertMode::DoNothing || updateClauses.isEmpty()) {
        sql = QStringLiteral("INSERT OR IGNORE INTO %1 (%2) VALUES (%3)")
                  .arg(m.table, colList, placeholders);
    } else {
        const QString pkConflict = m.pkColumns.join(QStringLiteral(", "));
        sql = QStringLiteral(
                  "INSERT INTO %1 (%2) VALUES (%3) "
                  "ON CONFLICT(%4) DO UPDATE SET %5")
                  .arg(m.table, colList, placeholders, pkConflict,
                       updateClauses.join(QStringLiteral(", ")));
    }

    QSqlQuery q(wconn_);
    q.prepare(sql);
    for (const QVariant& v : m.values)
        q.addBindValue(v);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

}  // namespace dbridge::sync
