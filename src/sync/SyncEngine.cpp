#include "sync/SyncEngine.h"

#include "dbridge/Errors.h"

#include <QDateTime>
#include <QDebug>
#include <QObject>

namespace dbridge::sync {

SyncEngine::SyncEngine(DataBridge& bridge) : bridge_(bridge) {
}

SyncEngine::~SyncEngine() {
    // J-09: Unblock direct imports before tearing down.
    bridge_.setSyncActive(false);
    if (worker_) {
        worker_->requestStop();
        worker_->wait(5000);
        worker_.reset();
    }
    if (ctx_) {
        SyncContextRegistry::instance().release(canonicalKey_);
        ctx_.reset();
    }
}

bool SyncEngine::initialize(const SyncConfig& config, QString* err) {
    if (initialized_) {
        if (err)
            *err = QStringLiteral("Already initialized");
        return false;
    }
    if (!config.isValid()) {
        if (err)
            *err = QStringLiteral("Invalid SyncConfig");
        return false;
    }

    configPtr_ = std::make_unique<SyncConfig>(config);
    setProgress(SyncState::Idle);

    // Acquire (or create) the shared SyncContext for this database file.
    // canonicalKey_ receives the dev+inode key used for release() (I-12 fix).
    QString ctxErr;
    ctx_ = SyncContextRegistry::instance().getOrCreate(configPtr_->sqlitePath(), &canonicalKey_,
                                                       &ctxErr);
    if (!ctx_) {
        if (err)
            *err = ctxErr;
        appendError({err::E_SYNC_INIT, Severity::Fatal, "init", configPtr_->nodeId(), ctxErr});
        return false;
    }
    ctx_->config = *configPtr_;

    // Start SyncWorker — it creates its own write connection in run() (I-01 / I-02 fix).
    worker_ = std::make_unique<SyncWorker>(*configPtr_);
    QObject::connect(worker_.get(), &SyncWorker::progressUpdated,
                     [this](SyncProgress p) { onWorkerProgress(p); });
    QObject::connect(worker_.get(), &SyncWorker::errorOccurred,
                     [this](SyncError e) { onWorkerError(e); });
    worker_->start();

    // Block until worker finishes initialisation (or times out / fails).
    if (!worker_->waitForInit(10000)) {
        const QString rawErr = worker_->initError();
        const QString displayErr =
            rawErr.isEmpty() ? QStringLiteral("Worker init timeout") : rawErr;
        if (err)
            *err = displayErr;
        // J-14: Propagate precise error code from worker initError prefix.
        QString errCode = QLatin1String(err::E_SYNC_INIT);
        if (rawErr.startsWith(QLatin1String("E_SYNC_SESSION_UNAVAILABLE")))
            errCode = QLatin1String(err::E_SYNC_SESSION_UNAVAILABLE);
        else if (rawErr.startsWith(QLatin1String("E_SYNC_UNSUPPORTED_SCHEMA")))
            errCode = QLatin1String(err::E_SYNC_UNSUPPORTED_SCHEMA);
        appendError(
            {errCode, Severity::Fatal, QStringLiteral("init"), configPtr_->nodeId(), displayErr});
        worker_->requestStop();
        worker_->wait(3000);
        worker_.reset();
        SyncContextRegistry::instance().release(canonicalKey_);
        ctx_.reset();
        return false;
    }
    if (!worker_->initError().isEmpty()) {
        const QString rawErr = worker_->initError();
        if (err)
            *err = rawErr;
        // J-14: Propagate precise error code from worker initError prefix.
        QString errCode = QLatin1String(err::E_SYNC_INIT);
        if (rawErr.startsWith(QLatin1String("E_SYNC_SESSION_UNAVAILABLE")))
            errCode = QLatin1String(err::E_SYNC_SESSION_UNAVAILABLE);
        else if (rawErr.startsWith(QLatin1String("E_SYNC_UNSUPPORTED_SCHEMA")))
            errCode = QLatin1String(err::E_SYNC_UNSUPPORTED_SCHEMA);
        appendError(
            {errCode, Severity::Fatal, QStringLiteral("init"), configPtr_->nodeId(), rawErr});
        worker_->requestStop();
        worker_->wait(3000);
        worker_.reset();
        SyncContextRegistry::instance().release(canonicalKey_);
        ctx_.reset();
        return false;
    }

    // I-04: Wire importFn so BatchTransfer can route imports through SyncWorker.
    // Profile/catalog snapshots are taken on the calling thread (safe) and forwarded to
    // the worker so it never touches DataBridge::db_ or catalog_ from the wrong thread.
    ctx_->importFn = [this](const QString& xlsxPath, const ImportOptions& opts,
                            const detail::ProfileSpec& profile,
                            const detail::SchemaCatalog& catalog) -> ImportResult {
        return worker_->submitImportSync(opts, xlsxPath, profile, catalog);
    };

    // J-09: Block direct DataBridge::importExcel() while sync is active.
    bridge_.setSyncActive(true);

    initialized_ = true;
    appendLog(Severity::Info, QStringLiteral("init"),
              QStringLiteral("SyncEngine initialized for node ") + configPtr_->nodeId());
    return true;
}

bool SyncEngine::sync(QString* err) {
    if (!initialized_) {
        if (err)
            *err = QStringLiteral("Not initialized");
        return false;
    }
    // Acquire the foreground gate so at most one manual sync runs at a time
    if (!ctx_->gate.tryAcquire(err))
        return false;

    setProgress(SyncState::Importing);

    // Enqueue a full scan+broadcast task on the worker.
    // I-14 fix: gate is released inside the worker task.
    // I-19: Start ACK deadline timer so worker emits E_SYNC_ACK_TIMEOUT if no ACK arrives.
    // J-02: Stay in Exporting (percent=-1) until ACK arrives or timeout.
    // The worker task triggers a broadcast cycle; ACK arrival or timeout drives state to
    // Completed/Failed via onWorkerError (E_SYNC_ACK_TIMEOUT) in the main loop.
    setProgress(SyncState::Exporting, -1);
    // C-02 fix: enqueue a real manual drain — scanInbox + broadcast before arming ACK timer.
    worker_->enqueueDrain();
    worker_->startAckWait();  // arm timeout AFTER drain is queued
    // Gate released by ACK arrival (Completed) or timeout (Failed) via onWorkerError.
    return true;
}

bool SyncEngine::stop(QString* err) {
    if (!initialized_)
        return true;
    if (worker_) {
        worker_->requestStop();
        bool finished = worker_->wait(3000);
        if (!finished) {
            if (err)
                *err = QStringLiteral("Worker did not stop in time");
            return false;
        }
    }
    setProgress(SyncState::Stopped);
    return true;
}

SyncState SyncEngine::state() const {
    QMutexLocker lk(&snapMutex_);
    return progress_.state;
}

SyncProgress SyncEngine::progress() const {
    QMutexLocker lk(&snapMutex_);
    return progress_;
}

QList<SyncLogEntry> SyncEngine::logs() const {
    QMutexLocker lk(&snapMutex_);
    return logs_;
}

QList<SyncError> SyncEngine::errors() const {
    QMutexLocker lk(&snapMutex_);
    return errors_;
}

SyncResult SyncEngine::result() const {
    QMutexLocker lk(&snapMutex_);
    return result_;
}

bool SyncEngine::syncSelected(const SyncSelection& selection, QString* err) {
    if (!initialized_) {
        if (err)
            *err = QStringLiteral("Not initialized");
        return false;
    }
    if (!ctx_->gate.tryAcquire(err))
        return false;

    // C-01 fix: snapshot the catalog on the calling thread (safe), then enqueue the full
    // SelectionResolver → FkClosureBuilder → ChunkStreamer → OutboxWriter chain on the worker.
    detail::SchemaCatalog catalogSnapshot;
    QString snapErr;
    if (!bridge_.snapshotProfileCatalog(QString(), nullptr, &catalogSnapshot, &snapErr)) {
        ctx_->gate.release();
        if (err)
            *err = QStringLiteral("Cannot snapshot schema catalog: ") + snapErr;
        return false;
    }

    setProgress(SyncState::Capturing);
    worker_->startAckWait();  // arm ACK deadline before enqueueing so no race window
    worker_->enqueueSelectionPush(selection, catalogSnapshot);
    // Gate stays held until ACK (Completed) or timeout (Failed) via onWorkerError.
    return true;
}

// --- Private helpers ---

void SyncEngine::appendLog(Severity sev, const QString& phase, const QString& msg) {
    SyncLogEntry e;
    e.epochMs = QDateTime::currentMSecsSinceEpoch();
    e.severity = sev;
    e.phase = phase;
    e.message = msg;

    QMutexLocker lk(&snapMutex_);
    constexpr int kMaxLogs = 500;
    if (logs_.size() >= kMaxLogs)
        logs_.removeFirst();
    logs_.append(e);
}

void SyncEngine::appendError(const SyncError& e) {
    QMutexLocker lk(&snapMutex_);
    constexpr int kMaxErrors = 200;
    if (errors_.size() >= kMaxErrors)
        errors_.removeFirst();
    errors_.append(e);
}

void SyncEngine::setProgress(SyncState st, int pct) {
    QMutexLocker lk(&snapMutex_);
    progress_.state = st;
    progress_.percent = pct;
}

void SyncEngine::onWorkerProgress(SyncProgress p) {
    QMutexLocker lk(&snapMutex_);
    progress_ = p;
}

void SyncEngine::onWorkerError(SyncError e) {
    appendError(e);
    appendLog(e.severity, e.phase, e.message);

    // H-09 fix: Fatal/Error from worker that represents a terminal foreground failure must
    // transition the foreground state to Failed and release the gate.  Without this the
    // caller would see state() == Exporting forever after e.g. E_SYNC_ACK_TIMEOUT.
    if (e.severity == Severity::Fatal || e.severity == Severity::Error) {
        QMutexLocker lk(&snapMutex_);
        if (progress_.state == SyncState::Exporting || progress_.state == SyncState::Capturing) {
            progress_.state = SyncState::Failed;
            progress_.percent = 0;
            result_.ok = false;
            result_.errors.append(e);
        }
    }
    // Gate release is handled by the worker emitting its own "operation done" signal;
    // we intentionally do NOT release here to avoid double-release races.
}

// --- Factory ---
std::unique_ptr<ISyncEngine> createSyncEngine(DataBridge& bridge) {
    return std::make_unique<SyncEngine>(bridge);
}

}  // namespace dbridge::sync
