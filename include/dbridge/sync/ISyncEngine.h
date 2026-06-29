#pragma once
#include "dbridge/Export.h"
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncSelection.h"
#include "dbridge/sync/SyncTypes.h"

#include <memory>

namespace dbridge {
class DataBridge;
}

namespace dbridge::sync {

class DBRIDGE_EXPORT ISyncEngine {
   public:
    virtual ~ISyncEngine() = default;

    // ① Initialize: load config, eligibility check, session attach
    virtual bool initialize(const SyncConfig& config, QString* err = nullptr) = 0;

    // ② Manual drain: scan inbox + pack outbox
    virtual bool sync(QString* err = nullptr) = 0;

    // ③ Cooperative stop of current foreground operation
    virtual bool stop(QString* err = nullptr) = 0;

    // ④ Foreground operation state snapshot
    virtual SyncState state() const = 0;

    // ⑤ Progress snapshot
    virtual SyncProgress progress() const = 0;

    // ⑥ Log ring
    virtual QList<SyncLogEntry> logs() const = 0;

    // ⑦ Error ring
    virtual QList<SyncError> errors() const = 0;

    // ⑧ Result of last completed operation
    virtual SyncResult result() const = 0;

    // ⑨ Upstream selective push (FR-17)
    virtual bool syncSelected(const SyncSelection& selection, QString* err = nullptr) = 0;

    // ⑩ Session-captured write: executes mutations through the session recorder so that
    //    every row change is tracked by the SQLite changeset and will be packaged into the
    //    outbox on the next sync() call.  Use this instead of direct SQL to ensure changes
    //    are propagated to peers.
    virtual bool write(const QList<RowMutation>& mutations, QString* err = nullptr) = 0;
};

DBRIDGE_EXPORT std::unique_ptr<ISyncEngine> createSyncEngine(DataBridge& bridge);

}  // namespace dbridge::sync
