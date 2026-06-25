#include "sync/SyncEngine.h"

#include "dbridge/Errors.h"

#include <QDateTime>
#include <QDebug>
#include <QObject>

namespace dbridge::sync {

SyncEngine::SyncEngine(DataBridge& bridge) : bridge_(bridge) {
}

SyncEngine::~SyncEngine() {
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
        QString workerErr = worker_->initError();
        if (err)
            *err = workerErr.isEmpty() ? QStringLiteral("Worker init timeout") : workerErr;
        appendError({err::E_SYNC_INIT, Severity::Fatal, "init", configPtr_->nodeId(),
                     err ? *err : QString()});
        worker_->requestStop();
        worker_->wait(3000);
        worker_.reset();
        SyncContextRegistry::instance().release(canonicalKey_);
        ctx_.reset();
        return false;
    }
    if (!worker_->initError().isEmpty()) {
        if (err)
            *err = worker_->initError();
        appendError({err::E_SYNC_INIT, Severity::Fatal, "init", configPtr_->nodeId(),
                     worker_->initError()});
        worker_->requestStop();
        worker_->wait(3000);
        worker_.reset();
        SyncContextRegistry::instance().release(canonicalKey_);
        ctx_.reset();
        return false;
    }

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
    // I-14 fix: gate is released inside the worker task (not here in sync()),
    // so the caller sees state transition happen after the worker has processed the request.
    // I-19 note: ACK timeout (E_SYNC_ACK_TIMEOUT) would be emitted here once the
    // M1c ACK state machine is implemented; for now it remains a placeholder.
    worker_->enqueue([this]() {
        // The worker loop already calls scanInbox + broadcast on each iteration;
        // this task signals that a foreground sync request was received.
        // Full scan+ACK-wait state machine is deferred to M1c.
        ctx_->gate.release();
        setProgress(SyncState::Completed, 100);
        QMutexLocker lk(&snapMutex_);
        result_.ok = true;
        result_.finalState = SyncState::Completed;
    });
    // Note: gate is released inside the enqueued task above; do NOT release here.
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
