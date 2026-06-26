#include "CapturedWriteTemplate.h"

#include "dbridge/Errors.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>

#include <sqlite3.h>

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
// Branch A: InboundChangeset (I-07)
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
        result.errorCode = QLatin1String(err::E_SYNC_GAP);
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
                        p.syncTables, &result.applyOutcome, &err)) {
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

    // 5a. Update table_state from changeset mutations (I-07)
    QList<TableMutation> muts = extractMutations(p.changesetBlob);
    if (!muts.isEmpty()) {
        // Non-fatal: log failure but continue — table_state is a soft-accounting layer.
        if (!ts_.applyMutations(wconn_, muts, p.epoch, p.schemaFp, p.seq, &err))
            err.clear();  // swallow; will be recomputed on next baseline reset if needed
    }

    // 5b. Store raw blob in changelog (appendForward)
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
    result.tableMutations = muts;
    return result;
}

// ---------------------------------------------------------------------------
// Branch B/C: InboundSelectionPush or LocalWrite (I-08)
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

    // Execute row mutations via UpsertExecutor (I-08: replaces hand-rolled execMutation loop).
    UpsertExecutor upsertEx;
    QList<dbridge::RowError> rowErrors;
    if (!upsertEx.apply(wconn_, p.mutations, &rowErrors, &err)) {
        rec_.abort();
        txn.rollback();
        result.errorCode = QStringLiteral("E_DB_UPSERT");
        result.errorMsg = err;
        return result;
    }

    // Seal changeset into changelog
    qint64 localSeq = 0;
    qint64 parentSeq = 0;  // caller may enrich later
    qint64 originSeq = isInbound ? p.seq : 0;
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

    // Update table_state from RowMutations (I-08).
    QList<TableMutation> tmuts;
    tmuts.reserve(p.mutations.size());
    for (const RowMutation& m : p.mutations) {
        TableMutation tm;
        tm.table = m.table;
        // J-16: Determine insert vs update by querying whether the row exists before UPSERT.
        // For DoNothing mode, treat as potential insert (safe: no-op if row exists).
        // For DoUpdate mode, check if PK already exists to distinguish insert from update.
        bool rowExists = false;
        if (!m.pkColumns.isEmpty() && !m.table.isEmpty()) {
            QStringList whereParts;
            for (const QString& pk : m.pkColumns)
                whereParts << QStringLiteral("\"%1\"=?").arg(pk);
            QSqlQuery existQ(wconn_);
            existQ.prepare(QStringLiteral("SELECT 1 FROM \"%1\" WHERE %2 LIMIT 1")
                               .arg(m.table, whereParts.join(QLatin1String(" AND "))));
            for (const QString& pk : m.pkColumns) {
                int idx = m.columns.indexOf(pk);
                if (idx >= 0)
                    existQ.addBindValue(m.values[idx]);
            }
            rowExists = existQ.exec() && existQ.next();
        }
        tm.isInsert = !rowExists;
        tm.isDelete = false;

        // Build pkHash from PK columns.
        QByteArray pkMat;
        for (int i = 0; i < m.columns.size(); ++i) {
            if (m.pkColumns.contains(m.columns[i])) {
                pkMat.append(m.values[i].toString().toUtf8());
                pkMat.append('\0');
            }
        }
        QByteArray pkH = QCryptographicHash::hash(pkMat, QCryptographicHash::Sha256).left(16);
        tm.pkHash = QString::fromLatin1(pkH.toHex());

        // Content hash from all values.
        QByteArray contentMat;
        for (const QVariant& v : m.values) {
            contentMat.append(v.toString().toUtf8());
            contentMat.append('\0');
        }
        tm.afterHash = QCryptographicHash::hash(contentMat, QCryptographicHash::Sha256).left(16);

        // For update: fetch old row hash to enable correct checksum delta.
        if (rowExists && !m.pkColumns.isEmpty() && !tm.isInsert) {
            QStringList whereParts;
            for (const QString& pk : m.pkColumns)
                whereParts << QStringLiteral("\"%1\"=?").arg(pk);
            QSqlQuery oldQ(wconn_);
            oldQ.prepare(QStringLiteral("SELECT * FROM \"%1\" WHERE %2 LIMIT 1")
                             .arg(m.table, whereParts.join(QLatin1String(" AND "))));
            for (const QString& pk : m.pkColumns) {
                int idx = m.columns.indexOf(pk);
                if (idx >= 0)
                    oldQ.addBindValue(m.values[idx]);
            }
            if (oldQ.exec() && oldQ.next()) {
                QByteArray oldMat;
                QSqlRecord rec = oldQ.record();
                for (int ci = 0; ci < rec.count(); ++ci) {
                    oldMat.append(oldQ.value(ci).toString().toUtf8());
                    oldMat.append('\0');
                }
                tm.beforeHash =
                    QCryptographicHash::hash(oldMat, QCryptographicHash::Sha256).left(16);
            }
        }
        tmuts.append(tm);
    }

    if (!tmuts.isEmpty()) {
        // Non-fatal: log swallowed; table_state can be rebuilt from baseline.
        ts_.applyMutations(wconn_, tmuts, isInbound ? p.epoch : streamEpoch_,
                           isInbound ? p.schemaFp : schemaFp_, isInbound ? p.seq : 0, &err);
        err.clear();
    }

    if (!txn.commit(&err)) {
        result.errorCode = QStringLiteral("TXN_COMMIT");
        result.errorMsg = err;
        return result;
    }

    result.ok = true;
    result.localChangelogSeq = localSeq;
    result.tableMutations = tmuts;
    return result;
}

// ---------------------------------------------------------------------------
// private: extractMutations — parse changeset blob into TableMutation list (I-07)
// ---------------------------------------------------------------------------

QList<TableMutation> CapturedWriteTemplate::extractMutations(const QByteArray& changeset) {
    QList<TableMutation> muts;
    if (changeset.isEmpty())
        return muts;

    sqlite3_changeset_iter* iter = nullptr;
    if (sqlite3changeset_start(
            &iter, changeset.size(),
            const_cast<void*>(static_cast<const void*>(changeset.constData()))) != SQLITE_OK)
        return muts;

    while (sqlite3changeset_next(iter) == SQLITE_ROW) {
        const char* tbl = nullptr;
        int nCol = 0, op = 0, indirect = 0;
        sqlite3changeset_op(iter, &tbl, &nCol, &op, &indirect);

        unsigned char* pkMask = nullptr;
        sqlite3changeset_pk(iter, &pkMask, nullptr);

        // Build pkHash and content hashes from column values.
        auto hashSlice = [&](bool useNew) -> QByteArray {
            QByteArray mat;
            for (int i = 0; i < nCol; i++) {
                sqlite3_value* val = nullptr;
                if (useNew)
                    sqlite3changeset_new(iter, i, &val);
                else
                    sqlite3changeset_old(iter, i, &val);
                if (!val) {
                    mat.append('\0');
                    continue;
                }
                const int vt = sqlite3_value_type(val);
                if (vt == SQLITE_TEXT) {
                    const char* txt = reinterpret_cast<const char*>(sqlite3_value_text(val));
                    mat.append(txt ? txt : "");
                } else if (vt == SQLITE_INTEGER) {
                    mat.append(QByteArray::number(static_cast<qint64>(sqlite3_value_int64(val))));
                } else if (vt == SQLITE_BLOB) {
                    const void* b = sqlite3_value_blob(val);
                    const int bl = sqlite3_value_bytes(val);
                    if (b && bl > 0)
                        mat.append(static_cast<const char*>(b), bl);
                }
                mat.append('\0');
            }
            return QCryptographicHash::hash(mat, QCryptographicHash::Sha256).left(16);
        };

        auto pkHashStr = [&](bool useNew) -> QString {
            QByteArray mat;
            for (int i = 0; i < nCol; i++) {
                if (!pkMask || !pkMask[i])
                    continue;
                sqlite3_value* val = nullptr;
                if (useNew)
                    sqlite3changeset_new(iter, i, &val);
                else
                    sqlite3changeset_old(iter, i, &val);
                if (!val) {
                    mat.append('\0');
                    continue;
                }
                const int vt = sqlite3_value_type(val);
                if (vt == SQLITE_TEXT) {
                    const char* txt = reinterpret_cast<const char*>(sqlite3_value_text(val));
                    mat.append(txt ? txt : "");
                } else if (vt == SQLITE_INTEGER) {
                    mat.append(QByteArray::number(static_cast<qint64>(sqlite3_value_int64(val))));
                } else if (vt == SQLITE_BLOB) {
                    const void* b = sqlite3_value_blob(val);
                    const int bl = sqlite3_value_bytes(val);
                    if (b && bl > 0)
                        mat.append(static_cast<const char*>(b), bl);
                }
                mat.append('\0');
            }
            if (mat.isEmpty())
                mat = hashSlice(useNew);  // fallback: hash all cols
            QByteArray h = QCryptographicHash::hash(mat, QCryptographicHash::Sha256).left(16);
            return QString::fromLatin1(h.toHex());
        };

        TableMutation tm;
        tm.table = QString::fromUtf8(tbl ? tbl : "");

        if (op == SQLITE_INSERT) {
            tm.pkHash = pkHashStr(true);
            tm.afterHash = hashSlice(true);
            tm.isInsert = true;
            tm.isDelete = false;
        } else if (op == SQLITE_DELETE) {
            tm.pkHash = pkHashStr(false);
            tm.beforeHash = hashSlice(false);
            tm.isInsert = false;
            tm.isDelete = true;
        } else if (op == SQLITE_UPDATE) {
            tm.pkHash = pkHashStr(false);  // PK must not change across UPDATE
            tm.beforeHash = hashSlice(false);
            tm.afterHash = hashSlice(true);
            tm.isInsert = false;
            tm.isDelete = false;
        } else {
            continue;
        }
        muts.append(tm);
    }
    sqlite3changeset_finalize(iter);
    return muts;
}

// ---------------------------------------------------------------------------
// private: execMutation (kept for completeness; branchBC now uses UpsertExecutor)
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
