#include "InboxWatcher.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace dbridge::sync {

InboxWatcher::InboxWatcher(const QString& inboxDir, QSqlDatabase& db, InboxLedger& ledger,
                           QObject* parent)
    : QObject(parent), dir_(inboxDir), db_(db), ledger_(ledger) {
}

void InboxWatcher::start() {
    if (!watcher_) {
        watcher_ = new QFileSystemWatcher(this);
        watcher_->addPath(dir_);
        connect(watcher_, &QFileSystemWatcher::directoryChanged, this,
                &InboxWatcher::onDirectoryChanged);
    }
    if (!timer_) {
        timer_ = new QTimer(this);
        timer_->setInterval(10000);  // 10 s fallback
        connect(timer_, &QTimer::timeout, this, &InboxWatcher::onTimer);
        timer_->start();
    }
    // Startup scan
    scanInbox();
}

void InboxWatcher::stop() {
    if (timer_) {
        timer_->stop();
    }
    if (watcher_) {
        watcher_->removePaths(watcher_->directories());
    }
}

void InboxWatcher::onDirectoryChanged(const QString& /*path*/) {
    scanInbox();
}

void InboxWatcher::onTimer() {
    scanInbox();
}

void InboxWatcher::scanInbox() {
    QDir d(dir_);
    if (!d.exists())
        return;

    // Find all .ready markers.
    const QStringList readyFiles =
        d.entryList(QStringList{QStringLiteral("*.ready")}, QDir::Files, QDir::Name);

    for (const QString& readyName : readyFiles) {
        // Derive artifact name: strip the trailing ".ready"
        QString artifactName = readyName;
        if (artifactName.endsWith(QStringLiteral(".ready")))
            artifactName.chop(6);  // len(".ready") == 6

        // Check ledger: skip if already consumed or corrupt.
        LedgerStatus st = ledger_.status(db_, artifactName);
        if (st == LedgerStatus::Consumed || st == LedgerStatus::Corrupt)
            continue;

        // Mark as seen (idempotent).
        ledger_.markSeen(db_, artifactName, nullptr);

        const QString artifactPath = d.filePath(artifactName);
        if (QFileInfo::exists(artifactPath))
            emit artifactReady(artifactPath);
    }
}

}  // namespace dbridge::sync
