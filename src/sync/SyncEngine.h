#pragma once
#include "dbridge/sync/ISyncEngine.h"
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncTypes.h"

#include <QMutex>

#include "SyncContext.h"
#include "SyncWorker.h"
#include <memory>

namespace dbridge {
class DataBridge;
}

namespace dbridge::sync {

class SyncEngine : public ISyncEngine {
   public:
    explicit SyncEngine(DataBridge& bridge);
    ~SyncEngine() override;

    bool initialize(const SyncConfig& config, QString* err = nullptr) override;
    bool sync(QString* err = nullptr) override;
    bool stop(QString* err = nullptr) override;

    SyncState state() const override;
    SyncProgress progress() const override;
    QList<SyncLogEntry> logs() const override;
    QList<SyncError> errors() const override;
    SyncResult result() const override;

    bool syncSelected(const SyncSelection& selection, QString* err = nullptr) override;

   private:
    // Helpers
    void appendLog(Severity sev, const QString& phase, const QString& msg);
    void appendError(const SyncError& e);
    void setProgress(SyncState st, int pct = -1);
    void onWorkerProgress(SyncProgress p);
    void onWorkerError(SyncError e);
    void releaseGateIfTerminal(SyncState state);

    DataBridge& bridge_;
    std::unique_ptr<SyncConfig> configPtr_;  // delayed init via initialize()
    std::shared_ptr<SyncContext> ctx_;
    std::unique_ptr<SyncWorker> worker_;
    bool initialized_ = false;
    QString canonicalKey_;

    mutable QMutex snapMutex_;
    SyncProgress progress_;
    SyncResult result_;
    QList<SyncLogEntry> logs_;
    QList<SyncError> errors_;
};

}  // namespace dbridge::sync
