#include "sync/SyncEngine.h"

#include "dbridge/Errors.h"

#include <QDateTime>
#include <QDebug>
#include <QFileInfo>
#include <QObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace dbridge::sync {

SyncEngine::SyncEngine(DataBridge& bridge) : bridge_(bridge) {
}

SyncEngine::~SyncEngine() {
    // J-09: Unblock direct imports before tearing down.
    bridge_.setSyncActive(false);
    if (ctx_) {
        ctx_->importFn = nullptr;
        ctx_->workerWriteFn = nullptr;
        ctx_->workerCaptureWriteFn = nullptr;
        ctx_->rescanFn = nullptr;
    }
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

    // H-02 fix: resolve the SQLite main library's actual path via PRAGMA database_list
    // before calling getOrCreate(), so URI paths, relative paths, and platform-specific
    // aliases all map to the same OS file identity (dev+inode / volume+fileindex).
    QString resolvedPath = configPtr_->sqlitePath();
    {
        const QString tmpConn = QStringLiteral("dbridge_se_resolve_") +
                                QUuid::createUuid().toString(QUuid::WithoutBraces);
        {
            QSqlDatabase tmp = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), tmpConn);
            tmp.setDatabaseName(configPtr_->sqlitePath());
            if (tmp.open()) {
                QSqlQuery q(tmp);
                if (q.exec(QStringLiteral("PRAGMA database_list"))) {
                    while (q.next()) {
                        if (q.value(1).toString() == QLatin1String("main")) {
                            const QString p = q.value(2).toString();
                            if (!p.isEmpty())
                                resolvedPath = QFileInfo(p).absoluteFilePath();
                            break;
                        }
                    }
                }
                tmp.close();
            }
        }
        QSqlDatabase::removeDatabase(tmpConn);
    }

    // Acquire (or create) the shared SyncContext for this database file.
    // canonicalKey_ receives the dev+inode key used for release() (I-12 fix).
    QString ctxErr;
    ctx_ = SyncContextRegistry::instance().getOrCreate(resolvedPath, &canonicalKey_, &ctxErr);
    if (!ctx_) {
        if (err)
            *err = ctxErr;
        appendError({err::E_SYNC_INIT, Severity::Fatal, "init", configPtr_->nodeId(), ctxErr});
        return false;
    }
    ctx_->config = *configPtr_;

    // Start SyncWorker — it creates its own write connection in run() (I-01 / I-02 fix).
    worker_ = std::make_unique<SyncWorker>(*configPtr_, ctx_->inboundTableGate);
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
        ctx_->importFn = nullptr;
        ctx_->workerWriteFn = nullptr;
        ctx_->workerCaptureWriteFn = nullptr;
        ctx_->rescanFn = nullptr;
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
        ctx_->importFn = nullptr;
        ctx_->workerWriteFn = nullptr;
        ctx_->workerCaptureWriteFn = nullptr;
        ctx_->rescanFn = nullptr;
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
    ctx_->workerWriteFn = [this](const std::function<bool(QSqlDatabase&, QString*)>& task,
                                 QString* taskErr) {
        return worker_ && worker_->submitWriteSync(task, taskErr);
    };
    // C-05 fix: route comparison-session saves through CapturedWriteTemplate so writes are
    // session-captured and broadcast to peers like normal local writes.
    ctx_->workerCaptureWriteFn = [this](const QList<RowMutation>& mutations,
                                        const QStringList& syncTables, QString* captureErr) {
        return worker_ && worker_->submitCaptureWriteSync(mutations, syncTables, captureErr);
    };
    ctx_->rescanFn = [this]() {
        if (worker_)
            worker_->requestRescan();
    };

    // J-09: Block direct DataBridge::importExcel() while sync is active.
    // M-02: pass sync-monitored tables so importExcel() can allow non-sync profiles through.
    bridge_.setSyncActive(true, configPtr_->syncTables());

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

    setProgress(SyncState::Exporting, -1);
    // C-1 fix: arm ACK wait BEFORE enqueueDrain to close the race window where an ACK arrives
    // between broadcast completion and startAckWait() — which would leave ackWaiting_ true
    // forever (or until timeout).  If no payload is actually sent, cancelAckWait() clears the
    // flag before any timeout can fire.
    worker_->startAckWait();
    QString drainErr;
    const bool hasPayload = worker_->enqueueDrain(&drainErr);
    if (!drainErr.isEmpty()) {
        worker_->cancelAckWait();
        appendError({err::E_SYNC_TRANSPORT, Severity::Error, QStringLiteral("sync"),
                     configPtr_->nodeId(), drainErr});
        setProgress(SyncState::Failed, 0);
        ctx_->gate.release();
        if (err)
            *err = drainErr;
        return false;
    }
    if (!hasPayload) {
        // Nothing was broadcast — no ACK will come.
        worker_->cancelAckWait();
        setProgress(SyncState::Completed, 100);
        ctx_->gate.release();
        return true;
    }
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
    releaseGateIfTerminal(SyncState::Stopped);  // H-02 fix: gate must be released on stop
    // M-10 fix: clear context-bound worker callbacks so a surviving ComparisonSession (which may
    // still hold the shared SyncContext) cannot dispatch save()/rescan() to a stopped worker.
    if (ctx_) {
        ctx_->importFn = nullptr;
        ctx_->workerWriteFn = nullptr;
        ctx_->workerCaptureWriteFn = nullptr;
        ctx_->rescanFn = nullptr;
    }
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
    if (!bridge_.snapshotCatalog(&catalogSnapshot, &snapErr)) {
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
    {
        QMutexLocker lk(&snapMutex_);
        progress_ = p;
        if (p.state == SyncState::Completed || p.state == SyncState::Failed ||
            p.state == SyncState::Stopped) {
            result_.finalState = p.state;
            result_.ok = (p.state == SyncState::Completed);
        }
    }
    releaseGateIfTerminal(p.state);
}

void SyncEngine::onWorkerError(SyncError e) {
    appendError(e);
    appendLog(e.severity, e.phase, e.message);

    if (e.severity == Severity::Fatal || e.severity == Severity::Error) {
        bool terminalFailure = false;
        QMutexLocker lk(&snapMutex_);
        if (progress_.state == SyncState::Exporting || progress_.state == SyncState::Capturing) {
            progress_.state = SyncState::Failed;
            progress_.percent = 0;
            result_.ok = false;
            result_.finalState = SyncState::Failed;
            result_.errors.append(e);
            terminalFailure = true;
        }
        if (terminalFailure)
            releaseGateIfTerminal(SyncState::Failed);
    }
}

void SyncEngine::releaseGateIfTerminal(SyncState state) {
    if (!ctx_)
        return;
    if (state == SyncState::Completed || state == SyncState::Failed || state == SyncState::Stopped)
        ctx_->gate.release();
}

// --- Factory ---
std::unique_ptr<ISyncEngine> createSyncEngine(DataBridge& bridge) {
    return std::make_unique<SyncEngine>(bridge);
}

}  // namespace dbridge::sync
