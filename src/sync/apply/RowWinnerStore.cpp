#include "RowWinnerStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>

#include "../schema/TableStateStore.h"
#include <climits>

namespace dbridge::sync {

bool RowWinnerStore::init(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("SELECT COUNT(*) FROM __sync_row_winner WHERE 0"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

RowWinner RowWinnerStore::get(QSqlDatabase& db, const QString& table, const QString& pkHash_) {
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("SELECT winning_origin, winning_rank, winning_origin_seq, "
                       "content_hash, winning_content "
                       "FROM __sync_row_winner "
                       "WHERE table_name = ? AND pk_hash = ?"));
    q.addBindValue(table);
    q.addBindValue(pkHash_);
    if (!q.exec() || !q.next()) {
        RowWinner empty;
        empty.rank = INT_MIN;
        return empty;
    }
    RowWinner w;
    w.origin = q.value(0).toString();
    w.rank = q.value(1).toInt();
    w.originSeq = q.value(2).toLongLong();
    w.contentHash = q.value(3).toByteArray();
    w.winningContent = q.value(4).toString();
    return w;
}

bool RowWinnerStore::put(QSqlDatabase& db, const QString& table, const QString& pkHash_,
                         const RowWinner& winner, QString* err) {
    const RowWinner cur = get(db, table, pkHash_);
    if (!beats(winner, cur)) {
        return true;  // incumbent wins; no-op
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("INSERT INTO __sync_row_winner "
                       "(table_name, pk_hash, winning_origin, winning_rank, "
                       " winning_origin_seq, content_hash, winning_content, updated_ms) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                       "ON CONFLICT(table_name, pk_hash) DO UPDATE SET "
                       "  winning_origin     = excluded.winning_origin, "
                       "  winning_rank       = excluded.winning_rank, "
                       "  winning_origin_seq = excluded.winning_origin_seq, "
                       "  content_hash       = excluded.content_hash, "
                       "  winning_content    = excluded.winning_content, "
                       "  updated_ms         = excluded.updated_ms"));
    q.addBindValue(table);
    q.addBindValue(pkHash_);
    q.addBindValue(winner.origin);
    q.addBindValue(winner.rank);
    q.addBindValue(winner.originSeq);
    q.addBindValue(winner.contentHash);
    q.addBindValue(winner.winningContent);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool RowWinnerStore::resetAll(QSqlDatabase& db, QString* err) {
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("DELETE FROM __sync_row_winner"))) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

bool RowWinnerStore::clear(QSqlDatabase& db, const QString& table, const QString& pkHash_,
                           QString* err) {
    QSqlQuery q(db);
    q.prepare(QStringLiteral("DELETE FROM __sync_row_winner WHERE table_name = ? AND pk_hash = ?"));
    q.addBindValue(table);
    q.addBindValue(pkHash_);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

QString RowWinnerStore::pkHash(const QVariantMap& pkValues) {
    // M-01 fix: use the same canonical type-tagged encoding as TableStateStore::rowHash()
    // to prevent constructible collisions between different PK rows.
    return QString::fromLatin1(TableStateStore::rowHash(pkValues).toHex());
}

bool RowWinnerStore::putOrRefill(QSqlDatabase& db, const QString& table, const QString& pkHash_,
                                 const RowWinner& winner, QString* err) {
    const RowWinner cur = get(db, table, pkHash_);
    // Accept if challenger normally beats incumbent, OR if rank/seq match and stored content is
    // empty (H-01: complete a partial write that had no winningContent).
    const bool sameRankSeq = (cur.rank != INT_MIN) && (winner.rank == cur.rank) &&
                             (winner.originSeq == cur.originSeq) && cur.winningContent.isEmpty();
    if (!beats(winner, cur) && !sameRankSeq) {
        return true;  // incumbent wins; no-op
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare(
        QStringLiteral("INSERT INTO __sync_row_winner "
                       "(table_name, pk_hash, winning_origin, winning_rank, "
                       " winning_origin_seq, content_hash, winning_content, updated_ms) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
                       "ON CONFLICT(table_name, pk_hash) DO UPDATE SET "
                       "  winning_origin     = excluded.winning_origin, "
                       "  winning_rank       = excluded.winning_rank, "
                       "  winning_origin_seq = excluded.winning_origin_seq, "
                       "  content_hash       = excluded.content_hash, "
                       "  winning_content    = excluded.winning_content, "
                       "  updated_ms         = excluded.updated_ms"));
    q.addBindValue(table);
    q.addBindValue(pkHash_);
    q.addBindValue(winner.origin);
    q.addBindValue(winner.rank);
    q.addBindValue(winner.originSeq);
    q.addBindValue(winner.contentHash);
    q.addBindValue(winner.winningContent);
    q.addBindValue(nowMs);
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// private
// ---------------------------------------------------------------------------

bool RowWinnerStore::beats(const RowWinner& challenger, const RowWinner& incumbent) {
    if (incumbent.rank == INT_MIN)
        return true;  // no incumbent
    if (challenger.rank > incumbent.rank)
        return true;
    if (challenger.rank < incumbent.rank)
        return false;
    // Same rank: compare seq.
    if (challenger.originSeq > incumbent.originSeq)
        return true;
    if (challenger.originSeq < incumbent.originSeq)
        return false;
    // H-01 fix: rank == rank and seq == seq — use originId as a stable, deterministic
    // tie-breaker so that applying changesets in any order yields the same final state.
    // Lexicographically larger originId wins (arbitrary but consistent).
    return challenger.origin > incumbent.origin;
}

}  // namespace dbridge::sync
