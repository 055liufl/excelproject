#include "ChangesetApplier.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

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
                             RowWinnerStore& winners, const ApplyOptions& opts, ApplyOutcome* out,
                             QString* err) {
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

    // For non-authoritative path we use apply_v2 with a rebase buffer.
    void* pRebase = nullptr;
    int nRebase = 0;

    int rc = sqlite3changeset_apply_v2(
        h, changeset.size(), const_cast<void*>(static_cast<const void*>(changeset.constData())),
        nullptr,  // xFilter (accept all tables)
        &conflictCb, &ctx, &pRebase, &nRebase,
        0  // flags
    );

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

    // Post-apply: update row_winner for all INSERT/UPDATE rows that were
    // applied without triggering a conflict callback (I-03).
    updateWinnersFromChangeset(changeset, origin, originRank, originSeq, winners, wconn);

    return true;
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

void ChangesetApplier::updateWinnersFromChangeset(const QByteArray& changeset,
                                                  const QString& origin, int rank, qint64 seq,
                                                  RowWinnerStore& winners, QSqlDatabase& wconn) {
    sqlite3_changeset_iter* iter = nullptr;
    int rc =
        sqlite3changeset_start(&iter, changeset.size(),
                               const_cast<void*>(static_cast<const void*>(changeset.constData())));
    if (rc != SQLITE_OK || !iter)
        return;

    while (sqlite3changeset_next(iter) == SQLITE_ROW) {
        const char* tbl = nullptr;
        int nCol = 0, op = 0, indirect = 0;
        sqlite3changeset_op(iter, &tbl, &nCol, &op, &indirect);

        // Only track applied writes — INSERT and UPDATE.
        if (op != SQLITE_INSERT && op != SQLITE_UPDATE)
            continue;

        unsigned char* pkMask = nullptr;
        sqlite3changeset_pk(iter, &pkMask, nullptr);

        QByteArray pkMaterial, contentMaterial;
        extractHashMaterials(iter, nCol, /*useNew=*/true, pkMask, &pkMaterial, &contentMaterial);

        const QString pkHashStr = computePkHashStr(pkMaterial, contentMaterial);
        const QString tableName = QString::fromUtf8(tbl ? tbl : "");
        QByteArray contentH =
            QCryptographicHash::hash(contentMaterial, QCryptographicHash::Sha256).left(16);

        RowWinner challenger;
        challenger.origin = origin;
        challenger.rank = rank;
        challenger.originSeq = seq;
        challenger.contentHash = contentH;

        RowWinner incumbent = winners.get(wconn, tableName, pkHashStr);
        const bool shouldUpdate = (incumbent.rank == INT_MIN) || (rank > incumbent.rank) ||
                                  (rank == incumbent.rank && seq > incumbent.originSeq);

        if (shouldUpdate)
            winners.put(wconn, tableName, pkHashStr, challenger, nullptr);
    }
    sqlite3changeset_finalize(iter);
}

}  // namespace dbridge::sync
