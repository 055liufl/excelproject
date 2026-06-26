#include "sync/baseline/BaselineManager.h"

#include "dbridge/Errors.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

#include "sql/SqlBuilder.h"
#include "sync/WriteTxn.h"

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// serializeTables
// ---------------------------------------------------------------------------
bool BaselineManager::serializeTables(QSqlDatabase& rconn, const QStringList& tables,
                                      QByteArray* out, qint64* maxSeq, QString* err) {
    QByteArray raw;
    QDataStream ds(&raw, QIODevice::WriteOnly);
    ds.setVersion(QDataStream::Qt_5_12);

    ds << static_cast<quint32>(tables.size());

    for (const QString& table : tables) {
        // M-02 fix: use quoteIdent() to safely escape table names with embedded double-quotes.
        const QString quotedTable = detail::SqlBuilder::quoteIdent(table);

        // Count rows first so we can write rowCount before row data.
        QSqlQuery cntQ(rconn);
        cntQ.prepare(QStringLiteral("SELECT COUNT(*) FROM %1").arg(quotedTable));
        if (!cntQ.exec() || !cntQ.next()) {
            if (err)
                *err = QStringLiteral("count failed on %1: %2").arg(table, cntQ.lastError().text());
            return false;
        }
        const quint64 rowCount = static_cast<quint64>(cntQ.value(0).toLongLong());

        ds << table;
        ds << rowCount;

        QSqlQuery rowQ(rconn);
        rowQ.prepare(QStringLiteral("SELECT * FROM %1").arg(quotedTable));
        if (!rowQ.exec()) {
            if (err)
                *err =
                    QStringLiteral("select failed on %1: %2").arg(table, rowQ.lastError().text());
            return false;
        }

        const QSqlRecord rec = rowQ.record();
        const int colCount = rec.count();

        quint64 written = 0;
        while (rowQ.next()) {
            QVariantMap rowMap;
            for (int c = 0; c < colCount; ++c) {
                rowMap.insert(rec.fieldName(c), rowQ.value(c));
            }
            ds << rowMap;
            ++written;
        }

        if (written != rowCount) {
            if (err)
                *err = QStringLiteral("row count mismatch for %1: expected %2 got %3")
                           .arg(table)
                           .arg(rowCount)
                           .arg(written);
            return false;
        }
    }

    // Max local_seq from changelog (diagnostic).
    {
        QSqlQuery seqQ(rconn);
        seqQ.prepare(QStringLiteral("SELECT MAX(local_seq) FROM __sync_changelog"));
        if (seqQ.exec() && seqQ.next() && !seqQ.value(0).isNull()) {
            *maxSeq = seqQ.value(0).toLongLong();
        } else {
            *maxSeq = 0;
        }
    }

    *out = qCompress(raw, 6);
    return true;
}

// C-03 fix: query the applied_vector (not changelog MAX) so the baseline carries authoritative
// applied sequences per (origin, stream_epoch) rather than the highest-seen changeset seq.
// Using the applied_vector prevents a race where a changeset arrived but was not yet applied.
static QVector<BaselineOriginCut> queryOriginCuts(QSqlDatabase& rconn) {
    QVector<BaselineOriginCut> result;
    QSqlQuery q(rconn);
    if (!q.exec(
            QStringLiteral("SELECT origin, stream_epoch, applied_seq FROM __sync_applied_vector")))
        return result;
    while (q.next()) {
        BaselineOriginCut cut;
        cut.origin = q.value(0).toString();
        cut.streamEpoch = q.value(1).toLongLong();
        cut.appliedSeq = q.value(2).toLongLong();
        result.append(cut);
    }
    return result;
}

// ---------------------------------------------------------------------------
// deserializeAndApply
// ---------------------------------------------------------------------------
bool BaselineManager::deserializeAndApply(QSqlDatabase& wconn, const QByteArray& data,
                                          QStringList* tables, QString* err) {
    const QByteArray raw = qUncompress(data);
    if (raw.isEmpty()) {
        if (err)
            *err = QStringLiteral("decompression failed or empty payload");
        return false;
    }

    QDataStream ds(raw);
    ds.setVersion(QDataStream::Qt_5_12);

    quint32 tableCount = 0;
    ds >> tableCount;
    if (ds.status() != QDataStream::Ok) {
        if (err)
            *err = QStringLiteral("corrupt baseline: failed to read table count");
        return false;
    }

    tables->clear();

    for (quint32 t = 0; t < tableCount; ++t) {
        QString tableName;
        quint64 rowCount = 0;
        ds >> tableName >> rowCount;
        if (ds.status() != QDataStream::Ok) {
            if (err)
                *err = QStringLiteral("corrupt baseline: failed to read table header at index %1")
                           .arg(t);
            return false;
        }

        tables->append(tableName);

        // DELETE existing rows.
        {
            QSqlQuery delQ(wconn);
            delQ.prepare(QStringLiteral("DELETE FROM ") +
                         detail::SqlBuilder::quoteIdent(tableName));
            if (!delQ.exec()) {
                if (err)
                    *err = QStringLiteral("delete failed on %1: %2")
                               .arg(tableName, delQ.lastError().text());
                return false;
            }
        }

        for (quint64 r = 0; r < rowCount; ++r) {
            QVariantMap rowMap;
            ds >> rowMap;
            if (ds.status() != QDataStream::Ok) {
                if (err)
                    *err = QStringLiteral("corrupt baseline: row %1 in table %2")
                               .arg(r)
                               .arg(tableName);
                return false;
            }

            if (rowMap.isEmpty())
                continue;

            // M-05 fix: plain INSERT (not OR REPLACE). The DELETE above already cleared the
            // table, so no PK conflicts exist. INSERT OR REPLACE triggers DELETE+INSERT
            // semantics which can fire FK cascade deletes on child tables unexpectedly.
            // M-11 fix: quote table and column identifiers (escapes embedded double-quotes).
            QStringList cols, placeholders;
            for (auto it = rowMap.cbegin(); it != rowMap.cend(); ++it) {
                cols << detail::SqlBuilder::quoteIdent(it.key());
                placeholders << QStringLiteral("?");
            }

            const QString sql =
                QStringLiteral("INSERT INTO %1 (%2) VALUES (%3)")
                    .arg(detail::SqlBuilder::quoteIdent(tableName), cols.join(QLatin1Char(',')),
                         placeholders.join(QLatin1Char(',')));

            QSqlQuery insQ(wconn);
            insQ.prepare(sql);
            int idx = 0;
            for (auto it = rowMap.cbegin(); it != rowMap.cend(); ++it, ++idx) {
                insQ.bindValue(idx, it.value());
            }
            if (!insQ.exec()) {
                if (err)
                    *err = QStringLiteral("insert failed on %1 row %2: %3")
                               .arg(tableName)
                               .arg(r)
                               .arg(insQ.lastError().text());
                return false;
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// exportBaseline
// ---------------------------------------------------------------------------
bool BaselineManager::exportBaseline(QSqlDatabase& rconn, const QStringList& tables,
                                     BaselineArtifact* out, QString* err,
                                     const QString& localOrigin, qint64 localEpoch,
                                     qint64 localOriginSeq) {
    qint64 maxSeq = 0;
    QByteArray data;
    if (!serializeTables(rconn, tables, &data, &maxSeq, err)) {
        // I-20: map inner failure to E_SYNC_BASELINE_FAILED error code.
        if (err && !err->startsWith(QLatin1String("E_SYNC")))
            *err = QStringLiteral("%1: %2").arg(QLatin1String(err::E_SYNC_BASELINE_FAILED), *err);
        return false;
    }
    out->data = data;
    out->sourceMaxSeq = maxSeq;
    // C-03 fix: capture per-origin applied_vector snapshot so applyBaseline can call
    // av.resetTo(origin, epoch, seq, generation) with the correct epoch per origin.
    out->originCuts = queryOriginCuts(rconn);

    // M-01 fix: the exporting node's own writes never advance its own __sync_applied_vector,
    // so the (localOrigin, localEpoch) cut is absent from queryOriginCuts().  Merge it in
    // by taking the max, so the receiver does not reset the exporter's seq to 0.
    if (!localOrigin.isEmpty() && localEpoch > 0) {
        bool found = false;
        for (BaselineOriginCut& cut : out->originCuts) {
            if (cut.origin == localOrigin && cut.streamEpoch == localEpoch) {
                if (localOriginSeq > cut.appliedSeq)
                    cut.appliedSeq = localOriginSeq;
                found = true;
                break;
            }
        }
        if (!found) {
            BaselineOriginCut selfCut;
            selfCut.origin = localOrigin;
            selfCut.streamEpoch = localEpoch;
            selfCut.appliedSeq = localOriginSeq;
            out->originCuts.append(selfCut);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// applyBaseline
// ---------------------------------------------------------------------------
bool BaselineManager::applyBaseline(QSqlDatabase& wconn, sqlite3* /*h*/,
                                    const BaselineArtifact& art, AppliedVectorStore& av,
                                    TableStateStore& ts, RowWinnerStore& rw,
                                    ConsistencyCache& cache, qint64 epoch, const QString& origin,
                                    const QString& schemaFp, qint64* newAnchorSeq, QString* err,
                                    int baselineRank) {
    // I-20 helper: prefix err with E_SYNC_BASELINE_FAILED if not already an E_SYNC code.
    auto wrapErr = [&](QString* ep) {
        if (ep && !ep->startsWith(QLatin1String("E_SYNC")))
            *ep = QStringLiteral("%1: %2").arg(QLatin1String(err::E_SYNC_BASELINE_FAILED), *ep);
    };

    // C-1 fix: PRAGMA foreign_keys cannot be changed from within a SQLite transaction — the
    // pragma is silently ignored if issued inside BEGIN...COMMIT. Disable FK enforcement
    // BEFORE opening the transaction so the setting takes effect immediately.
    {
        QSqlQuery pragmaOff(wconn);
        if (!pragmaOff.exec(QStringLiteral("PRAGMA foreign_keys=OFF"))) {
            if (err)
                *err = QStringLiteral("%1: cannot disable FK enforcement: %2")
                           .arg(QLatin1String(err::E_SYNC_BASELINE_FAILED),
                                pragmaOff.lastError().text());
            return false;
        }
    }

    WriteTxn txn(wconn);
    if (!txn.begin(err)) {
        QSqlQuery pragmaOn(wconn);
        pragmaOn.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
        wrapErr(err);
        return false;
    }

    QStringList tables;
    if (!deserializeAndApply(wconn, art.data, &tables, err)) {
        txn.rollback();
        QSqlQuery pragmaOn(wconn);
        pragmaOn.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
        wrapErr(err);
        return false;
    }

    // C-4 fix: helper lambda to restore FK ON — called on every error and success path.
    // This is the RAII-style guard that prevents FK=OFF from leaking into the connection state
    // after any failure between FK=OFF and the final successful FK=ON.
    auto restoreFk = [&]() {
        QSqlQuery pragmaOn(wconn);
        pragmaOn.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    };

    // C-03 fix: reset applied_vector per (origin, epoch) using the authoritative cuts captured
    // at export time. Each cut carries its own stream_epoch so we call resetTo() with the
    // correct epoch instead of the local node's epoch (which may differ for remote origins).
    {
        bool primaryReset = false;
        for (const BaselineOriginCut& cut : art.originCuts) {
            // Use each cut's own epoch — this is the critical fix for multi-origin baselines.
            if (!av.resetTo(wconn, cut.origin, cut.streamEpoch, cut.appliedSeq, art.sourceMaxSeq,
                            err)) {
                txn.rollback();
                restoreFk();
                wrapErr(err);
                return false;
            }
            if (cut.origin == origin)
                primaryReset = true;
        }
        // If the primary origin was not in the cuts (empty baseline), reset it to seq=0.
        if (!primaryReset) {
            if (!av.resetTo(wconn, origin, epoch, 0, art.sourceMaxSeq, err)) {
                txn.rollback();
                restoreFk();
                wrapErr(err);
                return false;
            }
        }
    }

    // H-05 fix: pass the caller-supplied schemaFp so DiffEngine::tableDiffs() can match
    // the remote fingerprint and avoid producing false "Different" results after baseline apply.
    if (!ts.resetFromBaseline(wconn, tables, epoch, schemaFp, err)) {
        txn.rollback();
        restoreFk();
        wrapErr(err);
        return false;
    }

    // Clear row winner store — all rows now reflect baseline truth.
    if (!rw.resetAll(wconn, err)) {
        txn.rollback();
        restoreFk();
        wrapErr(err);
        return false;
    }

    // M-02 fix: after clearing row winners, seed RowWinner entries for every imported row so
    // subsequent low-rank challengers cannot overwrite baseline truth.  We use the same
    // pkHash format as ChangesetApplier: SHA-256(pk_value1\0pk_value2\0...).left(16).toHex().
    // appliedSeq for baseline rows is sourceMaxSeq (the highest changelog seq on the exporter).
    {
        const qint64 baselineSeq = art.sourceMaxSeq;
        for (const QString& tableName : tables) {
            // Collect PK column names and their indices via PRAGMA table_info.
            QStringList pkCols;
            {
                QSqlQuery pragmaQ(wconn);
                pragmaQ.prepare(QStringLiteral("PRAGMA table_info(") +
                                detail::SqlBuilder::quoteIdent(tableName) + QLatin1Char(')'));
                if (pragmaQ.exec()) {
                    while (pragmaQ.next()) {
                        if (pragmaQ.value(QStringLiteral("pk")).toInt() > 0)
                            pkCols << pragmaQ.value(QStringLiteral("name")).toString();
                    }
                }
            }
            if (pkCols.isEmpty())
                continue;  // no PK — skip (cannot compute pk_hash)

            QSqlQuery rowQ(wconn);
            rowQ.prepare(QStringLiteral("SELECT * FROM ") +
                         detail::SqlBuilder::quoteIdent(tableName));
            if (!rowQ.exec())
                continue;

            const QSqlRecord rec = rowQ.record();
            while (rowQ.next()) {
                // Build pkMaterial in the same format as ChangesetApplier::extractHashMaterials:
                // concatenate PK column values as UTF-8 bytes separated by \0.
                QByteArray pkMaterial;
                for (const QString& pkCol : pkCols) {
                    const int colIdx = rec.indexOf(pkCol);
                    if (colIdx >= 0) {
                        pkMaterial.append(rowQ.value(colIdx).toString().toUtf8());
                    }
                    pkMaterial.append('\0');
                }
                const QString pkHashStr = QString::fromLatin1(
                    QCryptographicHash::hash(pkMaterial, QCryptographicHash::Sha256)
                        .left(16)
                        .toHex());

                RowWinner winner;
                winner.origin = origin;
                winner.rank = baselineRank;
                winner.originSeq = baselineSeq;
                // contentHash left empty — rank/seq comparison is sufficient for conflict logic.
                rw.put(wconn, tableName, pkHashStr, winner, nullptr);
            }
        }
    }

    if (!txn.commit(err)) {
        restoreFk();
        wrapErr(err);
        return false;
    }

    // Re-enable FK enforcement AFTER the transaction commits.
    restoreFk();

    // Invalidate in-memory consistency cache for each table (outside txn is fine).
    for (const QString& table : tables) {
        cache.invalidateTable(wconn, table);
    }

    *newAnchorSeq = art.sourceMaxSeq;
    return true;
}

// ---------------------------------------------------------------------------
// shouldFallbackToBaseline
// ---------------------------------------------------------------------------
bool BaselineManager::shouldFallbackToBaseline(qint64 appliedSeq, qint64 sourceMinSeq) const {
    return appliedSeq < sourceMinSeq;
}

}  // namespace dbridge::sync
