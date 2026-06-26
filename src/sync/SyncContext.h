#pragma once
#include "dbridge/Types.h"
#include "dbridge/sync/SyncConfig.h"

#include <QMutex>
#include <QSqlDatabase>
#include <QString>

#include "ForegroundGate.h"
#include "diff/InboundTableGate.h"
#include "profile/ProfileSpec.h"
#include "schema/SchemaCatalog.h"
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
    std::shared_ptr<InboundTableGate> inboundTableGate = std::make_shared<InboundTableGate>();
    QString contextUuid;  // stored in __sync_context_uuid for double-checking

    // Reference count; context is destroyed when refCount reaches 0.
    int refCount = 0;

    // I-04: Set by SyncEngine after worker init. BatchTransfer calls this to route
    // imports through SyncWorker (wconn + session capture). Null when sync not active.
    // Signature: (xlsxPath, options, profile, catalog) -> ImportResult.
    // The profile/catalog snapshots are copied before dispatch so the worker never touches the
    // DataBridge-owned QSqlDatabase or mutable catalog.
    std::function<ImportResult(const QString&, const ImportOptions&,
                               const dbridge::detail::ProfileSpec&,
                               const dbridge::detail::SchemaCatalog&)>
        importFn;

    // Runs a synchronous write task on SyncWorker's single writer thread.
    std::function<bool(const std::function<bool(QSqlDatabase&, QString*)>&, QString*)>
        workerWriteFn;

    // Requests an inbox rescan after a comparison gate is released.
    std::function<void()> rescanFn;

    // L-01 fix: canonical sync table list (expanded from empty = all user tables).
    // Populated by SyncWorker after initialization so all modules share the same set.
    QStringList canonicalSyncTables;

    // H-13 fix: the worker's active stream epoch. Published by SyncWorker after init so the
    // ComparisonSession factory can read local __sync_table_state with the correct epoch
    // (instead of the placeholder 0, which made tableDiffs() read state as "not found").
    qint64 streamEpoch = 0;
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

    // J-10: Returns the existing context for path without incrementing refCount and without
    // creating a new entry. Returns nullptr if no context is registered for this path.
    // Caller must NOT call release() for a pointer obtained via getExisting().
    std::shared_ptr<SyncContext> getExisting(const QString& sqlitePath);

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
