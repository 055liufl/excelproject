#include "BatchTransfer.h"

#include "dbridge/Errors.h"

#include <QDebug>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtConcurrent/QtConcurrent>

#include "service/ExportService.h"
#include "service/ImportService.h"
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

    if (options.xlsxPath.isEmpty()) {
        if (err)
            *err = QStringLiteral("xlsxPath is required");
        return false;
    }

    detail::ProfileSpec profile;
    detail::SchemaCatalog catalog;
    if (!bridge_.snapshotProfileCatalog(options.profileName, &profile, &catalog, err))
        return false;
    const QString dbPath = bridge_.dbPath();

    // M-01 / H-06 fix: acquire the shared ForegroundGate. Capture shared_ptr to prevent
    // use-after-free when SyncEngine destroys the context while import runs in background.
    std::shared_ptr<sync::SyncContext> gateCtx;
    auto ctx = sync::SyncContextRegistry::instance().getExisting(dbPath);
    if (ctx) {
        if (!ctx->gate.tryAcquire(err))
            return false;
        gateCtx = ctx;  // keep alive until lambda completes
    }

    // Reset all import state.
    importStopRequested_.store(false);
    importState_ = TransferState::Running;
    importErrors_.clear();
    importResult_ = ImportResult{};
    importProgress_ = TransferProgress{};
    lock.unlock();

    importFuture_ = QtConcurrent::run([this, options, dbPath, profile, catalog, gateCtx]() {
        runImport(options, dbPath, profile, catalog);
        if (gateCtx)
            gateCtx->gate.release();
    });
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

    if (options.xlsxPath.isEmpty()) {
        if (err)
            *err = QStringLiteral("xlsxPath is required");
        return false;
    }

    detail::ProfileSpec profile;
    detail::SchemaCatalog catalog;
    if (!bridge_.snapshotProfileCatalog(options.profileName, &profile, &catalog, err))
        return false;
    const QString dbPath = bridge_.dbPath();

    // M-06 / H-06 fix: export acquires gate, capturing shared_ptr for lifetime safety.
    std::shared_ptr<sync::SyncContext> gateCtx;
    auto ctx = sync::SyncContextRegistry::instance().getExisting(dbPath);
    if (ctx) {
        if (!ctx->gate.tryAcquire(err))
            return false;
        gateCtx = ctx;
    }

    exportStopRequested_.store(false);
    exportState_ = TransferState::Running;
    exportErrors_.clear();
    exportResult_ = ExportResult{};
    exportProgress_ = TransferProgress{};
    lock.unlock();

    exportFuture_ = QtConcurrent::run([this, options, dbPath, profile, catalog, gateCtx]() {
        runExport(options, dbPath, profile, catalog);
        if (gateCtx)
            gateCtx->gate.release();
    });
    return true;
}

// ---------------------------------------------------------------------------
// runImport  (runs on worker thread)
//
void BatchTransfer::runImport(const ImportOptions& opts, const QString& dbPath,
                              const detail::ProfileSpec& profile,
                              const detail::SchemaCatalog& catalog) {
    // I-04 / J-10: Route import through SyncWorker if sync is active for this database.
    // J-10: Use getExisting() (no refCount increment, no context creation) so we never
    // create an empty context and never leak a reference if importFn is not set.
    {
        const QString dbPath = bridge_.dbPath();
        if (!dbPath.isEmpty()) {
            auto ctx = sync::SyncContextRegistry::instance().getExisting(dbPath);
            if (ctx && ctx->importFn) {
                ImportResult result = ctx->importFn(opts.xlsxPath, opts, profile, catalog);
                // No release() — getExisting() does not increment refCount.
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
            // ctx is nullptr or importFn not set — no release needed.
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

    // Use a connection opened on this QtConcurrent thread; never reuse DataBridge::db_ here.
    // We cannot inject progress ticks into it, so we report 50 % while it
    // runs and 100 % when it finishes (or 0 % on early-stop).
    {
        QMutexLocker lock(&mutex_);
        importProgress_.percent = 50;
    }

    ImportResult result;
    const QString connName =
        QStringLiteral("dbridge_bt_import_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
        if (!db.open()) {
            RowError e;
            e.code = QString::fromLatin1(err::E_OPEN_DB);
            e.message = db.lastError().text();
            result.errors.append(e);
        } else {
            QSqlQuery q(db);
            q.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
            detail::ImportService svc;
            result = svc.run(profile, catalog, opts.xlsxPath, opts, db);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

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
void BatchTransfer::runExport(const ExportOptions& opts, const QString& dbPath,
                              const detail::ProfileSpec& profile,
                              const detail::SchemaCatalog& catalog) {
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

    ExportResult result;
    const QString connName =
        QStringLiteral("dbridge_bt_export_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY=1;QSQLITE_BUSY_TIMEOUT=5000"));
        if (!db.open()) {
            RowError e;
            e.code = QString::fromLatin1(err::E_OPEN_DB);
            e.message = db.lastError().text();
            result.errors.append(e);
        } else {
            detail::ExportService svc;
            result = svc.run(profile, catalog, opts.xlsxPath, opts, db);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);

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
