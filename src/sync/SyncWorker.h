#pragma once
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncTypes.h"

#include <QMutex>
#include <QSemaphore>
#include <QThread>
#include <QWaitCondition>

#include "anchor/OutboundAckStore.h"
#include "apply/AppliedVectorStore.h"
#include "apply/CapturedWriteTemplate.h"
#include "apply/ChangesetApplier.h"
#include "apply/RowWinnerStore.h"
#include "capture/ChangelogStore.h"
#include "capture/SessionRecorder.h"
#include "payload/PayloadCodec.h"
#include "schema/QuarantineStore.h"
#include "schema/SchemaGuard.h"
#include "schema/TableStateStore.h"
#include "transport/AckChannel.h"
#include "transport/InboxLedger.h"
#include "transport/InboxWatcher.h"
#include "transport/OutboxWriter.h"
#include <functional>
#include <memory>
#include <sqlite3.h>

namespace dbridge::sync {

// Forward declarations for components implemented in parallel agents
class RoutingTable;
class ConflictArbiter;
class RebaseEngine;

// Single writer thread: processes a serialized task queue, scans inbox
// artifacts, and broadcasts packed outbox payloads to peers.
// The write connection is created inside run() so it is always owned by the worker thread.
class SyncWorker : public QThread {
    Q_OBJECT
   public:
    explicit SyncWorker(SyncConfig config);
    ~SyncWorker() override;

    using WriteTask = std::function<void()>;

    // Enqueue a write task (thread-safe). The task runs on the worker thread.
    void enqueue(WriteTask task);

    // Ask the worker to stop after draining remaining tasks.
    void requestStop();

    // Called by the thread that called start(): blocks until worker init completes
    // (or until timeoutMs elapses).  Returns true on success.
    bool waitForInit(int timeoutMs = 10000);

    // Non-empty when waitForInit() returns false or the worker emitted errorOccurred
    // during init.
    QString initError() const;

   signals:
    void progressUpdated(dbridge::sync::SyncProgress p);
    void errorOccurred(dbridge::sync::SyncError e);

   protected:
    void run() override;

   private:
    // --- Main loop phases ---
    void processPendingTasks();
    void scanInbox();
    void broadcast();

    // --- Inbox processing ---
    bool processArtifact(const QString& path);
    bool processChangesetArtifact(const DecodeResult& dec, const QString& artifactName);
    bool processSelectionPushArtifact(const DecodeResult& dec, const QString& artifactName);
    bool processAckArtifact(const QString& path, const QString& artifactName);

    // --- Broadcast helpers ---
    void broadcastTopeer(const QString& peer);
    qint64 computePeerAckedSeq(const QString& peer);

    // --- Data members ---
    SyncConfig config_;

    // Pointers into run()-owned local variables; valid only while run() is executing.
    // Task closures enqueued after init-success may use these safely since the queue
    // is drained before run() returns.
    QSqlDatabase* wconnPtr_ = nullptr;
    sqlite3* hPtr_ = nullptr;

    // Initialisation synchronisation (I-02 fix)
    QSemaphore initSemaphore_{0};
    QString initError_;

    QMutex queueMutex_;
    QWaitCondition queueCond_;
    QList<WriteTask> taskQueue_;
    bool stopRequested_ = false;

    // Tracking stores
    std::unique_ptr<AppliedVectorStore> av_;
    std::unique_ptr<RowWinnerStore> rw_;
    std::unique_ptr<TableStateStore> ts_;
    std::unique_ptr<ChangelogStore> clog_;
    std::unique_ptr<SessionRecorder> rec_;
    std::unique_ptr<SchemaGuard> guard_;
    std::unique_ptr<ChangesetApplier> applier_;
    std::unique_ptr<CapturedWriteTemplate> tpl_;
    std::unique_ptr<OutboxWriter> outbox_;
    std::unique_ptr<InboxLedger> ledger_;
    std::unique_ptr<InboxWatcher> watcher_;
    std::unique_ptr<AckChannel> ackChan_;
    std::unique_ptr<OutboundAckStore> ackStore_;
    std::unique_ptr<PayloadCodec> codec_;
    std::unique_ptr<RoutingTable> routing_;
    std::unique_ptr<ConflictArbiter> arbiter_;
    std::unique_ptr<RebaseEngine> rebaser_;
    std::unique_ptr<QuarantineStore> quarantine_;

    // Inbox scan state: list of pending artifact paths
    QStringList pendingArtifacts_;
    qint64 streamEpoch_ = 0;
    qint64 localOriginSeq_ = 0;  // next seq for local node
};

}  // namespace dbridge::sync
