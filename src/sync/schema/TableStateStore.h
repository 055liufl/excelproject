#pragma once
#include <QByteArray>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

namespace dbridge::sync {

// Represents a single row-level mutation for state accounting.
struct TableMutation {
    QString table;
    QString pkHash;
    QByteArray beforeHash;  // empty for INSERT
    QByteArray afterHash;   // empty for DELETE
    bool isInsert = false;
    bool isDelete = false;
};

// Maintains __sync_table_state with incremental checksum updates.
// content_checksum is a quint64 modular sum stored as hex string.
class TableStateStore {
   public:
    bool init(QSqlDatabase& db, QString* err);

    // Apply a batch of row mutations (called inside an active WriteTxn).
    bool applyMutations(QSqlDatabase& db, const QList<TableMutation>& muts, qint64 streamEpoch,
                        const QString& schemaFp, qint64 originSeq, QString* err);

    // Read current state for a table.
    bool readState(QSqlDatabase& db, const QString& table, qint64 streamEpoch, QString* fp,
                   QString* checksum, qint64* rowCount, QString* err);

    // Full baseline reset: scan all rows and recompute from scratch.
    bool resetFromBaseline(QSqlDatabase& db, const QStringList& tables, qint64 streamEpoch,
                           const QString& schemaFp, QString* err);

    // Canonical SHA-256 truncated to 16 bytes row hash.
    static QByteArray rowHash(const QVariantMap& row);

   private:
    // Convert first 8 bytes of hash to quint64 (big-endian).
    static quint64 hashToU64(const QByteArray& h);

    // Upsert one table's state row, applying delta to checksum and row_count.
    bool updateRow(QSqlDatabase& db, const QString& table, qint64 streamEpoch,
                   const QString& schemaFp, qint64 checksumDelta, qint64 rowCountDelta,
                   qint64 highWaterSeq, QString* err);

    // Read current raw checksum as quint64 (0 if not found).
    quint64 readChecksum(QSqlDatabase& db, const QString& table, qint64 streamEpoch);

    // Read current row count (0 if not found).
    qint64 readRowCount(QSqlDatabase& db, const QString& table, qint64 streamEpoch);
};

}  // namespace dbridge::sync
