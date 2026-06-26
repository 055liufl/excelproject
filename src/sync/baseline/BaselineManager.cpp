#include "sync/baseline/BaselineManager.h"

#include "dbridge/Errors.h"

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

// C-03 fix: query MAX(origin_seq) GROUP BY origin from __sync_changelog and return the map.
static QHash<QString, qint64> queryOriginMaxSeq(QSqlDatabase& rconn) {
    QHash<QString, qint64> result;
    QSqlQuery q(rconn);
    if (!q.exec(
            QStringLiteral("SELECT origin, MAX(origin_seq) FROM __sync_changelog GROUP BY origin")))
        return result;
    while (q.next())
        result.insert(q.value(0).toString(), q.value(1).toLongLong());
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
                                     BaselineArtifact* out, QString* err) {
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
    // C-03 fix: capture per-origin max origin_seq so applyBaseline can reset applied_vector
    // to the correct authoritative truncation point.
    out->originMaxSeq = queryOriginMaxSeq(rconn);
    return true;
}

// ---------------------------------------------------------------------------
// applyBaseline
// ---------------------------------------------------------------------------
bool BaselineManager::applyBaseline(QSqlDatabase& wconn, sqlite3* /*h*/,
                                    const BaselineArtifact& art, AppliedVectorStore& av,
                                    TableStateStore& ts, RowWinnerStore& rw,
                                    ConsistencyCache& cache, qint64 epoch, const QString& origin,
                                    const QString& schemaFp, qint64* newAnchorSeq, QString* err) {
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

    // C-03 fix: reset applied_vector per origin to the authoritative origin_seq captured
    // at export time.  For origins present in originMaxSeq use resetTo(); for the primary
    // origin use the supplied origin parameter as fallback.
    {
        // Reset the primary origin (the baseline provider).
        const qint64 primaryOriginSeq = art.originMaxSeq.value(origin, 0);
        if (!av.resetTo(wconn, origin, epoch, primaryOriginSeq, art.sourceMaxSeq, err)) {
            txn.rollback();
            restoreFk();
            wrapErr(err);
            return false;
        }
        // Reset all other origins captured in the export.
        for (auto it = art.originMaxSeq.cbegin(); it != art.originMaxSeq.cend(); ++it) {
            if (it.key() == origin)
                continue;
            if (!av.resetTo(wconn, it.key(), epoch, it.value(), art.sourceMaxSeq, err)) {
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
