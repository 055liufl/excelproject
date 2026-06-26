#include "InboxWatcher.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace dbridge::sync {

InboxWatcher::InboxWatcher(const QString& inboxDir, QSqlDatabase& db, InboxLedger& ledger,
                           QObject* parent)
    : QObject(parent), dir_(inboxDir), db_(db), ledger_(ledger) {
}

// ---------------------------------------------------------------------------
// scan() — called directly on the worker thread (no event loop required).
// Returns newly discovered artifact paths (full paths, not just names).
// ---------------------------------------------------------------------------

QStringList InboxWatcher::scan(QSqlDatabase& db) {
    QStringList newArtifacts;

    QDir d(dir_);
    if (!d.exists())
        return newArtifacts;

    // Find all .ready markers, sorted oldest-first for FIFO processing.
    // M-08 fix: QDir::Reversed inverts the default QDir::Time (newest-first) order to
    // produce the oldest-first sequence the comment and the gap-detection code assume.
    const QStringList readyFiles = d.entryList(QStringList{QStringLiteral("*.ready")}, QDir::Files,
                                               QDir::Time | QDir::Reversed);

    for (const QString& readyName : readyFiles) {
        // Derive artifact name: strip the trailing ".ready" (6 chars)
        QString artifactName = readyName.left(readyName.length() - 6);

        // Check ledger: skip if already consumed or corrupt.
        LedgerStatus st = ledger_.status(db, artifactName);
        if (st == LedgerStatus::Consumed || st == LedgerStatus::Corrupt)
            continue;

        // Mark as seen (idempotent: no-op if already known).
        // M-5 fix: if markSeen fails, skip this artifact — processing it without a ledger entry
        // could cause it to be re-processed indefinitely on the next scan.
        if (st != LedgerStatus::Seen) {
            QString markErr;
            if (!ledger_.markSeen(db, artifactName, &markErr))
                continue;  // ledger write failed; skip this scan cycle
        }

        const QString artifactPath = d.filePath(artifactName);
        if (QFileInfo::exists(artifactPath))
            newArtifacts.append(artifactPath);
    }

    return newArtifacts;
}

}  // namespace dbridge::sync
