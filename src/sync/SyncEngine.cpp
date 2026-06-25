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
    ctx_->importFn = [this](const QString& xlsxPath, const ImportOptions& opts) -> ImportResult {
        return worker_->submitImportSync(bridge_, opts, xlsxPath);
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
    worker_->startAckWait();
    worker_->enqueue([this]() {
        // Trigger an immediate broadcast cycle (worker loop would do it on next interval).
        // Do NOT set Completed here — that happens when ACK arrives.
        // DO release gate here so the calling thread is unblocked; state stays Exporting.
        ctx_->gate.release();
        // No Completed yet: caller polls state() and waits for ACK-driven transition.
    });
    // Note: gate released inside worker task; do NOT release here.
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

    // Enqueue selection push resolution on the worker thread
    // (full implementation would call SelectionResolver + FkClosureBuilder + ChunkStreamer)
    // I-05 fix: capture selection by value to avoid dangling reference after return.
    worker_->enqueue([this, sel = selection]() {
        Q_UNUSED(sel)
        // Placeholder: actual selection push logic goes here via SyncSelectionPushService
        // (assembles SelectionResolver -> FkClosureBuilder -> ChunkStreamer -> OutboxWriter)
        Q_UNUSED(this)
    });

    ctx_->gate.release();
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
}

// --- Factory ---
std::unique_ptr<ISyncEngine> createSyncEngine(DataBridge& bridge) {
    return std::make_unique<SyncEngine>(bridge);
}

}  // namespace dbridge::sync
