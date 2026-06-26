#pragma once
#include <QByteArray>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

#include <climits>

namespace dbridge::sync {

// Describes the current winning origin for a given (table, pk_hash).
struct RowWinner {
    QString origin;
    int rank = INT_MIN;  // sentinel: no winner
    qint64 originSeq = 0;
    QByteArray contentHash;
    QString winningContent;  // C-01: JSON-encoded row for low-rank DELETE recovery
};

// Maintains __sync_row_winner with (rank, seq) max-element conflict resolution (G-01).
class RowWinnerStore {
   public:
    bool init(QSqlDatabase& db, QString* err);

    // Read current winner. Returns rank == INT_MIN if no record exists.
    RowWinner get(QSqlDatabase& db, const QString& table, const QString& pkHash);

    // Write new winner if inRank > cur.rank, or (==rank && inSeq > cur.originSeq).
    bool put(QSqlDatabase& db, const QString& table, const QString& pkHash, const RowWinner& winner,
             QString* err);

    // H-01 fix: like put(), but also allows overwrite when rank/seq match and the stored
    // winningContent is empty (completes an earlier partial write from conflictCb).
    bool putOrRefill(QSqlDatabase& db, const QString& table, const QString& pkHash,
                     const RowWinner& winner, QString* err);

    // Delete all winner rows (called after baseline reset).
    bool resetAll(QSqlDatabase& db, QString* err);

    // C-06 fix: delete a single winner row (called when a low-rank DELETE erased the winner).
    bool clear(QSqlDatabase& db, const QString& table, const QString& pkHash, QString* err);

    // Generate pk_hash: canonical key string SHA-256 first 16 bytes as hex.
    static QString pkHash(const QVariantMap& pkValues);

   private:
    // Returns true if challenger wins over incumbent.
    static bool beats(const RowWinner& challenger, const RowWinner& incumbent);
};

}  // namespace dbridge::sync
