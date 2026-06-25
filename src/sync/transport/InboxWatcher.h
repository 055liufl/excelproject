#pragma once
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include "InboxLedger.h"

namespace dbridge::sync {

// Watches an inbox directory for .ready marker files.
//
// I-10 fix: SyncWorker uses QWaitCondition::wait() (not exec()), so QTimer and
// QFileSystemWatcher signals would never fire on the worker thread.  The previous
// signal-driven design has been replaced with a synchronous scan() method that
// the worker calls directly in its main loop.
//
// The artifactReady signal is retained as a forward-compatible hook for future
// event-loop-based workers, but it is NOT emitted by the current implementation.
class InboxWatcher : public QObject {
    Q_OBJECT
   public:
    explicit InboxWatcher(const QString& inboxDir, QSqlDatabase& db, InboxLedger& ledger,
                          QObject* parent = nullptr);

    // Synchronous scan: called directly on the worker thread.
    // Scans the inbox directory for *.ready files, updates the ledger for newly-seen
    // artifacts, and returns a list of full artifact file paths ready for processing.
    // db must be the worker thread's own QSqlDatabase.
    QStringList scan(QSqlDatabase& db);

   signals:
    // Reserved for future event-loop-based workers.  Not emitted by scan().
    void artifactReady(const QString& artifactPath);

   private:
    QString dir_;
    QSqlDatabase& db_;  // kept for legacy reference; scan() accepts an explicit db arg
    InboxLedger& ledger_;
};

}  // namespace dbridge::sync
