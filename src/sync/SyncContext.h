#pragma once
#include "dbridge/sync/SyncConfig.h"

#include <QMutex>
#include <QSqlDatabase>
#include <QString>
#include <QThread>

#include "ForegroundGate.h"
#include <memory>
#include <optional>

namespace dbridge::sync {

// Holds shared state for one physical SQLite database across all SyncEngine / BatchTransfer
// instances that share the same file (keyed by OS dev+inode, G-07).
struct SyncContext {
    std::optional<SyncConfig> config;  // set by SyncEngine::initialize()
    ForegroundGate gate;
    QSqlDatabase wconn;  // the single write connection owned by SyncWorker
    QString wconnName;
    QString contextUuid;  // stored in __sync_context_uuid for double-checking

    // Reference count; context is destroyed when refCount reaches 0.
    int refCount = 0;
};

// Process-wide singleton registry of SyncContext objects.
class SyncContextRegistry {
   public:
    static SyncContextRegistry& instance();

    // Open or retrieve context for the SQLite file at path.
    // Returns nullptr + sets *err on failure.
    std::shared_ptr<SyncContext> getOrCreate(const QString& sqlitePath, QString* err = nullptr);

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
