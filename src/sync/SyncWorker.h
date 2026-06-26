#pragma once
#include "dbridge/DataBridge.h"
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncSelection.h"
#include "dbridge/sync/SyncTypes.h"

#include <QMutex>
#include <QSemaphore>
#include <QSet>
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
#include "profile/ProfileSpec.h"
#include "schema/QuarantineStore.h"
#include "schema/SchemaCatalog.h"
#include "schema/SchemaGuard.h"
#include "schema/TableStateStore.h"
#include "transport/AckChannel.h"
#include "transport/InboxLedger.h"
#include "transport/InboxWatcher.h"
#include "transport/OutboxWriter.h"
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <sqlite3.h>

namespace dbridge::sync {

// Forward declarations for components implemented in parallel agents
class RoutingTable;
class ConflictArbiter;
class RebaseEngine;
class DeadPeerEvictor;
class InboundTableGate;

// Single writer thread: processes a serialized task queue, scans inbox
// artifacts, and broadcasts packed outbox payloads to peers.
// The write connection is created inside run() so it is always owned by the worker thread.
class SyncWorker : public QThread {
    Q_OBJECT
   public:
    explicit SyncWorker(SyncConfig config, std::shared_ptr<InboundTableGate> inboundGate = nullptr);
    ~SyncWorker() override;

    using WriteTask = std::function<void()>;

    // Enqueue a write task (thread-safe). The task runs on the worker thread.
    void enqueue(WriteTask task);
    bool submitWriteSync(const std::function<bool(QSqlDatabase&, QString*)>& task,
                         QString* err = nullptr);
    void requestRescan();

    // Ask the worker to stop after draining remaining tasks.
    void requestStop();

    // Called by the thread that called start(): blocks until worker init completes
    // (or until timeoutMs elapses).  Returns true on success.
    bool waitForInit(int timeoutMs = 10000);

    // Non-empty when waitForInit() returns false or the worker emitted errorOccurred
    // during init.
    QString initError() const;

    // I-04: Submit import to run on the worker thread using wconn + session capture.
    // Blocks until the import completes (safe: caller thread waits, worker executes).
    // profile/catalog are value-copied snapshots taken on the calling thread — the worker
    // never touches DataBridge::db_ or its mutable catalog (C-03 fix).
    ImportResult submitImportSync(const ImportOptions& opts, const QString& xlsxPath,
                                  const detail::ProfileSpec& profile,
                                  const detail::SchemaCatalog& catalog);

    // I-19: Notify worker that a foreground sync() is waiting for ACK.
    // The worker will emit E_SYNC_ACK_TIMEOUT if no ACK arrives within ackMaxDelayMs.
    void startAckWait();
    // C-1 fix: cancel a pending ACK wait when no payload was sent.
    void cancelAckWait();

    // C-05 fix: route RowMutations through CapturedWriteTemplate on the worker thread.
    // Gives comparison-session saves the same session-capture + changelog semantics as local
    // imports, ensuring changes are broadcast to peers.
    bool submitCaptureWriteSync(const QList<RowMutation>& mutations, const QStringList& syncTables,
                                QString* err = nullptr);

    // C-02: Enqueue an immediate drain cycle (scan inbox + broadcast) on the worker.
    bool enqueueDrain(QString* err = nullptr);

    // C-01: Enqueue a selection push on the worker thread.
    // catalog is a pre-snapshot from the calling thread (safe cross-thread).
    // Returns immediately; progress/errors are reported via signals.
    void enqueueSelectionPush(const SyncSelection& selection, const detail::SchemaCatalog& catalog);

   signals:
    void progressUpdated(dbridge::sync::SyncProgress p);
    void errorOccurred(dbridge::sync::SyncError e);

   protected:
    void run() override;

   private:
    // --- Main loop phases ---
    void processPendingTasks();
    void scanInbox();
    bool broadcast(QString* outErr = nullptr);

    // --- Inbox processing ---
    bool processArtifact(const QString& path);
    bool processChangesetArtifact(const DecodeResult& dec, const QString& artifactName);
    bool processSelectionPushArtifact(const DecodeResult& dec, const QString& artifactName,
                                      const QString& checksum);
    bool processBaselineRequestArtifact(const DecodeResult& dec, const QString& artifactName);
    bool processBaselineResponseArtifact(const DecodeResult& dec, const QString& artifactName);
    bool processAckArtifact(const QString& path, const QString& artifactName);

    // --- Broadcast helpers ---
    // C-01 fix: ackedEntries collects (peer, origin, epoch, maxOriginSeq) for every artifact
    // successfully written during this call, so enqueueDrain can build a complete ACK window
    // that covers ALL origins broadcast (not just the local node's origin).
    bool broadcastTopeer(const QString& peer, QString* outErr = nullptr,
                         QList<PendingAckEntry>* ackedEntries = nullptr);
    qint64 computePeerAckedSeq(const QString& peer);
    qint64 nextLocalOriginSeq();
    bool isPeerEvicted(const QString& peer);
    qint64 peerLastAckMs(const QString& peer);
    qint64 peerLagBytes(const QString& peer, qint64 afterLocalSeq);
    void evaluatePeers();
    bool runBaselineFallbackFor(const QString& artifactName);
    // M-06 fix: replay quarantined payloads that became eligible after a schema upgrade
    // or baseline apply.  Must be called on the worker thread with wconnPtr_ valid.
    void drainQuarantine();

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
    std::unique_ptr<DeadPeerEvictor> evictor_;
    std::shared_ptr<InboundTableGate> inboundGate_;

    qint64 streamEpoch_ = 0;
    qint64 localOriginSeq_ = 0;  // next seq for local node
    // C-08: canonical sync table list (expanded from empty = all user tables).
    QStringList canonicalSyncTables_;

    // I-16: Rebase buffers keyed by "origin/originSeq"; populated after each apply_v2.
    // J-13: Use insertion-ordered queue for correct LRU eviction.
    QHash<QString, QByteArray> rebaseBuffers_;
    QList<QString> rebaseBufferOrder_;  // tracks insertion order for eviction

    // J-02: ACK timeout tracking — atomic so startAckWait() (called from calling thread)
    // and the worker-thread main loop both access it safely without a mutex.
    std::atomic<bool> ackWaiting_{false};
    std::atomic<qint64> ackDeadlineMs_{0};

    // C-04 fix: tracks the pushId of an in-flight selection push so processAckArtifact()
    // can ignore chunk ACKs from stale or unrelated pushes. Accessed only on the worker thread
    // (set inside the enqueueSelectionPush lambda, read in processAckArtifact — both run on the
    // same worker thread, no additional locking needed).
    QString pendingPushId_;

    // C-01 fix: all (peer, origin, epoch, targetSeq) entries that must be ACKed before
    // the foreground sync() is considered complete. Empty when no ACK window is active.
    // Accessed only on the worker thread (set in enqueueDrain, read in processAckArtifact).
    QList<PendingAckEntry> pendingAckWindow_;

    QSet<QString> baselineRequestsInFlight_;
};

}  // namespace dbridge::sync
