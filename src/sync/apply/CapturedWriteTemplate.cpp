#include "CapturedWriteTemplate.h"

#include "dbridge/Errors.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>

#include "sql/SqlBuilder.h"
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
        // H-03 fix: gap means a predecessor seq is missing. Per design G-05 / plan S-01,
        // the artifact should remain in InboxLedger as 'seen' (pending) and be re-scanned
        // on the next tick; E_SYNC_GAP is only emitted after the gap timeout expires
        // (InboxLedger::stalePending). Returning a Gap-specific code (not a hard Error) lets
        // processArtifact skip markConsumed so the ledger stays 'seen'.
        result.errorCode = QStringLiteral(
            "GAP_PENDING");  // not a Errors.h error code — handled by processArtifact
        result.errorMsg = QStringLiteral("gap for origin=%1 seq=%2; keeping artifact pending")
                              .arg(p.origin)
                              .arg(p.seq);
        return result;
    }

    // 2. Verify schema
    if (!guard_.verifyPayload(p.schemaVer, p.schemaFp, &err)) {
        txn.rollback();
        result.errorCode = QLatin1String(err::E_SYNC_SCHEMA_MISMATCH);
        result.errorMsg = err;
        return result;
    }

    // 3. Apply changeset (no SessionRecorder – we store the raw blob)
    ApplyOptions opts;
    opts.authoritative = false;
    if (!applier_.apply(h_, wconn_, p.changesetBlob, p.origin, p.originRank, p.seq, rw_, opts,
                        p.syncTables, &result.applyOutcome, &err)) {
        txn.rollback();
        const QString lowerErr = err.toLower();
        if (lowerErr.contains(QLatin1String("foreign")) || lowerErr.contains(QLatin1String("fk")))
            result.errorCode = QLatin1String(err::E_SYNC_APPLY_FK);
        else
            result.errorCode = QLatin1String(err::E_SYNC_APPLY_CONSTRAINT);
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
    QList<TableMutation> muts = extractMutations(p.changesetBlob, p.syncTables);
    if (!muts.isEmpty()) {
        // M-03 fix: applyMutations() failure must roll back the entire apply transaction
        // so that changeset, applied_vector, and table_state remain in sync (apply三件套
        // must stay atomic). A stale table_state that cannot be corrected would silently
        // diverge checksums across nodes.
        if (!ts_.applyMutations(wconn_, muts, p.epoch, p.schemaFp, p.seq, &err)) {
            txn.rollback();
            result.errorCode = QStringLiteral("TABLE_STATE_UPDATE");
            result.errorMsg = err;
            return result;
        }
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
            QStringLiteral("SELECT status, checksum FROM __sync_push_chunk_progress "
                           "WHERE push_id = ? AND chunk_seq = ?"));
        chk.addBindValue(p.pushId);
        chk.addBindValue(p.chunkSeq);
        if (chk.exec() && chk.next()) {
            const QString st = chk.value(0).toString();
            if (st == QLatin1String("applied")) {
                // H-03 fix: verify checksum matches the already-applied chunk.
                // Identical checksum → idempotent no-op (safe to skip).
                // Different checksum → the payload is corrupt or mis-routed; quarantine.
                const QString storedCs = chk.value(1).toString();
                if (!storedCs.isEmpty() && !p.checksum.isEmpty() && storedCs != p.checksum) {
                    txn.rollback();
                    result.errorCode = QLatin1String(err::E_SYNC_PAYLOAD_CORRUPT);
                    result.errorMsg =
                        QStringLiteral(
                            "chunk %1 of push %2 was already applied with checksum %3 "
                            "but re-delivered with different checksum %4")
                            .arg(p.chunkSeq)
                            .arg(p.pushId, storedCs, p.checksum);
                    return result;
                }
                txn.rollback();
                result.ok = true;  // same checksum → idempotent skip
                return result;
            }
        }
    }

    // Inbound: schema guard
    if (isInbound) {
        if (!guard_.verifyPayload(p.schemaVer, p.schemaFp, &err)) {
            txn.rollback();
            result.errorCode = QLatin1String(err::E_SYNC_SCHEMA_MISMATCH);
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

    // H-05 fix: pre-scan row existence and old-hash BEFORE the UPSERT.
    // After UPSERT the old row is gone; reading "old" content post-write gives the new value.
    struct PreScan {
        bool rowExists = false;
        QByteArray beforeHash;  // empty when row didn't exist (INSERT case)
        QByteArray pkHash;
        QByteArray afterHash;
    };
    auto buildWhereParts = [](const RowMutation& m) {
        QStringList parts;
        for (const QString& pk : m.pkColumns)
            // H-1 fix: use quoteIdent to handle column names with embedded double-quotes.
            parts << detail::SqlBuilder::quoteIdent(pk) + QStringLiteral("=?");
        return parts;
    };
    auto bindPkValues = [](QSqlQuery& q, const RowMutation& m) {
        for (const QString& pk : m.pkColumns) {
            int idx = m.columns.indexOf(pk);
            if (idx >= 0)
                q.addBindValue(m.values[idx]);
        }
    };

    QList<PreScan> preScan;
    preScan.reserve(p.mutations.size());
    for (const RowMutation& m : p.mutations) {
        PreScan ps;
        // pkHash
        QByteArray pkMat;
        for (int i = 0; i < m.columns.size(); ++i) {
            if (m.pkColumns.contains(m.columns[i])) {
                pkMat.append(m.values[i].toString().toUtf8());
                pkMat.append('\0');
            }
        }
        ps.pkHash = QCryptographicHash::hash(pkMat, QCryptographicHash::Sha256).left(16);
        // M-02 fix: afterHash must use the same column-name-sorted QVariantMap format as
        // TableStateStore::rowHash() so that incremental mutations produce checksums
        // consistent with resetFromBaseline().  Build a QVariantMap keyed by column name
        // (QVariantMap is sorted by key automatically) and delegate to rowHash().
        {
            QVariantMap afterMap;
            for (int i = 0; i < m.columns.size(); ++i)
                afterMap.insert(m.columns[i], m.values[i]);
            ps.afterHash = TableStateStore::rowHash(afterMap);
        }
        // rowExists + beforeHash: query BEFORE UPSERT
        if (!m.pkColumns.isEmpty() && !m.table.isEmpty()) {
            QStringList wp = buildWhereParts(m);
            QSqlQuery existQ(wconn_);
            // H-2 fix: use quoteIdent for table name.
            existQ.prepare(
                QStringLiteral("SELECT * FROM %1 WHERE %2 LIMIT 1")
                    .arg(detail::SqlBuilder::quoteIdent(m.table), wp.join(QLatin1String(" AND "))));
            bindPkValues(existQ, m);
            if (existQ.exec() && existQ.next()) {
                ps.rowExists = true;
                // M-02 fix: beforeHash must also use column-name-sorted QVariantMap so it
                // matches the format produced by extractMutations (changeset path) and
                // resetFromBaseline (full scan path). Build QVariantMap from QSqlRecord.
                QVariantMap beforeMap;
                QSqlRecord rec = existQ.record();
                for (int ci = 0; ci < rec.count(); ++ci)
                    beforeMap.insert(rec.fieldName(ci), existQ.value(ci));
                ps.beforeHash = TableStateStore::rowHash(beforeMap);
            }
        }
        preScan.append(ps);
    }

    // Execute row mutations via UpsertExecutor (I-08).
    UpsertExecutor upsertEx;
    QList<dbridge::RowError> rowErrors;
    if (!upsertEx.apply(wconn_, p.mutations, &rowErrors, &err)) {
        rec_.abort();
        txn.rollback();
        result.errorCode = QStringLiteral("E_DB_UPSERT");
        result.errorMsg = err;
        return result;
    }
    // C-09 fix: any row-level errors (FK violation, constraint) must abort the whole chunk.
    // Committing with partial row errors would let the receiver ACK a broken chunk.
    if (!rowErrors.isEmpty()) {
        rec_.abort();
        txn.rollback();
        result.errorCode = rowErrors.first().code.contains(QLatin1String("FK")) ||
                                   rowErrors.first().message.contains(QLatin1String("foreign"))
                               ? QLatin1String(err::E_SYNC_APPLY_FK)
                               : QLatin1String(err::E_SYNC_APPLY_CONSTRAINT);
        result.errorMsg = QStringLiteral("%1 row(s) failed; first: %2")
                              .arg(rowErrors.size())
                              .arg(rowErrors.first().message);
        return result;
    }

    // Seal changeset into changelog
    qint64 localSeq = 0;
    qint64 parentSeq = 0;
    qint64 originSeq = isInbound ? p.seq : p.seq;
    const QString origin = isInbound ? p.origin : nodeId_;
    const qint64 epoch = isInbound ? p.epoch : streamEpoch_;
    if (!isInbound && originSeq <= 0) {
        rec_.abort();
        txn.rollback();
        result.errorCode = QLatin1String(err::E_SYNC_INIT);
        result.errorMsg = QStringLiteral("local origin_seq must be allocated before branch C seal");
        return result;
    }

    // H-01 fix: pass p.pushId so selection-push changesets (InboundSelectionPush) have their
    // push_id recorded in the changelog, enabling the broadcast barrier to filter by specific push.
    if (!rec_.sealInto(h_, clog_, wconn_, txn, origin, epoch, isInbound ? p.schemaVer : schemaVer_,
                       isInbound ? p.schemaFp : schemaFp_, parentSeq, originSeq, &localSeq, &err,
                       /*pushId=*/p.pushId)) {
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
                           "VALUES (?, ?, 'applied', ?, ?) "
                           "ON CONFLICT(push_id, chunk_seq) DO UPDATE SET "
                           "  status = 'applied', checksum = excluded.checksum, "
                           "  applied_ms = excluded.applied_ms"));
        upsert.addBindValue(p.pushId);
        upsert.addBindValue(p.chunkSeq);
        upsert.addBindValue(p.checksum);
        upsert.addBindValue(nowMs);
        if (!upsert.exec()) {
            txn.rollback();
            result.errorCode = QLatin1String(err::E_SYNC_TRANSPORT);
            result.errorMsg = upsert.lastError().text();
            return result;
        }

        // H-01 fix: after marking this chunk applied, check whether ALL chunks for this push
        // are now applied. If so, promote push_progress.status to 'done'.
        // This is critical for center nodes which receive every chunk from the originator
        // but never send push-chunk ACKs back to themselves — without this, the push barrier
        // in broadcastToPeer (status != 'done') permanently blocks downstream broadcast.
        {
            QSqlQuery doneQ(wconn_);
            doneQ.prepare(
                QStringLiteral("SELECT pp.total_chunks, "
                               "  (SELECT COUNT(*) FROM __sync_push_chunk_progress "
                               "   WHERE push_id = pp.push_id AND status = 'applied') "
                               "  AS applied_chunks "
                               "FROM __sync_push_progress pp "
                               "WHERE pp.push_id = ? AND pp.status != 'done' "
                               "  AND pp.status != 'failed'"));
            doneQ.addBindValue(p.pushId);
            if (doneQ.exec() && doneQ.next()) {
                const int total = doneQ.value(0).toInt();
                const int applied = doneQ.value(1).toInt();
                if (total > 0 && applied >= total) {
                    QSqlQuery markDone(wconn_);
                    markDone.prepare(
                        QStringLiteral("UPDATE __sync_push_progress "
                                       "SET status = 'done', updated_ms = ? "
                                       "WHERE push_id = ?"));
                    markDone.addBindValue(nowMs);
                    markDone.addBindValue(p.pushId);
                    if (!markDone.exec()) {
                        txn.rollback();
                        result.errorCode = QLatin1String(err::E_SYNC_TRANSPORT);
                        result.errorMsg = markDone.lastError().text();
                        return result;
                    }
                }
            }
        }
    }

    // Update table_state from RowMutations using pre-scanned (correct) old-row info (H-05).
    // M-01 fix: skip table_state update for DoNothing (INSERT OR IGNORE) mutations when the
    // row already existed before the UPSERT. In that case the INSERT is a no-op — SQLite does
    // not modify the row — so the actual session changeset contains no entry for it. Adding an
    // afterHash for a no-op write would pollute the checksum and cause divergence with peers
    // that compute table_state from the real changeset (branchA / extractMutations path).
    QList<TableMutation> tmuts;
    tmuts.reserve(p.mutations.size());
    for (int i = 0; i < p.mutations.size(); ++i) {
        const RowMutation& m = p.mutations[i];
        const PreScan& ps = preScan[i];
        if (m.mode == UpsertMode::DoNothing && ps.rowExists) {
            // INSERT OR IGNORE was a no-op: row already existed and was not changed.
            // Do not update table_state for this row.
            continue;
        }
        TableMutation tm;
        tm.table = m.table;
        tm.isInsert = !ps.rowExists;
        tm.isDelete = false;
        tm.pkHash = QString::fromLatin1(ps.pkHash.toHex());
        tm.afterHash = ps.afterHash;
        tm.beforeHash = ps.beforeHash;  // correctly from pre-UPSERT scan
        tmuts.append(tm);
    }

    if (!tmuts.isEmpty()) {
        // M-03 fix: applyMutations() failure must roll back the entire write transaction
        // so that the upserted rows, changelog entry, and table_state remain atomic.
        if (!ts_.applyMutations(wconn_, tmuts, isInbound ? p.epoch : streamEpoch_,
                                isInbound ? p.schemaFp : schemaFp_, originSeq, &err)) {
            txn.rollback();
            result.errorCode = QStringLiteral("TABLE_STATE_UPDATE");
            result.errorMsg = err;
            return result;
        }
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

QList<TableMutation> CapturedWriteTemplate::extractMutations(const QByteArray& changeset,
                                                             const QStringList& syncTables) {
    QList<TableMutation> muts;
    if (changeset.isEmpty())
        return muts;

    sqlite3_changeset_iter* iter = nullptr;
    if (sqlite3changeset_start(
            &iter, changeset.size(),
            const_cast<void*>(static_cast<const void*>(changeset.constData()))) != SQLITE_OK)
        return muts;

    // M-03 fix: cache column-name lists per table so that row hashes use the same
    // "key=value\n sorted by column name" format as TableStateStore::rowHash().
    // This ensures beforeHash / afterHash computed here are directly comparable to
    // the checksums produced by resetFromBaseline(), preventing checksum divergence
    // after a baseline reset followed by incremental UPDATE/DELETE.
    QMap<QString, QStringList> colNameCache;

    auto getColNames = [&](const QString& tableName, int nCol) -> QStringList {
        auto it = colNameCache.find(tableName);
        if (it != colNameCache.end())
            return it.value();
        QStringList names;
        QSqlQuery ti(wconn_);
        ti.prepare(QStringLiteral("PRAGMA table_info(\"%1\")")
                       .arg(QString(tableName).replace(QLatin1Char('"'), QLatin1String("\"\""))));
        if (ti.exec()) {
            QMap<int, QString> cidMap;
            while (ti.next())
                cidMap.insert(ti.value(0).toInt(), ti.value(1).toString());
            for (int i = 0; i < nCol; ++i)
                names.append(cidMap.value(i, QStringLiteral("_col_%1").arg(i)));
        } else {
            // Fallback: use positional names so the cache is not empty.
            for (int i = 0; i < nCol; ++i)
                names.append(QStringLiteral("_col_%1").arg(i));
        }
        colNameCache.insert(tableName, names);
        return names;
    };

    while (sqlite3changeset_next(iter) == SQLITE_ROW) {
        const char* tbl = nullptr;
        int nCol = 0, op = 0, indirect = 0;
        sqlite3changeset_op(iter, &tbl, &nCol, &op, &indirect);

        unsigned char* pkMask = nullptr;
        sqlite3changeset_pk(iter, &pkMask, nullptr);

        const QString tableName = QString::fromUtf8(tbl ? tbl : "");

        // H-01 fix: skip tables rejected by the allow-list so __sync_* meta tables and
        // non-sync tables are never written to __sync_table_state.
        if (!ChangesetApplier::isAllowedSyncTable(tableName, syncTables))
            continue;

        const QStringList colNames = getColNames(tableName, nCol);

        // M-03 fix: build a QVariantMap keyed by column name (sorted automatically by QMap),
        // then delegate to TableStateStore::rowHash() for a consistent hash format.
        auto rowHashFromIter = [&](bool useNew) -> QByteArray {
            QVariantMap rowMap;
            for (int i = 0; i < nCol; i++) {
                sqlite3_value* val = nullptr;
                if (useNew)
                    sqlite3changeset_new(iter, i, &val);
                else
                    sqlite3changeset_old(iter, i, &val);
                const QString colName =
                    (i < colNames.size()) ? colNames[i] : QStringLiteral("_col_%1").arg(i);
                if (!val) {
                    rowMap.insert(colName, QVariant());
                    continue;
                }
                const int vt = sqlite3_value_type(val);
                if (vt == SQLITE_TEXT) {
                    const char* txt = reinterpret_cast<const char*>(sqlite3_value_text(val));
                    rowMap.insert(colName, QString::fromUtf8(txt ? txt : ""));
                } else if (vt == SQLITE_INTEGER) {
                    rowMap.insert(colName,
                                  QVariant(static_cast<qlonglong>(sqlite3_value_int64(val))));
                } else if (vt == SQLITE_FLOAT) {
                    rowMap.insert(colName, sqlite3_value_double(val));
                } else if (vt == SQLITE_BLOB) {
                    const void* b = sqlite3_value_blob(val);
                    const int bl = sqlite3_value_bytes(val);
                    rowMap.insert(colName, (b && bl > 0) ? QVariant(QByteArray(
                                                               static_cast<const char*>(b), bl))
                                                         : QVariant(QByteArray()));
                } else {
                    rowMap.insert(colName, QVariant());
                }
            }
            return TableStateStore::rowHash(rowMap);
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
                mat = rowHashFromIter(useNew);  // fallback: hash all cols
            QByteArray h = QCryptographicHash::hash(mat, QCryptographicHash::Sha256).left(16);
            return QString::fromLatin1(h.toHex());
        };

        TableMutation tm;
        tm.table = tableName;

        if (op == SQLITE_INSERT) {
            tm.pkHash = pkHashStr(true);
            tm.afterHash = rowHashFromIter(true);
            tm.isInsert = true;
            tm.isDelete = false;
        } else if (op == SQLITE_DELETE) {
            tm.pkHash = pkHashStr(false);
            tm.beforeHash = rowHashFromIter(false);
            tm.isInsert = false;
            tm.isDelete = true;
        } else if (op == SQLITE_UPDATE) {
            tm.pkHash = pkHashStr(false);  // PK must not change across UPDATE
            tm.beforeHash = rowHashFromIter(false);
            tm.afterHash = rowHashFromIter(true);
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

// L-02 fix: execMutation() was dead code with unquoted identifiers. Removed.

}  // namespace dbridge::sync
