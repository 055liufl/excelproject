#include "sync/baseline/BaselineManager.h"

#include "dbridge/Errors.h"

#include <QDataStream>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

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
        // Count rows first so we can write rowCount before row data.
        QSqlQuery cntQ(rconn);
        cntQ.prepare(QStringLiteral("SELECT COUNT(*) FROM \"%1\"").arg(table));
        if (!cntQ.exec() || !cntQ.next()) {
            if (err)
                *err = QStringLiteral("count failed on %1: %2").arg(table, cntQ.lastError().text());
            return false;
        }
        const quint64 rowCount = static_cast<quint64>(cntQ.value(0).toLongLong());

        ds << table;
        ds << rowCount;

        QSqlQuery rowQ(rconn);
        rowQ.prepare(QStringLiteral("SELECT * FROM \"%1\"").arg(table));
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

    // Max local_seq from changelog.
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
            delQ.prepare(QStringLiteral("DELETE FROM \"%1\"").arg(tableName));
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

            // Build INSERT OR REPLACE.
            QStringList cols, placeholders;
            for (auto it = rowMap.cbegin(); it != rowMap.cend(); ++it) {
                cols << QStringLiteral("\"%1\"").arg(it.key());
                placeholders << QStringLiteral("?");
            }

            const QString sql = QStringLiteral("INSERT OR REPLACE INTO \"%1\" (%2) VALUES (%3)")
                                    .arg(tableName, cols.join(QLatin1Char(',')),
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
    return true;
}

// ---------------------------------------------------------------------------
// applyBaseline
// ---------------------------------------------------------------------------
bool BaselineManager::applyBaseline(QSqlDatabase& wconn, sqlite3* /*h*/,
                                    const BaselineArtifact& art, AppliedVectorStore& av,
                                    TableStateStore& ts, RowWinnerStore& rw,
                                    ConsistencyCache& cache, qint64 epoch, const QString& origin,
                                    qint64* newAnchorSeq, QString* err) {
    // I-20 helper: prefix err with E_SYNC_BASELINE_FAILED if not already an E_SYNC code.
    auto wrapErr = [&](QString* ep) {
        if (ep && !ep->startsWith(QLatin1String("E_SYNC")))
            *ep = QStringLiteral("%1: %2").arg(QLatin1String(err::E_SYNC_BASELINE_FAILED), *ep);
    };

    WriteTxn txn(wconn);
    if (!txn.begin(err)) {
        wrapErr(err);
        return false;
    }

    QStringList tables;
    if (!deserializeAndApply(wconn, art.data, &tables, err)) {
        txn.rollback();
        wrapErr(err);
        return false;
    }

    // Reset applied vector for this origin/epoch.
    // baselineGeneration = sourceMaxSeq serves as the generation stamp.
    if (!av.reset(wconn, origin, epoch, art.sourceMaxSeq, err)) {
        txn.rollback();
        wrapErr(err);
        return false;
    }

    // Reset table state store (schemaFp unknown at baseline time; pass empty string
    // — caller may re-apply with real fp after schema negotiation).
    if (!ts.resetFromBaseline(wconn, tables, epoch, QString(), err)) {
        txn.rollback();
        wrapErr(err);
        return false;
    }

    // Clear row winner store — all rows now reflect baseline truth.
    if (!rw.resetAll(wconn, err)) {
        txn.rollback();
        wrapErr(err);
        return false;
    }

    if (!txn.commit(err)) {
        wrapErr(err);
        return false;
    }

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
