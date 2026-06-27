#include "TableStateStore.h"

#include <QCryptographicHash>
#include <QDataStream>
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
    // Aggregate per-table deltas using unsigned quint64 to preserve modular-sum semantics.
    // M-02 fix: use separate add/sub quint64 instead of signed checksumDelta to avoid
    // signed-overflow UB when high bits are set.
    struct Delta {
        quint64 add = 0;
        quint64 sub = 0;
        qint64 rowDelta = 0;
    };
    QMap<QString, Delta> deltas;

    for (const TableMutation& m : muts) {
        Delta& d = deltas[m.table];
        if (m.isInsert) {
            d.add += hashToU64(m.afterHash);
            d.rowDelta += 1;
        } else if (m.isDelete) {
            d.sub += hashToU64(m.beforeHash);
            d.rowDelta -= 1;
        } else {
            d.add += hashToU64(m.afterHash);
            d.sub += hashToU64(m.beforeHash);
        }
    }

    for (auto it = deltas.begin(); it != deltas.end(); ++it) {
        if (!updateRow(db, it.key(), streamEpoch, schemaFp, it.value().add, it.value().sub,
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
    // H-04 fix: use length-prefix + type-tag encoding to prevent constructible collisions.
    // Format: quint32 col_count, then per-column: quint32 key_len, key_bytes,
    //   quint8 type (0=NULL, 1=int64, 2=double, 3=text, 4=blob), then value.
    // Columns are iterated in QMap (sorted) key order so hashes are deterministic.
    QByteArray data;
    QDataStream ds(&data, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::BigEndian);
    ds << quint32(row.size());
    for (auto it = row.cbegin(); it != row.cend(); ++it) {
        const QByteArray key = it.key().toUtf8();
        ds << quint32(key.size());
        ds.writeRawData(key.constData(), key.size());
        const QVariant& v = it.value();
        if (v.isNull()) {
            ds << quint8(0);
        } else if (v.type() == QVariant::ByteArray) {
            ds << quint8(4);
            const QByteArray ba = v.toByteArray();
            ds << quint32(ba.size());
            ds.writeRawData(ba.constData(), ba.size());
        } else if (v.canConvert<qlonglong>()) {
            ds << quint8(1) << qint64(v.toLongLong());
        } else if (v.canConvert<double>()) {
            ds << quint8(2) << double(v.toDouble());
        } else {
            ds << quint8(3);
            const QByteArray str = v.toString().toUtf8();
            ds << quint32(str.size());
            ds.writeRawData(str.constData(), str.size());
        }
    }
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).left(16);
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
                                const QString& schemaFp, quint64 add, quint64 sub,
                                qint64 rowCountDelta, qint64 highWaterSeq, QString* err) {
    const quint64 oldSum = readChecksum(db, table, streamEpoch);
    const qint64 oldRows = readRowCount(db, table, streamEpoch);

    // M-02 fix: pure unsigned modular arithmetic — no signed overflow UB.
    const quint64 newSum = oldSum + add - sub;
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
