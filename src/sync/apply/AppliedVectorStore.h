#pragma once
#include <QSqlDatabase>
#include <QString>

namespace dbridge::sync {

// Result of a sequence number check before applying a changeset (G-05).
enum class SeqCheckResult {
    Apply,  // seq == applied_seq+1, proceed
    NoOp,   // seq <= applied_seq, already applied (idempotent)
    Gap     // seq > applied_seq+1, missing predecessor
};

// Maintains __sync_applied_vector for strict consecutive-sequence enforcement.
class AppliedVectorStore {
   public:
    bool init(QSqlDatabase& db, QString* err);

    // Check whether seq can be applied for (origin, epoch).
    SeqCheckResult check(QSqlDatabase& db, const QString& origin, qint64 epoch, qint64 seq,
                         QString* err);

    // Advance applied_seq to seq (must be called within the same WriteTxn as the apply).
    bool advance(QSqlDatabase& db, const QString& origin, qint64 epoch, qint64 seq, QString* err);

    // Reset to baseline: set applied_seq = 0 and bump baseline_generation.
    bool reset(QSqlDatabase& db, const QString& origin, qint64 epoch, qint64 baselineGeneration,
               QString* err);

    // Return current applied_seq (-1 if row not found).
    qint64 current(QSqlDatabase& db, const QString& origin, qint64 epoch);

   private:
    // Read (applied_seq, baseline_generation) for (origin, epoch).
    // Returns false if row not present; *appliedSeq = -1 in that case.
    bool readRow(QSqlDatabase& db, const QString& origin, qint64 epoch, qint64* appliedSeq,
                 qint64* baselineGen);
};

}  // namespace dbridge::sync
