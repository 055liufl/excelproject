#include "ChangesetApplier.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QString>

#include <sqlite3.h>

namespace dbridge::sync {

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
            sqlite3changeset_op(iter, &tblName, nullptr, nullptr, nullptr);
            const QString table = QString::fromUtf8(tblName ? tblName : "");

            // Build a minimal pkHash from the primary key values in the iterator.
            // We use the conflict-side new values as the content fingerprint.
            // For simplicity we derive pkHash from the primary-key columns.
            int nCol = 0;
            int opOut = 0;
            int bIndirectOut = 0;
            sqlite3changeset_op(iter, nullptr, &nCol, &opOut, &bIndirectOut);

            // Gather PK values from changeset iterator (new values).
            QByteArray pkMaterial;
            for (int i = 0; i < nCol; i++) {
                sqlite3_value* newVal = nullptr;
                sqlite3changeset_new(iter, i, &newVal);
                if (!newVal)
                    continue;
                int vtype = sqlite3_value_type(newVal);
                if (vtype == SQLITE_TEXT) {
                    const char* txt = reinterpret_cast<const char*>(sqlite3_value_text(newVal));
                    pkMaterial.append(txt ? txt : "");
                } else if (vtype == SQLITE_INTEGER) {
                    pkMaterial.append(
                        QByteArray::number(static_cast<qint64>(sqlite3_value_int64(newVal))));
                } else if (vtype == SQLITE_BLOB) {
                    const void* blobPtr = sqlite3_value_blob(newVal);
                    int blobLen = sqlite3_value_bytes(newVal);
                    if (blobPtr && blobLen > 0)
                        pkMaterial.append(static_cast<const char*>(blobPtr), blobLen);
                }
                pkMaterial.append('\0');
            }
            QByteArray pkH =
                QCryptographicHash::hash(pkMaterial, QCryptographicHash::Sha256).left(16);
            const QString pkHashStr = QString::fromLatin1(pkH.toHex());

            RowWinner challenger;
            challenger.origin = c->origin;
            challenger.rank = c->rank;
            challenger.originSeq = c->seq;
            challenger.contentHash = pkH;  // rough proxy; caller may refine later

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

}  // namespace dbridge::sync
