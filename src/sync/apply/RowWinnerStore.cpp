#include "RowWinnerStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>

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
        QStringLiteral("SELECT winning_origin, winning_rank, winning_origin_seq, content_hash "
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
                       " winning_origin_seq, content_hash, updated_ms) "
                       "VALUES (?, ?, ?, ?, ?, ?, ?) "
                       "ON CONFLICT(table_name, pk_hash) DO UPDATE SET "
                       "  winning_origin     = excluded.winning_origin, "
                       "  winning_rank       = excluded.winning_rank, "
                       "  winning_origin_seq = excluded.winning_origin_seq, "
                       "  content_hash       = excluded.content_hash, "
                       "  updated_ms         = excluded.updated_ms"));
    q.addBindValue(table);
    q.addBindValue(pkHash_);
    q.addBindValue(winner.origin);
    q.addBindValue(winner.rank);
    q.addBindValue(winner.originSeq);
    q.addBindValue(winner.contentHash);
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
    // Canonical: sorted keys, "key=value\n" joined, SHA-256 first 16 bytes as hex.
    QByteArray data;
    // QVariantMap iterates in key order (sorted).
    for (auto it = pkValues.begin(); it != pkValues.end(); ++it) {
        data.append(it.key().toUtf8());
        data.append('=');
        data.append(it.value().toByteArray());
        data.append('\n');
    }
    QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    return QString::fromLatin1(hash.left(16).toHex());
}

// ---------------------------------------------------------------------------
// private
// ---------------------------------------------------------------------------

bool RowWinnerStore::beats(const RowWinner& challenger, const RowWinner& incumbent) {
    if (incumbent.rank == INT_MIN)
        return true;  // no incumbent
    if (challenger.rank > incumbent.rank)
        return true;
    if (challenger.rank == incumbent.rank && challenger.originSeq > incumbent.originSeq)
        return true;
    return false;
}

}  // namespace dbridge::sync
