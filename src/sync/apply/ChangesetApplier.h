#pragma once
#include <QByteArray>
#include <QSqlDatabase>
#include <QString>

#include "RowWinnerStore.h"
#include <sqlite3.h>

namespace dbridge::sync {

struct ApplyOptions {
    // Authoritative down-link: always replace without consulting RowWinnerStore.
    bool authoritative = false;
};

struct ApplyOutcome {
    int applied = 0;
    int conflicts = 0;
    int ignored = 0;
    // Rebase output — filled only when authoritative=false.
    QByteArray rebaseBuffer;
};

// Applies a raw SQLite changeset via sqlite3changeset_apply_v2.
// Conflict resolution is delegated to the static conflictCb() which consults
// RowWinnerStore (G-01) for non-authoritative paths.
class ChangesetApplier {
   public:
    // H-04 fix: syncTables limits which tables xFilter accepts; empty = accept all (test only).
    bool apply(sqlite3* h, QSqlDatabase& wconn, const QByteArray& changeset, const QString& origin,
               int originRank, qint64 originSeq, RowWinnerStore& winners, const ApplyOptions& opts,
               const QStringList& syncTables, ApplyOutcome* out, QString* err);

    // H-01 fix: shared allow-list predicate — same logic as filterCb() so all three paths
    // (xFilter, updateWinnersFromChangeset, extractMutations) reject the same tables.
    // Public so CapturedWriteTemplate can reuse the same predicate.
    static bool isAllowedSyncTable(const QString& table, const QStringList& syncTables);

   private:
    struct ConflictCtx {
        ChangesetApplier* self;
        sqlite3* h;
        QSqlDatabase* wconn;
        QString origin;
        int rank;
        qint64 seq;
        RowWinnerStore* winners;
        bool authoritative;
        ApplyOutcome* outcome;
        // H-04: tables allowed by xFilter; empty = accept all.
        const QStringList* syncTables = nullptr;
    };

    static int conflictCb(void* ctx, int conflict, sqlite3_changeset_iter* iter);
    static int filterCb(void* ctx, const char* tblName);  // H-04: xFilter

    // C-11/C-12: post-apply, update row_winner for INSERT/UPDATE and restore any high-rank row
    // erased by a dominated low-rank DELETE. Returns false (with *err set) if a required restore
    // failed — the caller MUST roll back the transaction so the low-rank DELETE never wins.
    // H-01 fix: syncTables is the same allow-list used by filterCb; tables not in the list
    // (and __sync_* tables) are skipped so row_winner is not polluted.
    bool updateWinnersFromChangeset(const QByteArray& changeset, const QString& origin, int rank,
                                    qint64 seq, RowWinnerStore& winners, QSqlDatabase& wconn,
                                    const QStringList& syncTables, QString* err);
};

}  // namespace dbridge::sync
