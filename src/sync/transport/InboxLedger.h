#pragma once
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

namespace dbridge::sync {

enum class LedgerStatus { Seen, Consumed, Corrupt, Unknown };

// CRUD wrapper for __sync_inbox_ledger.
// Enforces idempotent artifact consumption: once an artifact is Consumed it
// is never re-processed regardless of how many times it arrives.
class InboxLedger {
   public:
    bool init(QSqlDatabase& db, QString* err);

    // Record first discovery of an artifact (idempotent: no-op if already known).
    bool markSeen(QSqlDatabase& db, const QString& artifactName, QString* err);

    // Advance status to Consumed.
    bool markConsumed(QSqlDatabase& db, const QString& artifactName, QString* err);

    // Mark artifact as Corrupt (will not be retried automatically).
    bool markCorrupt(QSqlDatabase& db, const QString& artifactName, QString* err);

    // Query current status.  Returns Unknown if not in ledger.
    LedgerStatus status(QSqlDatabase& db, const QString& artifactName);

    // Return all artifact names with status == 'seen' (pending consumption).
    QStringList pendingSeen(QSqlDatabase& db);
};

}  // namespace dbridge::sync
