#pragma once
#include <QFileSystemWatcher>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QTimer>

#include "InboxLedger.h"

namespace dbridge::sync {

// Watches an inbox directory for .ready marker files and emits artifactReady()
// for each new, unconsumed artifact path.
// Three discovery paths:
//   1. Startup scan (start())
//   2. QFileSystemWatcher directory-change events
//   3. Periodic timer (every 10 s as fallback)
class InboxWatcher : public QObject {
    Q_OBJECT
   public:
    explicit InboxWatcher(const QString& inboxDir, QSqlDatabase& db, InboxLedger& ledger,
                          QObject* parent = nullptr);

    void start();
    void stop();

   signals:
    // Emitted for each artifact path that is ready to consume.
    void artifactReady(const QString& artifactPath);

   private slots:
    void onDirectoryChanged(const QString& path);
    void onTimer();

   private:
    void scanInbox();

    QString dir_;
    QSqlDatabase& db_;
    InboxLedger& ledger_;
    QFileSystemWatcher* watcher_ = nullptr;
    QTimer* timer_ = nullptr;
};

}  // namespace dbridge::sync
