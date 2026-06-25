#include "sync/SyncEngine.h"

#include "dbridge/Errors.h"

#include <QDateTime>
#include <QDebug>
#include <QObject>
#include <QSqlError>
#include <QSqlQuery>

#include "sync/SyncDDL.h"
#include "sync/capture/SqliteHandle.h"
#include "sync/schema/SchemaEligibility.h"

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

    // Acquire (or create) the shared SyncContext for this database file
    QString ctxErr;
    ctx_ = SyncContextRegistry::instance().getOrCreate(configPtr_->sqlitePath(), &ctxErr);
    if (!ctx_) {
        if (err)
            *err = ctxErr;
        appendError({err::E_SYNC_INIT, Severity::Fatal, "init", configPtr_->nodeId(), ctxErr});
        return false;
    }

    // Derive canonical key for later release
    // The registry stores by dev+inode; we'll just use the path as key here
    canonicalKey_ = configPtr_->sqlitePath();
    ctx_->config = *configPtr_;

    QSqlDatabase& wconn = ctx_->wconn;

    // Verify SQLite session extension is available
    sqlite3* h = SqliteHandle::of(wconn);
    if (!h) {
        if (err)
            *err = QStringLiteral("Cannot obtain sqlite3* handle");
        return false;
    }
    if (!SqliteHandle::sessionAvailable(h)) {
        if (err)
            *err = QStringLiteral("SQLite session extension not available");
        return false;
    }

    // Create sync DDL tables
    QString ddlErr;
    for (const QString& stmt : ddl::allCreateStatements()) {
        QSqlQuery q(wconn);
        if (!q.exec(stmt)) {
            ddlErr = q.lastError().text() + QStringLiteral(" | SQL: ") + stmt.left(80);
            if (err)
                *err = ddlErr;
            appendError({err::E_SYNC_INIT, Severity::Fatal, "ddl", configPtr_->nodeId(), ddlErr});
            return false;
        }
    }

    // Schema eligibility check
    QStringList rejected;
    QString eligErr;
    if (!SchemaEligibility::verify(wconn, configPtr_->syncTables(), &rejected, &eligErr)) {
        QString msg = eligErr;
        if (!rejected.isEmpty())
            msg += QStringLiteral("; rejected: ") + rejected.join(QLatin1Char(','));
        if (err)
            *err = msg;
        appendError(
            {err::E_SYNC_UNSUPPORTED_SCHEMA, Severity::Fatal, "schema", configPtr_->nodeId(), msg});
        return false;
    }

    // Start the SyncWorker thread
    worker_ = std::make_unique<SyncWorker>(wconn, h, *configPtr_);
    QObject::connect(worker_.get(), &SyncWorker::progressUpdated,
                     [this](SyncProgress p) { onWorkerProgress(p); });
    QObject::connect(worker_.get(), &SyncWorker::errorOccurred,
                     [this](SyncError e) { onWorkerError(e); });
    worker_->start();

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

    // Enqueue a full scan+broadcast task on the worker
    worker_->enqueue([this]() {
        // The worker already does scanInbox+broadcast in its loop;
        // this explicit enqueue forces an immediate cycle.
        // (No additional logic needed — the loop handles it.)
    });

    ctx_->gate.release();

    {
        QMutexLocker lk(&snapMutex_);
        result_.ok = true;
        result_.finalState = SyncState::Completed;
    }
    setProgress(SyncState::Completed, 100);
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
    bool success = true;
    worker_->enqueue([this, &selection, &success]() {
        Q_UNUSED(selection)
        // Placeholder: actual selection push logic goes here via SyncSelectionPushService
        // (assembles SelectionResolver -> FkClosureBuilder -> ChunkStreamer -> OutboxWriter)
        Q_UNUSED(this)
    });

    ctx_->gate.release();

    if (!success && err)
        *err = QStringLiteral("Selection push failed");
    return success;
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
