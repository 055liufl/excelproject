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
    bool apply(sqlite3* h, QSqlDatabase& wconn, const QByteArray& changeset, const QString& origin,
               int originRank, qint64 originSeq, RowWinnerStore& winners, const ApplyOptions& opts,
               ApplyOutcome* out, QString* err);

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
    };

    static int conflictCb(void* ctx, int conflict, sqlite3_changeset_iter* iter);

    // Post-apply: update row_winner for all successfully inserted/updated rows.
    void updateWinnersFromChangeset(const QByteArray& changeset, const QString& origin, int rank,
                                    qint64 seq, RowWinnerStore& winners, QSqlDatabase& wconn);
};

}  // namespace dbridge::sync
