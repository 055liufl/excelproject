#include "ChangesetApplier.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include <sqlite3.h>

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// helpers: build pkHash and contentHash from a changeset iterator position
// ---------------------------------------------------------------------------

namespace {

// Extract PK-only material and full-content material from a changeset row's
// new values (or old values for DELETE).  Returns false if nothing was read.
bool extractHashMaterials(sqlite3_changeset_iter* iter, int nCol, bool useNew,
                          unsigned char* pkMask, QByteArray* pkMaterial,
                          QByteArray* contentMaterial) {
    pkMaterial->clear();
    contentMaterial->clear();

    for (int i = 0; i < nCol; i++) {
        sqlite3_value* val = nullptr;
        if (useNew)
            sqlite3changeset_new(iter, i, &val);
        else
            sqlite3changeset_old(iter, i, &val);
        if (!val)
            continue;

        QByteArray colBytes;
        const int vtype = sqlite3_value_type(val);
        if (vtype == SQLITE_TEXT) {
            const char* txt = reinterpret_cast<const char*>(sqlite3_value_text(val));
            colBytes = QByteArray(txt ? txt : "");
        } else if (vtype == SQLITE_INTEGER) {
            colBytes = QByteArray::number(static_cast<qint64>(sqlite3_value_int64(val)));
        } else if (vtype == SQLITE_BLOB) {
            const void* b = sqlite3_value_blob(val);
            int blen = sqlite3_value_bytes(val);
            if (b && blen > 0)
                colBytes = QByteArray(static_cast<const char*>(b), blen);
        }
        // NULL stays as empty bytes — included as separator only.

        if (pkMask && pkMask[i]) {
            pkMaterial->append(colBytes);
            pkMaterial->append('\0');
        }
        contentMaterial->append(colBytes);
        contentMaterial->append('\0');
    }
    return true;
}

QString computePkHashStr(const QByteArray& pkMaterial, const QByteArray& contentMaterial) {
    const QByteArray& src = pkMaterial.isEmpty() ? contentMaterial : pkMaterial;
    QByteArray h = QCryptographicHash::hash(src, QCryptographicHash::Sha256).left(16);
    return QString::fromLatin1(h.toHex());
}

}  // namespace

// ---------------------------------------------------------------------------
// public: apply
// ---------------------------------------------------------------------------

bool ChangesetApplier::apply(sqlite3* h, QSqlDatabase& wconn, const QByteArray& changeset,
                             const QString& origin, int originRank, qint64 originSeq,
                             RowWinnerStore& winners, const ApplyOptions& opts,
                             const QStringList& syncTables, ApplyOutcome* out, QString* err) {
    if (!out)
        return false;
    *out = ApplyOutcome{};

    ConflictCtx ctx;
    ctx.self = this;
    ctx.h = h;
    ctx.wconn = &wconn;
    ctx.origin = origin;
    ctx.rank = originRank;
    ctx.seq = originSeq;
    ctx.winners = &winners;
    ctx.authoritative = opts.authoritative;
    ctx.outcome = out;

    // H-04 fix: pass syncTables into ConflictCtx so the shared pCtx carries both
    // xFilter (table allow-list) and xConflict (row-winner arbitration) state.
    ctx.syncTables = syncTables.isEmpty() ? nullptr : &syncTables;

    // C-11 fix: there is no reliable public SQLite API to rebuild a row-filtered changeset, so
    // we do NOT pre-filter. Instead, low-rank DELETE protection is enforced AFTER apply by
    // updateWinnersFromChangeset(): it restores any high-rank row erased by a dominated DELETE,
    // within the SAME transaction. If the restore fails, apply() returns false and the caller
    // rolls back — so a low-rank DELETE can never win (G-01/FR-6).
    void* pRebase = nullptr;
    int nRebase = 0;

    int rc = sqlite3changeset_apply_v2(
        h, changeset.size(), const_cast<void*>(static_cast<const void*>(changeset.constData())),
        &filterCb,  // H-04: table filter — uses same pCtx as conflictCb
        &conflictCb, &ctx, &pRebase, &nRebase, SQLITE_CHANGESETAPPLY_NOSAVEPOINT);

    if (pRebase && nRebase > 0 && !opts.authoritative) {
        out->rebaseBuffer = QByteArray(static_cast<const char*>(pRebase), nRebase);
    }
    if (pRebase)
        sqlite3_free(pRebase);

    if (rc != SQLITE_OK && rc != SQLITE_ROW) {
        if (err)
            *err = QStringLiteral("sqlite3changeset_apply_v2 rc=%1").arg(rc);
        return false;
    }

    // C-12 fix: post-apply winner update + low-rank DELETE recovery. Returns false on a failed
    // recovery; the caller must roll back so the bad terminal state is never committed/ACKed.
    // Authoritative (down-link) applies skip winner arbitration entirely.
    if (!opts.authoritative) {
        if (!updateWinnersFromChangeset(changeset, origin, originRank, originSeq, winners, wconn,
                                        err))
            return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// private: xFilter callback (H-04)
// ---------------------------------------------------------------------------

int ChangesetApplier::filterCb(void* ctx, const char* tblName) {
    auto* c = static_cast<ConflictCtx*>(ctx);
    if (!c->syncTables)
        return 1;  // accept all when list is empty
    // Always reject internal __sync_* meta tables.
    if (qstrncmp(tblName, "__sync_", 7) == 0)
        return 0;
    return c->syncTables->contains(QString::fromUtf8(tblName)) ? 1 : 0;
}

// ---------------------------------------------------------------------------
// private: conflict callback
// ---------------------------------------------------------------------------

int ChangesetApplier::conflictCb(void* ctx, int conflict, sqlite3_changeset_iter* iter) {
    auto* c = static_cast<ConflictCtx*>(ctx);

    switch (conflict) {
        case SQLITE_CHANGESET_DATA:
        case SQLITE_CHANGESET_CONFLICT: {
            c->outcome->conflicts++;
            if (c->authoritative) {
                c->outcome->applied++;
                return SQLITE_CHANGESET_REPLACE;
            }

            // Non-authoritative: consult RowWinnerStore.
            const char* tblName = nullptr;
            int nCol = 0;
            int opOut = 0;
            int bIndirectOut = 0;
            sqlite3changeset_op(iter, &tblName, &nCol, &opOut, &bIndirectOut);
            const QString table = QString::fromUtf8(tblName ? tblName : "");

            // Use PK mask to build pkHash from PK columns only (I-06).
            unsigned char* pkMask = nullptr;
            sqlite3changeset_pk(iter, &pkMask, nullptr);

            QByteArray pkMaterial, contentMaterial;
            extractHashMaterials(iter, nCol, /*useNew=*/true, pkMask, &pkMaterial,
                                 &contentMaterial);

            const QString pkHashStr = computePkHashStr(pkMaterial, contentMaterial);
            QByteArray contentH =
                QCryptographicHash::hash(contentMaterial, QCryptographicHash::Sha256).left(16);

            RowWinner challenger;
            challenger.origin = c->origin;
            challenger.rank = c->rank;
            challenger.originSeq = c->seq;
            challenger.contentHash = contentH;

            RowWinner incumbent = c->winners->get(*c->wconn, table, pkHashStr);
            bool win =
                (incumbent.rank == INT_MIN) || (challenger.rank > incumbent.rank) ||
                (challenger.rank == incumbent.rank && challenger.originSeq > incumbent.originSeq);
            if (win) {
                c->winners->put(*c->wconn, table, pkHashStr, challenger, nullptr);
                c->outcome->applied++;
                return SQLITE_CHANGESET_REPLACE;
            }
            c->outcome->ignored++;
            return SQLITE_CHANGESET_OMIT;
        }

        case SQLITE_CHANGESET_NOTFOUND:
            c->outcome->ignored++;
            return SQLITE_CHANGESET_OMIT;

        case SQLITE_CHANGESET_FOREIGN_KEY:
        case SQLITE_CHANGESET_CONSTRAINT:
            return SQLITE_CHANGESET_ABORT;

        default:
            return SQLITE_CHANGESET_OMIT;
    }
}

// ---------------------------------------------------------------------------
// private: updateWinnersFromChangeset (I-03)
// ---------------------------------------------------------------------------

bool ChangesetApplier::updateWinnersFromChangeset(const QByteArray& changeset,
                                                  const QString& origin, int rank, qint64 seq,
                                                  RowWinnerStore& winners, QSqlDatabase& wconn,
                                                  QString* err) {
    sqlite3_changeset_iter* iter = nullptr;
    int rc =
        sqlite3changeset_start(&iter, changeset.size(),
                               const_cast<void*>(static_cast<const void*>(changeset.constData())));
    if (rc != SQLITE_OK || !iter)
        return true;  // nothing to iterate — not a failure

    bool ok = true;
    while (sqlite3changeset_next(iter) == SQLITE_ROW) {
        const char* tbl = nullptr;
        int nCol = 0, op = 0, indirect = 0;
        sqlite3changeset_op(iter, &tbl, &nCol, &op, &indirect);

        unsigned char* pkMask = nullptr;
        sqlite3changeset_pk(iter, &pkMask, nullptr);

        // For DELETE: use old values to build the pk/content hash.
        const bool useNew = (op != SQLITE_DELETE);
        QByteArray pkMaterial, contentMaterial;
        extractHashMaterials(iter, nCol, useNew, pkMask, &pkMaterial, &contentMaterial);

        const QString pkHashStr = computePkHashStr(pkMaterial, contentMaterial);
        const QString tableName = QString::fromUtf8(tbl ? tbl : "");

        if (op == SQLITE_DELETE) {
            // C-12 fix: a DELETE from a node dominated by the current winner must NOT erase the
            // high-rank row. apply_v2 already executed the DELETE; restore the winning row here,
            // in the same transaction. If we cannot restore, return false so the caller rolls
            // back the whole apply (the DELETE is undone, applied_vector is not advanced, no ACK).
            RowWinner incumbent = winners.get(wconn, tableName, pkHashStr);
            const bool dominated =
                incumbent.rank != INT_MIN &&
                ((rank < incumbent.rank) || (rank == incumbent.rank && seq < incumbent.originSeq));
            if (dominated) {
                if (incumbent.winningContent.isEmpty()) {
                    // We know the row should survive but have no content to restore it with.
                    // Fail the apply so the DELETE is rolled back rather than silently winning.
                    if (err)
                        *err = QStringLiteral(
                                   "E_SYNC_APPLY_CONSTRAINT: low-rank DELETE on %1 would erase "
                                   "high-rank winner (origin=%2 rank=%3) with no stored content "
                                   "to restore")
                                   .arg(tableName, incumbent.origin)
                                   .arg(incumbent.rank);
                    ok = false;
                    break;
                }
                QJsonDocument doc = QJsonDocument::fromJson(incumbent.winningContent.toUtf8());
                if (!doc.isObject()) {
                    if (err)
                        *err = QStringLiteral(
                                   "E_SYNC_APPLY_CONSTRAINT: winning_content for %1 is "
                                   "not a JSON object")
                                   .arg(tableName);
                    ok = false;
                    break;
                }
                const QJsonObject obj = doc.object();
                QStringList cols, placeholders;
                QVariantList vals;
                for (auto it = obj.begin(); it != obj.end(); ++it) {
                    cols << QStringLiteral("\"%1\"").arg(
                        QString(it.key()).replace(QLatin1Char('"'), QLatin1String("\"\"")));
                    placeholders << QStringLiteral("?");
                    vals << it.value().toVariant();
                }
                if (cols.isEmpty()) {
                    if (err)
                        *err = QStringLiteral(
                                   "E_SYNC_APPLY_CONSTRAINT: winning_content for %1 has no columns")
                                   .arg(tableName);
                    ok = false;
                    break;
                }
                QSqlQuery restoreQ(wconn);
                restoreQ.prepare(
                    QStringLiteral("INSERT OR REPLACE INTO \"%1\" (%2) VALUES (%3)")
                        .arg(QString(tableName).replace(QLatin1Char('"'), QLatin1String("\"\"")),
                             cols.join(QLatin1Char(',')), placeholders.join(QLatin1Char(','))));
                for (const QVariant& v : vals)
                    restoreQ.addBindValue(v);
                if (!restoreQ.exec()) {
                    if (err)
                        *err = QStringLiteral(
                                   "E_SYNC_APPLY_CONSTRAINT: failed to restore high-rank "
                                   "row on %1: %2")
                                   .arg(tableName, restoreQ.lastError().text());
                    ok = false;
                    break;
                }
                // Restore succeeded — the winner entry is unchanged and remains authoritative.
            }
            continue;  // do not update winner with DELETE info
        }

        // INSERT / UPDATE: track applied writes and store row content for recovery.
        QByteArray contentH =
            QCryptographicHash::hash(contentMaterial, QCryptographicHash::Sha256).left(16);

        // C-08 fix: use real column names (not indices) so the DELETE recovery path can build
        // a valid INSERT SQL. Fetch column name order from PRAGMA table_info keyed by 'cid'.
        QStringList colNames;
        {
            QSqlQuery ti(wconn);
            ti.prepare(
                QStringLiteral("PRAGMA table_info(\"%1\")")
                    .arg(QString(tableName).replace(QLatin1Char('"'), QLatin1String("\"\""))));
            if (ti.exec()) {
                QMap<int, QString> cidToName;
                while (ti.next())
                    cidToName.insert(ti.value(0).toInt(), ti.value(1).toString());
                for (int i = 0; i < nCol; ++i)
                    colNames.append(cidToName.value(i, QStringLiteral("_col_%1").arg(i)));
            }
        }

        QJsonObject rowJson;
        for (int ci = 0; ci < nCol; ++ci) {
            sqlite3_value* newVal = nullptr;
            sqlite3changeset_new(iter, ci, &newVal);
            if (!newVal)
                continue;
            // Use real column name; fall back to "col_N" if PRAGMA failed.
            const QString colKey =
                (ci < colNames.size()) ? colNames[ci] : QStringLiteral("col_%1").arg(ci);
            switch (sqlite3_value_type(newVal)) {
                case SQLITE_TEXT:
                    rowJson[colKey] = QString::fromUtf8(
                        reinterpret_cast<const char*>(sqlite3_value_text(newVal)));
                    break;
                case SQLITE_INTEGER:
                    rowJson[colKey] = static_cast<double>(sqlite3_value_int64(newVal));
                    break;
                case SQLITE_FLOAT:
                    rowJson[colKey] = sqlite3_value_double(newVal);
                    break;
                default:
                    rowJson[colKey] = QJsonValue::Null;
            }
        }

        RowWinner challenger;
        challenger.origin = origin;
        challenger.rank = rank;
        challenger.originSeq = seq;
        challenger.contentHash = contentH;
        challenger.winningContent =
            QString::fromUtf8(QJsonDocument(rowJson).toJson(QJsonDocument::Compact));

        RowWinner incumbent = winners.get(wconn, tableName, pkHashStr);
        const bool shouldUpdate = (incumbent.rank == INT_MIN) || (rank > incumbent.rank) ||
                                  (rank == incumbent.rank && seq >= incumbent.originSeq);

        if (shouldUpdate)
            winners.put(wconn, tableName, pkHashStr, challenger, nullptr);
    }
    sqlite3changeset_finalize(iter);
    return ok;
}

}  // namespace dbridge::sync
