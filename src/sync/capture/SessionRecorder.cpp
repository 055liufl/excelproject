#include "SessionRecorder.h"

#include <QByteArray>

namespace dbridge::sync {

bool SessionRecorder::begin(sqlite3* h, const QStringList& syncTables, QString* err) {
    if (session_) {
        if (err)
            *err = QStringLiteral("SessionRecorder already active");
        return false;
    }
    if (!h) {
        if (err)
            *err = QStringLiteral("null sqlite3 handle");
        return false;
    }

    int rc = sqlite3session_create(h, "main", &session_);
    if (rc != SQLITE_OK) {
        session_ = nullptr;
        if (err)
            *err = QStringLiteral("sqlite3session_create failed: %1").arg(sqlite3_errmsg(h));
        return false;
    }

    // Attach each sync table to the session.
    for (const QString& tbl : syncTables) {
        const QByteArray tblUtf8 = tbl.toUtf8();
        rc = sqlite3session_attach(session_, tblUtf8.constData());
        if (rc != SQLITE_OK) {
            sqlite3session_delete(session_);
            session_ = nullptr;
            if (err)
                *err = QStringLiteral("sqlite3session_attach failed for '%1': %2")
                           .arg(tbl)
                           .arg(sqlite3_errmsg(h));
            return false;
        }
    }
    return true;
}

bool SessionRecorder::sealInto(sqlite3* h, ChangelogStore& store, QSqlDatabase& db, WriteTxn& txn,
                               const QString& origin, qint64 epoch, qint64 schemaVer,
                               const QString& schemaFp, qint64 parentSeq, qint64 originSeq,
                               qint64* outLocalSeq, QString* err, const QString& pushId) {
    if (!session_) {
        if (err)
            *err = QStringLiteral("SessionRecorder not active");
        return false;
    }
    if (!txn.isActive()) {
        if (err)
            *err = QStringLiteral("WriteTxn not active during sealInto");
        return false;
    }

    QByteArray changeset = collectChangeset(err);
    if (changeset.isNull()) {
        // collectChangeset sets *err
        abort();
        return false;
    }

    // Detach session before writing to avoid re-recording the changelog INSERT.
    sqlite3session_delete(session_);
    session_ = nullptr;

    if (changeset.isEmpty()) {
        // No changes captured; still report success with seq=0.
        if (outLocalSeq)
            *outLocalSeq = 0;
        return true;
    }

    bool ok = store.append(db, QStringLiteral("changeset"), origin,
                           /*sourcePeer=*/QString(), originSeq, parentSeq, epoch, schemaVer,
                           schemaFp, changeset,
                           /*authoritative=*/true, outLocalSeq, err,
                           /*pushId=*/pushId);  // H-01 fix: forward pushId so selection-push
                                                // changesets have push_id recorded in changelog
    Q_UNUSED(h);
    return ok;
}

void SessionRecorder::abort() {
    if (session_) {
        sqlite3session_delete(session_);
        session_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// private
// ---------------------------------------------------------------------------

QByteArray SessionRecorder::collectChangeset(QString* err) {
    int nChangeset = 0;
    void* pChangeset = nullptr;

    // sqlite3session_changeset outputs a patchset / changeset blob.
    int rc = sqlite3session_changeset(session_, &nChangeset, &pChangeset);
    if (rc != SQLITE_OK) {
        if (err)
            *err = QStringLiteral("sqlite3session_changeset failed: rc=%1").arg(rc);
        return QByteArray();  // null = error
    }

    if (nChangeset == 0 || pChangeset == nullptr) {
        // H-06 fix: distinguish "no changes captured" from "error" (both returned QByteArray()
        // before). Return a non-null empty QByteArray so the caller can use isNull() for
        // error-detection and isEmpty() for the no-changes fast-path.
        return QByteArray("");  // non-null empty = no changes, not an error
    }

    QByteArray result(static_cast<const char*>(pChangeset), nChangeset);
    sqlite3_free(pChangeset);
    return result;
}

}  // namespace dbridge::sync
