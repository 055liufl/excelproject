#pragma once
#include "dbridge/Types.h"
#include "dbridge/sync/SyncConfig.h"

#include <QMutex>
#include <QSqlDatabase>
#include <QString>

#include "ForegroundGate.h"
#include <functional>
#include <memory>
#include <optional>

namespace dbridge::sync {

// Holds shared state for one physical SQLite database across all SyncEngine / BatchTransfer
// instances that share the same file (keyed by OS dev+inode, G-07).
// Note: write connection is owned by SyncWorker (created in its run() thread), not here.
struct SyncContext {
    std::optional<SyncConfig> config;  // set by SyncEngine::initialize()
    ForegroundGate gate;
    QString contextUuid;  // stored in __sync_context_uuid for double-checking

    // Reference count; context is destroyed when refCount reaches 0.
    int refCount = 0;

    // I-04: Set by SyncEngine after worker init. BatchTransfer calls this to route
    // imports through SyncWorker (wconn + session capture). Null when sync not active.
    // Signature: (xlsxPath, options) -> ImportResult (blocking, runs on worker thread)
    std::function<ImportResult(const QString&, const ImportOptions&)> importFn;
};

// Process-wide singleton registry of SyncContext objects.
class SyncContextRegistry {
   public:
    static SyncContextRegistry& instance();

    // Open or retrieve context for the SQLite file at path.
    // On success, fills *canonicalKeyOut with the dev+inode key to use for release().
    // Returns nullptr + sets *err on failure.
    std::shared_ptr<SyncContext> getOrCreate(const QString& sqlitePath, QString* canonicalKeyOut,
                                             QString* err = nullptr);

    // Decrement ref; destroys context if refCount reaches 0.
    void release(const QString& canonicalKey);

   private:
    SyncContextRegistry() = default;

    // Compute canonical key: dev+inode on POSIX, volume+fileindex on Windows.
    static QString canonicalKey(const QString& path, QString* err = nullptr);

    QMutex mutex_;
    QHash<QString, std::shared_ptr<SyncContext>> registry_;
};

}  // namespace dbridge::sync
