#include "BatchTransfer.h"

#include <QDebug>
#include <QtConcurrent/QtConcurrent>

#include "sync/SyncContext.h"

namespace dbridge {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

BatchTransfer::BatchTransfer(DataBridge& bridge) : bridge_(bridge) {
}

BatchTransfer::~BatchTransfer() {
    // Best-effort graceful shutdown: signal both tasks and wait.
    importStopRequested_.store(true);
    exportStopRequested_.store(true);
    if (importFuture_.isRunning()) {
        importFuture_.waitForFinished();
    }
    if (exportFuture_.isRunning()) {
        exportFuture_.waitForFinished();
    }
}

// ---------------------------------------------------------------------------
// startImport
// ---------------------------------------------------------------------------

bool BatchTransfer::startImport(const ImportOptions& options, QString* err) {
    QMutexLocker lock(&mutex_);
    if (importState_ == TransferState::Running || importState_ == TransferState::Stopping) {
        if (err)
            *err = QStringLiteral("Import already running");
        return false;
    }

    // Reset all import state.
    importStopRequested_.store(false);
    importState_ = TransferState::Running;
    importErrors_.clear();
    importResult_ = ImportResult{};
    importProgress_ = TransferProgress{};
    lock.unlock();

    // QtConcurrent::run copies `options` into the lambda capture; safe.
    importFuture_ = QtConcurrent::run([this, options]() { runImport(options); });
    return true;
}

// ---------------------------------------------------------------------------
// startExport
// ---------------------------------------------------------------------------

bool BatchTransfer::startExport(const ExportOptions& options, QString* err) {
    QMutexLocker lock(&mutex_);
    if (exportState_ == TransferState::Running || exportState_ == TransferState::Stopping) {
        if (err)
            *err = QStringLiteral("Export already running");
        return false;
    }

    exportStopRequested_.store(false);
    exportState_ = TransferState::Running;
    exportErrors_.clear();
    exportResult_ = ExportResult{};
    exportProgress_ = TransferProgress{};
    lock.unlock();

    exportFuture_ = QtConcurrent::run([this, options]() { runExport(options); });
    return true;
}

// ---------------------------------------------------------------------------
// runImport  (runs on worker thread)
//
// Convention: opts.xlsxPath holds the xlsx file path.
//             opts.profileName is the import profile name (distinct from the file path).
//             opts.sheetName   is passed as-is to DataBridge (empty = profile default).
// ---------------------------------------------------------------------------

void BatchTransfer::runImport(const ImportOptions& opts) {
    // I-04: Route import through SyncWorker if sync is active for this database.
    // Check SyncContextRegistry for an active context; if importFn is set, the engine
    // has wired up a worker-thread import path with session capture.
    {
        const QString dbPath = bridge_.dbPath();
        if (!dbPath.isEmpty()) {
            QString ctxKey;
            auto ctx = sync::SyncContextRegistry::instance().getOrCreate(dbPath, &ctxKey, nullptr);
            if (ctx && ctx->importFn) {
                // Sync is active: run import on SyncWorker thread with session capture.
                // xlsxPath is carried in opts.profileName (design limitation; M3 adds dedicated
                // field).
                const QString xlsxPath = opts.profileName;
                ImportResult result = ctx->importFn(xlsxPath, opts);
                sync::SyncContextRegistry::instance().release(ctxKey);
                // Commit result and finish
                if (importStopRequested_.load()) {
                    QMutexLocker lock(&mutex_);
                    importResult_ = result;
                    importErrors_ = result.errors;
                    importProgress_ = TransferProgress{0, 0, -1};
                    importState_ = TransferState::Stopped;
                } else {
                    QMutexLocker lock(&mutex_);
                    importResult_ = result;
                    importErrors_ = result.errors;
                    importProgress_ = TransferProgress{result.ok ? 100 : 0,
                                                       static_cast<qint64>(result.writtenRows), -1};
                    importState_ = result.ok ? TransferState::Completed : TransferState::Failed;
                }
                return;
            }
            if (ctx)
                sync::SyncContextRegistry::instance().release(ctxKey);
        }
    }
    // Sync not active: fall back to direct DataBridge call (no session capture).

    // Report 0 % at the start.
    {
        QMutexLocker lock(&mutex_);
        importProgress_ = TransferProgress{0, 0, -1};
    }

    // Honour a stop request that arrived before we even started.
    if (importStopRequested_.load()) {
        QMutexLocker lock(&mutex_);
        importState_ = TransferState::Stopped;
        return;
    }

    // The synchronous DataBridge call does all the heavy lifting.
    // We cannot inject progress ticks into it, so we report 50 % while it
    // runs and 100 % when it finishes (or 0 % on early-stop).
    {
        QMutexLocker lock(&mutex_);
        importProgress_.percent = 50;
    }

    // opts.profileName is reused here as the xlsx file path (design smell: the field
    // serves dual purpose as both profile identifier and file path).  A dedicated
    // xlsxPath field should be added to ImportOptions in M3 when full SyncWorker
    // routing is implemented.
    const QString xlsxPath = opts.profileName;
    ImportResult result = bridge_.importExcel(xlsxPath, opts);

    // Check for stop request that arrived while the bridge call was in flight.
    if (importStopRequested_.load()) {
        QMutexLocker lock(&mutex_);
        importResult_ = result;  // preserve partial result
        importErrors_ = result.errors;
        importProgress_ = TransferProgress{0, 0, -1};
        importState_ = TransferState::Stopped;
        return;
    }

    // Commit results under the lock.
    {
        QMutexLocker lock(&mutex_);
        importResult_ = result;
        importErrors_ = result.errors;
        importProgress_.percent = 100;
        importProgress_.rowsDone = result.writtenRows;
        importProgress_.rowsTotal = result.readRows > 0 ? static_cast<qint64>(result.readRows) : -1;
        importState_ = result.ok ? TransferState::Completed : TransferState::Failed;
    }
}

// ---------------------------------------------------------------------------
// runExport  (runs on worker thread)
//
// Convention: opts.profileName is the xlsx file path.
// ---------------------------------------------------------------------------

void BatchTransfer::runExport(const ExportOptions& opts) {
    {
        QMutexLocker lock(&mutex_);
        exportProgress_ = TransferProgress{0, 0, -1};
    }

    if (exportStopRequested_.load()) {
        QMutexLocker lock(&mutex_);
        exportState_ = TransferState::Stopped;
        return;
    }

    {
        QMutexLocker lock(&mutex_);
        exportProgress_.percent = 50;
    }

    const QString xlsxPath = opts.profileName;
    ExportResult result = bridge_.exportExcel(xlsxPath, opts);

    if (exportStopRequested_.load()) {
        QMutexLocker lock(&mutex_);
        exportResult_ = result;
        exportErrors_ = result.errors;
        exportProgress_ = TransferProgress{0, 0, -1};
        exportState_ = TransferState::Stopped;
        return;
    }

    {
        QMutexLocker lock(&mutex_);
        exportResult_ = result;
        exportErrors_ = result.errors;
        exportProgress_.percent = 100;
        exportProgress_.rowsDone = result.writtenRows;
        exportProgress_.rowsTotal = static_cast<qint64>(result.writtenRows);
        exportState_ = result.ok ? TransferState::Completed : TransferState::Failed;
    }
}

// ---------------------------------------------------------------------------
// stop
// ---------------------------------------------------------------------------

bool BatchTransfer::stop(QString* err) {
    Q_UNUSED(err)

    // Signal both tasks.
    importStopRequested_.store(true);
    exportStopRequested_.store(true);

    // Transition any Running state to Stopping so callers can see we tried.
    {
        QMutexLocker lock(&mutex_);
        if (importState_ == TransferState::Running) {
            importState_ = TransferState::Stopping;
        }
        if (exportState_ == TransferState::Running) {
            exportState_ = TransferState::Stopping;
        }
    }

    // Wait for both futures to finish.  The worker threads check the atomic
    // flag and exit early; if they have already finished this is a no-op.
    if (importFuture_.isRunning()) {
        importFuture_.waitForFinished();
    }
    if (exportFuture_.isRunning()) {
        exportFuture_.waitForFinished();
    }

    return true;
}

// ---------------------------------------------------------------------------
// Polling getters  (locked snapshots)
// ---------------------------------------------------------------------------

TransferProgress BatchTransfer::importProgress() const {
    QMutexLocker lock(&mutex_);
    return importProgress_;
}

QList<RowError> BatchTransfer::importErrors() const {
    QMutexLocker lock(&mutex_);
    return importErrors_;
}

ImportResult BatchTransfer::importResult() const {
    QMutexLocker lock(&mutex_);
    return importResult_;
}

TransferProgress BatchTransfer::exportProgress() const {
    QMutexLocker lock(&mutex_);
    return exportProgress_;
}

QList<RowError> BatchTransfer::exportErrors() const {
    QMutexLocker lock(&mutex_);
    return exportErrors_;
}

ExportResult BatchTransfer::exportResult() const {
    QMutexLocker lock(&mutex_);
    return exportResult_;
}

TransferState BatchTransfer::importState() const {
    QMutexLocker lock(&mutex_);
    return importState_;
}

TransferState BatchTransfer::exportState() const {
    QMutexLocker lock(&mutex_);
    return exportState_;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

std::unique_ptr<IBatchTransfer> createBatchTransfer(DataBridge& bridge) {
    return std::make_unique<BatchTransfer>(bridge);
}

}  // namespace dbridge
