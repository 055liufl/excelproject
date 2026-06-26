#include "TableStateStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>
#include <QVariant>

#include "sql/SqlBuilder.h"

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

bool TableStateStore::init(QSqlDatabase& db, QString* err) {
    // Table is created by SyncDDL; here we just verify it is accessible.
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_table_state WHERE 0"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool TableStateStore::applyMutations(QSqlDatabase& db, const QList<TableMutation>& muts,
                                     qint64 streamEpoch, const QString& schemaFp, qint64 originSeq,
                                     QString* err) {
    // Aggregate per-table deltas to minimise SQL round-trips.
    struct Delta {
        qint64 checksumDelta = 0;
        qint64 rowDelta = 0;
    };
    QMap<QString, Delta> deltas;

    for (const TableMutation& m : muts) {
        Delta& d = deltas[m.table];
        if (m.isInsert) {
            // INSERT: +H(new)
            d.checksumDelta += static_cast<qint64>(hashToU64(m.afterHash));
            d.rowDelta += 1;
        } else if (m.isDelete) {
            // DELETE: -H(old)
            d.checksumDelta -= static_cast<qint64>(hashToU64(m.beforeHash));
            d.rowDelta -= 1;
        } else {
            // UPDATE: +H(new) - H(old)
            d.checksumDelta += static_cast<qint64>(hashToU64(m.afterHash));
            d.checksumDelta -= static_cast<qint64>(hashToU64(m.beforeHash));
        }
    }

    for (auto it = deltas.begin(); it != deltas.end(); ++it) {
        if (!updateRow(db, it.key(), streamEpoch, schemaFp, it.value().checksumDelta,
                       it.value().rowDelta, originSeq, err)) {
            return false;
        }
    }
    return true;
}

bool TableStateStore::readState(QSqlDatabase& db, const QString& table, qint64 streamEpoch,
                                QString* fp, QString* checksum, qint64* rowCount, bool* found,
                                QString* err) {
    // J-12: Distinguish "not found" (table never synced) from "query error".
    if (found)
        *found = false;

    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT schema_fingerprint, content_checksum, row_count "
                       "FROM __sync_table_state "
                       "WHERE table_name = ? AND stream_epoch = ?"));
    q.addBindValue(table);
    q.addBindValue(streamEpoch);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;  // genuine query error
    }
    if (!q.next()) {
        // Row does not exist — not an error; *found remains false.
        return true;
    }
    if (found)
        *found = true;
    if (fp)
        *fp = q.value(0).toString();
    if (checksum)
        *checksum = q.value(1).toString();
    if (rowCount)
        *rowCount = q.value(2).toLongLong();
    return true;
}

bool TableStateStore::resetFromBaseline(QSqlDatabase& db, const QStringList& tables,
                                        qint64 streamEpoch, const QString& schemaFp, QString* err) {
    // Delete existing rows for this epoch, then rebuild by scanning each table.
    for (const QString& tbl : tables) {
        {
            QSqlQuery del(db);
            del.prepare(QStringLiteral(
                "DELETE FROM __sync_table_state WHERE table_name = ? AND stream_epoch = ?"));
            del.addBindValue(tbl);
            del.addBindValue(streamEpoch);
            if (!del.exec()) {
                if (err)
                    *err = del.lastError().text();
                return false;
            }
        }

        // Scan all rows of the business table to compute checksum + count.
        // H-3 fix: use quoteIdent to handle table names with embedded double-quotes.
        QSqlQuery scan(db);
        if (!scan.exec(QStringLiteral("SELECT * FROM ") + detail::SqlBuilder::quoteIdent(tbl))) {
            if (err)
                *err = scan.lastError().text();
            return false;
        }

        quint64 runningSum = 0;
        qint64 rowCount = 0;
        while (scan.next()) {
            QVariantMap row;
            const QSqlRecord rec = scan.record();
            for (int i = 0; i < rec.count(); ++i) {
                row.insert(rec.fieldName(i), rec.value(i));
            }
            runningSum += hashToU64(rowHash(row));
            ++rowCount;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        QSqlQuery ins(db);
        ins.prepare(
            QStringLiteral("INSERT OR REPLACE INTO __sync_table_state "
                           "(table_name, stream_epoch, schema_fingerprint, high_water_seq, "
                           " content_checksum, row_count, updated_ms) "
                           "VALUES (?, ?, ?, 0, ?, ?, ?)"));
        ins.addBindValue(tbl);
        ins.addBindValue(streamEpoch);
        ins.addBindValue(schemaFp);
        ins.addBindValue(QString::number(runningSum));
        ins.addBindValue(rowCount);
        ins.addBindValue(nowMs);
        if (!ins.exec()) {
            if (err)
                *err = ins.lastError().text();
            return false;
        }
    }
    return true;
}

QByteArray TableStateStore::rowHash(const QVariantMap& row) {
    // Serialise map as "key=value\n" pairs in sorted key order, then SHA-256.
    QByteArray data;
    for (auto it = row.begin(); it != row.end(); ++it) {
        data.append(it.key().toUtf8());
        data.append('=');
        data.append(it.value().toByteArray());
        data.append('\n');
    }
    QByteArray full = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    return full.left(16);
}

// ---------------------------------------------------------------------------
// private helpers
// ---------------------------------------------------------------------------

quint64 TableStateStore::hashToU64(const QByteArray& h) {
    if (h.size() < 8)
        return 0;
    quint64 v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<quint8>(h[i]);
    }
    return v;
}

bool TableStateStore::updateRow(QSqlDatabase& db, const QString& table, qint64 streamEpoch,
                                const QString& schemaFp, qint64 checksumDelta, qint64 rowCountDelta,
                                qint64 highWaterSeq, QString* err) {
    const quint64 oldSum = readChecksum(db, table, streamEpoch);
    const qint64 oldRows = readRowCount(db, table, streamEpoch);

    // Modular addition on quint64 (wraps naturally).
    const quint64 newSum = static_cast<quint64>(static_cast<qint64>(oldSum) + checksumDelta);
    const qint64 newRows = oldRows + rowCountDelta;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("INSERT INTO __sync_table_state "
                       "(table_name, stream_epoch, schema_fingerprint, high_water_seq, "
                       " content_checksum, row_count, updated_ms) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?) "
                       "ON CONFLICT(table_name, stream_epoch) DO UPDATE SET "
                       "  schema_fingerprint = excluded.schema_fingerprint, "
                       "  high_water_seq     = MAX(high_water_seq, excluded.high_water_seq), "
                       "  content_checksum   = excluded.content_checksum, "
                       "  row_count          = excluded.row_count, "
                       "  updated_ms         = excluded.updated_ms"));
    q.addBindValue(table);
    q.addBindValue(streamEpoch);
    q.addBindValue(schemaFp);
    q.addBindValue(highWaterSeq);
    q.addBindValue(QString::number(newSum));
    q.addBindValue(newRows);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

quint64 TableStateStore::readChecksum(QSqlDatabase& db, const QString& table, qint64 streamEpoch) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT content_checksum FROM __sync_table_state "
                       "WHERE table_name = ? AND stream_epoch = ?"));
    q.addBindValue(table);
    q.addBindValue(streamEpoch);
    if (q.exec() && q.next()) {
        bool ok = false;
        quint64 v = q.value(0).toString().toULongLong(&ok);
        return ok ? v : 0;
    }
    return 0;
}

qint64 TableStateStore::readRowCount(QSqlDatabase& db, const QString& table, qint64 streamEpoch) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT row_count FROM __sync_table_state "
                       "WHERE table_name = ? AND stream_epoch = ?"));
    q.addBindValue(table);
    q.addBindValue(streamEpoch);
    if (q.exec() && q.next())
        return q.value(0).toLongLong();
    return 0;
}

}  // namespace dbridge::sync
