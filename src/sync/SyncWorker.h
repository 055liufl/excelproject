#pragma once
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncTypes.h"

#include <QMutex>
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
class SyncWorker : public QThread {
    Q_OBJECT
   public:
    SyncWorker(QSqlDatabase& wconn, sqlite3* h, SyncConfig config);
    ~SyncWorker() override;

    using WriteTask = std::function<void()>;

    // Enqueue a write task (thread-safe). The task runs on the worker thread.
    void enqueue(WriteTask task);

    // Ask the worker to stop after draining remaining tasks.
    void requestStop();

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
    QSqlDatabase& wconn_;
    sqlite3* h_;
    SyncConfig config_;

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
