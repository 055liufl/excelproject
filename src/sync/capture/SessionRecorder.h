#pragma once
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include "../WriteTxn.h"
#include "ChangelogStore.h"
#include <sqlite3.h>

namespace dbridge::sync {

// Records a short-lived sqlite3_session against a set of sync tables.
// Usage:
//   WriteTxn txn(db); txn.begin();
//   recorder.begin(h, tables);
//   // ... business writes ...
//   recorder.sealInto(h, store, db, txn, ...);
//   txn.commit();
//
// Requires SQLITE_ENABLE_SESSION + SQLITE_ENABLE_PREUPDATE_HOOK.
class SessionRecorder {
   public:
    SessionRecorder() = default;
    ~SessionRecorder() {
        abort();
    }

    // Non-copyable
    SessionRecorder(const SessionRecorder&) = delete;
    SessionRecorder& operator=(const SessionRecorder&) = delete;

    // Attach a new sqlite3_session tracking all syncTables.
    // Must be called after WriteTxn::begin() and before any business writes.
    bool begin(sqlite3* h, const QStringList& syncTables, QString* err);

    // Collect the changeset from the session, write it into the changelog,
    // and detach the session. Must be called within the same WriteTxn.
    // On success, *outLocalSeq is the assigned local_seq.
    // H-01 fix: pushId is forwarded to ChangelogStore::append so selection-push changesets
    // have their push_id recorded, enabling the broadcast barrier to block only the specific push.
    bool sealInto(sqlite3* h, ChangelogStore& store, QSqlDatabase& db, WriteTxn& txn,
                  const QString& origin, qint64 epoch, qint64 schemaVer, const QString& schemaFp,
                  qint64 parentSeq, qint64 originSeq, qint64* outLocalSeq, QString* err,
                  const QString& pushId = QString());

    // Detach and discard the session without writing.
    void abort();

    bool isActive() const {
        return session_ != nullptr;
    }

   private:
    sqlite3_session* session_ = nullptr;

    // Collect changeset bytes from session into a QByteArray.
    // Returns empty array on error.
    QByteArray collectChangeset(QString* err);
};

}  // namespace dbridge::sync
