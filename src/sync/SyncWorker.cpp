#include "sync/SyncWorker.h"

#include "dbridge/Errors.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include "service/ImportService.h"
#include "sql/SqlBuilder.h"
#include "sync/SyncContext.h"
#include "sync/SyncDDL.h"
#include "sync/WriteTxn.h"
#include "sync/baseline/BaselineManager.h"
#include "sync/capture/SqliteHandle.h"
#include "sync/conflict/ConflictArbiter.h"
#include "sync/conflict/RebaseEngine.h"
#include "sync/conflict/RoutingTable.h"
#include "sync/diff/InboundTableGate.h"
#include "sync/peer/DeadPeerEvictor.h"
#include "sync/schema/SchemaEligibility.h"
#include "sync/selection/ChunkStreamer.h"
#include "sync/selection/ConsistencyCache.h"
#include "sync/selection/FkClosureBuilder.h"
#include "sync/selection/SelectionResolver.h"
#include <future>

namespace dbridge::sync {

namespace {
QSet<QString> payloadTables(const DecodeResult& dec) {
    QSet<QString> tables;
    if (dec.kind == PayloadKind::SelectionPush) {
        for (const FrozenEntry& entry : dec.selection.frozenEntries) {
            if (!entry.table.isEmpty())
                tables.insert(entry.table);
        }
        return tables;
    }

    sqlite3_changeset_iter* it = nullptr;
    if (sqlite3changeset_start(
            &it, dec.changeset.size(),
            const_cast<void*>(static_cast<const void*>(dec.changeset.constData()))) != SQLITE_OK)
        return tables;

    while (sqlite3changeset_next(it) == SQLITE_ROW) {
        const char* tableName = nullptr;
        int columns = 0;
        int op = 0;
        int indirect = 0;
        if (sqlite3changeset_op(it, &tableName, &columns, &op, &indirect) == SQLITE_OK &&
            tableName) {
            tables.insert(QString::fromUtf8(tableName));
        }
    }
    sqlite3changeset_finalize(it);
    return tables;
}
}  // namespace

SyncWorker::SyncWorker(SyncConfig config, std::shared_ptr<InboundTableGate> inboundGate)
    : config_(std::move(config)), inboundGate_(std::move(inboundGate)) {
    av_ = std::make_unique<AppliedVectorStore>();
    rw_ = std::make_unique<RowWinnerStore>();
    ts_ = std::make_unique<TableStateStore>();
    clog_ = std::make_unique<ChangelogStore>();
    rec_ = std::make_unique<SessionRecorder>();
    guard_ = std::make_unique<SchemaGuard>();
    applier_ = std::make_unique<ChangesetApplier>();
    outbox_ = std::make_unique<OutboxWriter>(config_.outboxDir());
    ledger_ = std::make_unique<InboxLedger>();
    ackChan_ = std::make_unique<AckChannel>(*outbox_, config_.nodeId(), config_.ackMaxDelayMs());
    ackStore_ = std::make_unique<OutboundAckStore>();
    codec_ = std::make_unique<PayloadCodec>();
    routing_ = std::make_unique<RoutingTable>();
    arbiter_ = std::make_unique<ConflictArbiter>();
    rebaser_ = std::make_unique<RebaseEngine>();
    quarantine_ = std::make_unique<QuarantineStore>();
    evictor_ = std::make_unique<DeadPeerEvictor>();
    evictor_->configure(config_.peerLagSoftSeq(), config_.peerLagHardSeq(),
                        config_.peerLagSoftBytes(), config_.peerLagHardBytes(),
                        config_.peerLagSoftMs(), config_.peerLagHardMs());
    if (!inboundGate_)
        inboundGate_ = std::make_shared<InboundTableGate>();
    // InboxWatcher is created in run() so it lives on the worker thread
}

SyncWorker::~SyncWorker() {
    requestStop();
    wait(5000);
}

void SyncWorker::enqueue(WriteTask task) {
    QMutexLocker lk(&queueMutex_);
    taskQueue_.append(std::move(task));
    queueCond_.wakeOne();
}

bool SyncWorker::submitWriteSync(const std::function<bool(QSqlDatabase&, QString*)>& task,
                                 QString* err) {
    if (!isRunning() || !wconnPtr_) {
        if (err)
            *err = QStringLiteral("SyncWorker not ready");
        return false;
    }
    if (QThread::currentThread() == this) {
        return task(*wconnPtr_, err);
    }

    auto sharedPromise = std::make_shared<std::promise<QPair<bool, QString>>>();
    std::future<QPair<bool, QString>> future = sharedPromise->get_future();
    enqueue([this, task, sp = sharedPromise]() {
        QString taskErr;
        bool ok = false;
        if (wconnPtr_) {
            ok = task(*wconnPtr_, &taskErr);
        } else {
            taskErr = QStringLiteral("wconn not available in worker");
        }
        sp->set_value(qMakePair(ok, taskErr));
    });
    if (future.wait_for(std::chrono::seconds(60)) == std::future_status::timeout) {
        if (err)
            *err = QStringLiteral("submitWriteSync timed out after 60s");
        return false;
    }
    const auto result = future.get();
    if (!result.first && err)
        *err = result.second;
    return result.first;
}

void SyncWorker::requestRescan() {
    if (!isRunning())
        return;
    enqueue([this]() { scanInbox(); });
}

void SyncWorker::requestStop() {
    QMutexLocker lk(&queueMutex_);
    stopRequested_ = true;
    queueCond_.wakeAll();
}

bool SyncWorker::waitForInit(int timeoutMs) {
    return initSemaphore_.tryAcquire(1, timeoutMs);
}

QString SyncWorker::initError() const {
    return initError_;
}

void SyncWorker::run() {
    // --- Create write connection on the worker thread (I-02 fix) ---
    QString connName =
        QStringLiteral("dbridge_sw_") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlDatabase wconn = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName);
    wconn.setDatabaseName(config_.sqlitePath());
    wconn.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!wconn.open()) {
        initError_ = QStringLiteral("cannot open db: ") + wconn.lastError().text();
        initSemaphore_.release();
        QSqlDatabase::removeDatabase(connName);
        return;
    }

    // WAL + foreign_keys
    {
        QSqlQuery q(wconn);
        q.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        q.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    }

    sqlite3* h = SqliteHandle::of(wconn);
    if (!h) {
        initError_ = QStringLiteral("cannot get sqlite3* handle");
        initSemaphore_.release();
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    if (!SqliteHandle::sessionAvailable(h)) {
        initError_ = QStringLiteral("E_SYNC_SESSION_UNAVAILABLE");
        initSemaphore_.release();
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }

    // Run DDL
    for (const QString& stmt : ddl::allCreateStatements()) {
        QSqlQuery q(wconn);
        if (!q.exec(stmt)) {
            initError_ = QStringLiteral("DDL: ") + q.lastError().text();
            initSemaphore_.release();
            wconn.close();
            QSqlDatabase::removeDatabase(connName);
            return;
        }
    }

    // C-08 fix: expand empty syncTables to all user tables so session always attaches something.
    QString expandErr;
    canonicalSyncTables_ =
        SchemaEligibility::expandSyncTables(wconn, config_.syncTables(), &expandErr);
    if (canonicalSyncTables_.isEmpty() && !config_.syncTables().isEmpty()) {
        // explicit tables given but expansion failed — use original list
        canonicalSyncTables_ = config_.syncTables();
    }

    // Schema eligibility check
    QStringList rejected;
    QString eligErr;
    if (!SchemaEligibility::verify(wconn, canonicalSyncTables_, &rejected, &eligErr)) {
        initError_ = QStringLiteral("E_SYNC_UNSUPPORTED_SCHEMA: ") + eligErr;
        if (!rejected.isEmpty())
            initError_ += QStringLiteral("; rejected: ") + rejected.join(QLatin1Char(','));
        initSemaphore_.release();
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }

    // Expose pointers for task closures (valid only within this run() lifetime)
    wconnPtr_ = &wconn;
    hPtr_ = h;

    // --- One-time store initialization on the worker thread ---
    streamEpoch_ = QDateTime::currentMSecsSinceEpoch();

    QString initErr;
    if (!av_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    if (!rw_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    if (!ts_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    if (!clog_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    {
        QSqlQuery q(wconn);
        q.prepare(
            QStringLiteral("SELECT COALESCE(MAX(origin_seq), 0) "
                           "FROM __sync_changelog WHERE origin = ?"));
        q.addBindValue(config_.nodeId());
        if (q.exec() && q.next())
            localOriginSeq_ = q.value(0).toLongLong();
    }
    if (!ledger_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    if (!ackStore_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }
    if (!quarantine_->init(wconn, &initErr)) {
        initError_ = initErr;
        initSemaphore_.release();
        wconnPtr_ = nullptr;
        hPtr_ = nullptr;
        wconn.close();
        QSqlDatabase::removeDatabase(connName);
        return;
    }

    QString schemaFp = SchemaGuard::computeFingerprint(wconn, canonicalSyncTables_);
    guard_->setLocal(config_.schemaVersion(), schemaFp);

    routing_->configure(config_.nodeId(), config_.peerNodes());
    arbiter_->setRankMap(config_.allRanks());

    // Initialize CapturedWriteTemplate now that all stores are ready
    tpl_ = std::make_unique<CapturedWriteTemplate>(wconn, h, *av_, *rw_, *ts_, *clog_, *rec_,
                                                   *guard_, *applier_, config_.nodeId(),
                                                   streamEpoch_, schemaFp, config_.schemaVersion());

    // Create InboxWatcher on this thread.
    // I-10 fix: InboxWatcher no longer uses QFileSystemWatcher/QTimer (which require an event
    // loop).  It now exposes a synchronous scan() method called explicitly in scanInbox().
    watcher_ = std::make_unique<InboxWatcher>(config_.inboxDir(), wconn, *ledger_);

    // L-01 fix: publish the canonical sync table list to the shared SyncContext so other
    // modules (ComparisonSession, BatchTransfer, diagnostics) can read the same expanded set.
    // H-13 fix: also publish the active stream epoch so the ComparisonSession factory reads
    // __sync_table_state at the correct epoch (instead of a 0 placeholder).
    {
        auto ctx = SyncContextRegistry::instance().getExisting(config_.sqlitePath());
        if (ctx) {
            if (!canonicalSyncTables_.isEmpty())
                ctx->canonicalSyncTables = canonicalSyncTables_;
            ctx->streamEpoch = streamEpoch_;
        }
    }

    // H-16 fix: on startup, replay any quarantined payloads whose schema version is now
    // applicable — e.g. payloads quarantined before a restart that brought the local schema up
    // to date, or after a baseline. drainReady() removes the rows; a replay that still fails
    // re-quarantines. We drain once at init (not every scan) because the worker's schema version
    // is fixed for its lifetime, so re-draining at runtime would only churn incompatible rows.
    {
        const QList<QByteArray> readyPayloads =
            quarantine_->drainReady(wconn, config_.schemaVersion());
        for (const QByteArray& payload : readyPayloads) {
            DecodeResult qdec;
            QString qerr;
            if (codec_->decode(payload, &qdec, &qerr) && qdec.kind == PayloadKind::Changeset)
                processChangesetArtifact(qdec, QString());
        }
    }

    // Signal successful initialization to initialize() caller
    initSemaphore_.release();

    qint64 lastBroadcastMs = 0;

    // --- Main event loop ---
    while (true) {
        // Wait for work or timeout for periodic tasks
        {
            QMutexLocker lk(&queueMutex_);
            if (taskQueue_.isEmpty() && !stopRequested_) {
                // L-02 fix: shorten wait when an ACK deadline is active so E_SYNC_ACK_TIMEOUT
                // fires at the configured time, not delayed by broadcastIntervalMs.
                qint64 waitMs = config_.broadcastIntervalMs();
                if (ackWaiting_.load()) {
                    const qint64 remaining =
                        ackDeadlineMs_.load() - QDateTime::currentMSecsSinceEpoch();
                    if (remaining > 0 && remaining < waitMs)
                        waitMs = remaining;
                    else if (remaining <= 0)
                        waitMs = 1;  // already expired; wake immediately
                }
                queueCond_.wait(&queueMutex_, static_cast<ulong>(waitMs));
            }
            if (stopRequested_ && taskQueue_.isEmpty())
                break;
        }

        processPendingTasks();
        scanInbox();

        qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (nowMs - lastBroadcastMs >= config_.broadcastIntervalMs()) {
            broadcast();
            lastBroadcastMs = nowMs;
        }

        // I-19: ACK timeout — emit E_SYNC_ACK_TIMEOUT if foreground sync() is waiting
        // and no ACK has arrived within ackMaxDelayMs.
        if (ackWaiting_ && nowMs >= ackDeadlineMs_) {
            ackWaiting_ = false;
            emit errorOccurred({QLatin1String(err::E_SYNC_ACK_TIMEOUT), Severity::Error,
                                QStringLiteral("sync"), config_.nodeId(),
                                QStringLiteral("ACK not received within ackMaxDelayMs=") +
                                    QString::number(config_.ackMaxDelayMs())});
        }
    }

    // I-10: watcher_->stop() removed — no QTimer/QFileSystemWatcher to tear down.
    QString ackErr;
    if (!ackChan_->flush(*codec_, &ackErr)) {
        emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Warning, QStringLiteral("ack"),
                            config_.nodeId(), ackErr});
    }

    // Teardown: clear pointers before closing connection
    tpl_.reset();
    watcher_.reset();
    wconnPtr_ = nullptr;
    hPtr_ = nullptr;
    wconn.close();
    QSqlDatabase::removeDatabase(connName);
}

void SyncWorker::processPendingTasks() {
    QList<WriteTask> tasks;
    {
        QMutexLocker lk(&queueMutex_);
        tasks.swap(taskQueue_);
    }
    for (auto& task : tasks) {
        task();
    }
}

void SyncWorker::scanInbox() {
    if (!wconnPtr_)
        return;

    // I-10 fix: use synchronous scan() instead of signal-driven InboxWatcher.
    // Direct scan (no event loop needed).
    QStringList found = watcher_->scan(*wconnPtr_);

    // Also add ledger pending-seen artifacts not yet seen by the directory scan.
    QStringList pending = ledger_->pendingSeen(*wconnPtr_);
    for (const QString& name : pending) {
        QString full = config_.inboxDir() + QDir::separator() + name;
        if (!found.contains(full))
            found.append(full);
    }

    for (const QString& path : found)
        processArtifact(path);

    // M-1 fix: gap timeout is now configurable via SyncConfig (default 30 s).
    QStringList stale = ledger_->stalePending(*wconnPtr_, config_.gapTimeoutMs());
    if (!stale.isEmpty()) {
        emit errorOccurred(
            {err::E_SYNC_GAP, Severity::Error, QStringLiteral("scanInbox"), QString(),
             QStringLiteral("Changeset gap unresolved after %1ms; %2 artifact(s) pending. "
                            "Baseline fallback required.")
                 .arg(config_.gapTimeoutMs())
                 .arg(stale.size())});
        for (const QString& artifactName : stale)
            runBaselineFallbackFor(artifactName);
    }
}

bool SyncWorker::processArtifact(const QString& path) {
    QFileInfo fi(path);
    QString name = fi.fileName();

    // M-07 fix: ACK artifacts also enter the inbox ledger to avoid infinite re-processing.
    if (name.endsWith(QStringLiteral(".ack"))) {
        LedgerStatus ackSt = ledger_->status(*wconnPtr_, name);
        if (ackSt == LedgerStatus::Consumed)
            return true;  // already processed
        ledger_->markSeen(*wconnPtr_, name, nullptr);
        const bool ok = processAckArtifact(path, name);
        if (ok)
            ledger_->markConsumed(*wconnPtr_, name, nullptr);
        return ok;
    }

    // Check ledger
    LedgerStatus st = ledger_->status(*wconnPtr_, name);
    if (st == LedgerStatus::Consumed || st == LedgerStatus::Corrupt)
        return true;

    // Read artifact
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "[SyncWorker] Cannot open artifact:" << path;
        return false;
    }
    QByteArray data = f.readAll();
    f.close();

    // Decode
    DecodeResult dec;
    QString decErr;
    if (!codec_->decode(data, &dec, &decErr)) {
        QString markErr;
        ledger_->markCorrupt(*wconnPtr_, name, &markErr);
        // J-08: Move corrupt artifact to quarantineDir so it's not re-scanned.
        if (!config_.quarantineDir().isEmpty()) {
            QDir qDir(config_.quarantineDir());
            qDir.mkpath(QStringLiteral("."));
            QFile::copy(path, qDir.filePath(name));
            QFile::remove(path);
        }
        emit errorOccurred(
            {err::E_SYNC_PAYLOAD_CORRUPT, Severity::Error, "inbox", dec.header.origin, decErr});
        return false;
    }

    ledger_->markSeen(*wconnPtr_, name, nullptr);
    if (inboundGate_ && inboundGate_->shouldDefer(payloadTables(dec))) {
        return false;
    }

    bool ok = false;
    if (dec.kind == PayloadKind::Changeset)
        ok = processChangesetArtifact(dec, name);
    else if (dec.kind == PayloadKind::SelectionPush) {
        const QString checksum =
            QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
        ok = processSelectionPushArtifact(dec, name, checksum);
    } else if (dec.kind == PayloadKind::BaselineRequest) {
        ok = processBaselineRequestArtifact(dec, name);
    } else if (dec.kind == PayloadKind::BaselineResponse) {
        ok = processBaselineResponseArtifact(dec, name);
    }

    if (ok) {
        ledger_->markConsumed(*wconnPtr_, name, nullptr);
        if (dec.kind == PayloadKind::Changeset) {
            // Send ACK back to the changeset's origin node (J-01 fix: populate toPeer).
            ChangesetAck ack;
            ack.origin = dec.header.origin;
            ack.streamEpoch = dec.header.streamEpoch;
            ack.appliedSeq = dec.header.originSeq;
            ack.toPeer = dec.header.origin;  // ACK addressed to the changeset producer
            QString ackErr;
            if (!ackChan_->scheduleChangesetAck(ack, *codec_, &ackErr)) {
                emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Warning, QStringLiteral("ack"),
                                    dec.header.origin, ackErr});
            }
        }
    }
    return ok;
}

bool SyncWorker::processChangesetArtifact(const DecodeResult& dec, const QString& /*name*/) {
    const PayloadHeader& hdr = dec.header;

    // Schema guard
    QString schemaErr;
    if (!guard_->verifyPayload(hdr.schemaVer, hdr.schemaFingerprint, &schemaErr)) {
        // Quarantine
        quarantine_->quarantine(*wconnPtr_, hdr.origin, hdr.originSeq, hdr.streamEpoch,
                                hdr.schemaVer, dec.changeset, nullptr);
        emit errorOccurred(
            {err::E_SYNC_SCHEMA_MISMATCH, Severity::Warning, "apply", hdr.origin, schemaErr});
        return false;
    }

    // Apply via CapturedWriteTemplate Branch A
    WriteParams p;
    p.kind = WriteKind::InboundChangeset;
    p.origin = hdr.origin;
    p.epoch = hdr.streamEpoch;
    p.seq = hdr.originSeq;
    p.schemaVer = hdr.schemaVer;
    p.schemaFp = hdr.schemaFingerprint;
    p.originRank = config_.originRank(hdr.origin);
    p.changesetBlob = dec.changeset;

    WriteResult res = tpl_->execute(p);
    if (!res.ok) {
        // H-03 fix: GAP_PENDING is not a hard error — keep artifact in ledger as 'seen' so the
        // three-time-rescan logic can retry. The E_SYNC_GAP warning fires via stalePending() later.
        if (res.errorCode == QLatin1String("GAP_PENDING"))
            return false;  // caller must NOT markConsumed; ledger stays 'seen'
        emit errorOccurred(
            {res.errorCode, Severity::Error, QStringLiteral("apply"), hdr.origin, res.errorMsg});
        return false;
    }

    // H-03(table_state): surface stale warning so callers know DiffEngine data may be stale.
    if (!res.tableStateStaleSince.isEmpty()) {
        emit errorOccurred(
            {err::W_SYNC_UNTRACKED_CHANGE, Severity::Warning, QStringLiteral("table_state"),
             hdr.origin, QStringLiteral("table_state update failed: ") + res.tableStateStaleSince});
    }

    // I-16/J-13: Store rebase buffer; use insertion-ordered list for correct LRU eviction.
    if (!res.applyOutcome.rebaseBuffer.isEmpty()) {
        QString key = hdr.origin + QLatin1Char('/') + QString::number(hdr.originSeq);
        if (!rebaseBuffers_.contains(key)) {
            rebaseBufferOrder_.append(key);
        }
        rebaseBuffers_.insert(key, res.applyOutcome.rebaseBuffer);
        // Evict oldest entry when over capacity
        constexpr int kMaxRebaseBuffers = 500;
        while (rebaseBuffers_.size() > kMaxRebaseBuffers && !rebaseBufferOrder_.isEmpty()) {
            QString oldest = rebaseBufferOrder_.takeFirst();
            rebaseBuffers_.remove(oldest);
        }
    }

    // J-02/I-19: Inbound apply is NOT a typed ACK — don't clear ackWaiting_ here.
    // ACK-wait is only cleared by processAckArtifact (typed ACK) or timeout.

    // Update local origin seq if this is our own changeset echoed back
    if (hdr.origin == config_.nodeId() && hdr.originSeq > localOriginSeq_)
        localOriginSeq_ = hdr.originSeq;

    return true;
}

bool SyncWorker::processSelectionPushArtifact(const DecodeResult& dec, const QString& /*name*/,
                                              const QString& checksum) {
    const PayloadHeader& hdr = dec.header;
    const SelectionPushBody& body = dec.selection;
    const QString pushId = !hdr.pushId.isEmpty() ? hdr.pushId : body.pushId;
    const int chunkSeq = hdr.chunkSeq != 0 ? hdr.chunkSeq : body.chunkSeq;
    const int totalChunks = hdr.totalChunks > 0 ? hdr.totalChunks : body.totalChunks;

    // M-05 fix: ensure __sync_push_progress row exists before any chunk processing.
    // Insert on first chunk arrival (ON CONFLICT DO NOTHING is idempotent for subsequent chunks).
    if (!pushId.isEmpty()) {
        QSqlQuery ins(*wconnPtr_);
        // H-05 fix: ON CONFLICT DO UPDATE so re-sends refresh total_chunks/status/updated_ms.
        ins.prepare(
            QStringLiteral("INSERT INTO __sync_push_progress "
                           "(push_id, origin, peer, total_chunks, schema_ver, status, updated_ms) "
                           "VALUES (?, ?, ?, ?, ?, 'streaming', ?) "
                           "ON CONFLICT(push_id) DO UPDATE SET "
                           "  status='streaming', total_chunks=excluded.total_chunks, "
                           "  schema_ver=excluded.schema_ver, updated_ms=excluded.updated_ms"));
        ins.addBindValue(pushId);
        ins.addBindValue(hdr.origin);
        ins.addBindValue(config_.nodeId());
        ins.addBindValue(totalChunks > 0 ? totalChunks : 1);
        ins.addBindValue(hdr.schemaVer);
        ins.addBindValue(QDateTime::currentMSecsSinceEpoch());
        ins.exec();  // non-fatal if table doesn't exist yet (pre-DDL edge case)
    }

    // J-04: Reject the entire push if the sender's schema version has moved.
    if (hdr.schemaVer != config_.schemaVersion()) {
        QSqlQuery q(*wconnPtr_);
        q.prepare(
            QStringLiteral("UPDATE __sync_push_progress SET status='failed', "
                           "failed_code='E_SYNC_PUSH_SCHEMA_MOVED', updated_ms=? "
                           "WHERE push_id=?"));
        q.addBindValue(QDateTime::currentMSecsSinceEpoch());
        q.addBindValue(pushId);
        q.exec();
        emit errorOccurred({err::E_SYNC_PUSH_SCHEMA_MOVED, Severity::Error,
                            QStringLiteral("selection_push"), hdr.origin,
                            QString(QStringLiteral("push_id=%1 schema_ver=%2 local=%3"))
                                .arg(pushId)
                                .arg(hdr.schemaVer)
                                .arg(config_.schemaVersion())});
        return false;
    }

    // H-15 fix: enforce in-order chunk application. ChunkStreamer emits chunks in topological
    // order (parent chunks before child chunks); applying chunk N before 0..N-1 would risk
    // FK-dangling rows and break the "half not externalised" guarantee. Apply chunk N only when
    // all chunks 0..N-1 are already applied; otherwise keep the artifact pending (ledger stays
    // 'seen') and retry on the next inbox scan once the predecessor chunks arrive.
    if (chunkSeq > 0 && !pushId.isEmpty()) {
        QSqlQuery cq(*wconnPtr_);
        cq.prepare(
            QStringLiteral("SELECT COUNT(*) FROM __sync_push_chunk_progress "
                           "WHERE push_id=? AND status='applied' AND chunk_seq < ?"));
        cq.addBindValue(pushId);
        cq.addBindValue(chunkSeq);
        if (cq.exec() && cq.next()) {
            const int appliedBefore = cq.value(0).toInt();
            if (appliedBefore < chunkSeq)
                return false;  // predecessor chunk(s) not yet applied — keep pending, retry later
        }
    }

    // Build mutations from selection push body.
    // J-05: Fill pkColumns from PRAGMA table_info so UpsertExecutor can build correct
    // ON CONFLICT(...) clauses. Use cached PRAGMA result per table.
    QHash<QString, QStringList> pkColsCache;
    auto getPkCols = [&](const QString& table) -> QStringList {
        if (pkColsCache.contains(table))
            return pkColsCache[table];
        QSqlQuery pq(*wconnPtr_);
        // H-3 fix: use quoteIdent for table name in PRAGMA.
        pq.prepare(QStringLiteral("PRAGMA table_info(") + detail::SqlBuilder::quoteIdent(table) +
                   QLatin1Char(')'));
        QStringList pks;
        if (pq.exec()) {
            while (pq.next()) {
                if (pq.value("pk").toInt() > 0)
                    pks << pq.value("name").toString();
            }
        }
        pkColsCache[table] = pks;
        return pks;
    };

    QList<RowMutation> mutations;
    for (int i = 0; i < body.rows.size(); ++i) {
        const QVariantMap& rowMap = body.rows[i];
        RowMutation m;
        m.table = (i < body.frozenEntries.size()) ? body.frozenEntries[i].table : QString();
        m.columns = rowMap.keys();
        m.values = rowMap.values();
        m.pkColumns = getPkCols(m.table);
        // recordKind: "selected" → DoUpdate (authoritative), "dependency" → DoNothing
        if (i < body.frozenEntries.size() &&
            body.frozenEntries[i].recordKind == QLatin1String("dependency"))
            m.mode = UpsertMode::DoNothing;
        else
            m.mode = UpsertMode::DoUpdate;
        mutations.append(m);
    }

    WriteParams p;
    p.kind = WriteKind::InboundSelectionPush;
    // C-03 fix: attribute the changelog entry to the local node (not the remote sender) and
    // allocate a fresh local origin seq. Using the sender's originSeq=0 (or any fixed seq)
    // causes UNIQUE(origin, stream_epoch, origin_seq) collisions when multiple chunks arrive
    // from the same sender in the same epoch. The changelog entry records "this node applied a
    // selection push" — it is a local event, not a forwarding of the remote changeset.
    p.origin = config_.nodeId();
    p.epoch = streamEpoch_;
    p.seq = nextLocalOriginSeq();
    p.schemaVer = hdr.schemaVer;
    p.schemaFp = hdr.schemaFingerprint;
    p.originRank = config_.originRank(hdr.origin);
    p.pushId = pushId;
    p.chunkSeq = chunkSeq;
    p.checksum = checksum;
    p.mutations = mutations;
    p.syncTables = canonicalSyncTables_;

    WriteResult res = tpl_->execute(p);
    if (!res.ok) {
        emit errorOccurred(
            {res.errorCode, Severity::Error, "selection_push", hdr.origin, res.errorMsg});
        return false;
    }
    if (!res.tableStateStaleSince.isEmpty()) {
        emit errorOccurred(
            {err::W_SYNC_UNTRACKED_CHANGE, Severity::Warning, QStringLiteral("table_state"),
             hdr.origin, QStringLiteral("table_state update failed: ") + res.tableStateStaleSince});
    }
    PushChunkAck ack;
    ack.pushId = pushId;
    ack.chunkSeq = chunkSeq;
    ack.totalChunks = totalChunks > 0 ? totalChunks : 1;
    ack.checksum = checksum;
    ack.ok = true;
    ack.toPeer = hdr.origin;  // H-04 fix: ACK routes back to the push origin
    QString ackErr;
    if (!ackChan_->schedulePushChunkAck(ack, *codec_, &ackErr)) {
        emit errorOccurred(
            {err::E_SYNC_TRANSPORT, Severity::Warning, QStringLiteral("ack"), hdr.origin, ackErr});
    }
    return true;
}

// C-01 fix: implement baseline request handler.
// When a peer asks for a baseline (because it has a gap it cannot self-heal), this node
// serializes the requested tables and writes a BaselineResponse artifact to the outbox.
bool SyncWorker::processBaselineRequestArtifact(const DecodeResult& dec,
                                                const QString& /*artifactName*/) {
    if (!wconnPtr_)
        return false;

    const BaselineRequestPayload& req = dec.baselineRequest;

    // Decide which tables to export: prefer the request's list, fall back to all sync tables.
    QStringList tables = req.requestedTables.isEmpty() ? canonicalSyncTables_ : req.requestedTables;

    BaselineManager bm;
    BaselineManager::BaselineArtifact art;
    QString exportErr;
    if (!bm.exportBaseline(*wconnPtr_, tables, &art, &exportErr)) {
        emit errorOccurred({err::E_SYNC_BASELINE_FAILED, Severity::Error,
                            QStringLiteral("baseline_request"), dec.header.origin, exportErr});
        return false;
    }

    BaselineResponsePayload resp;
    resp.origin = config_.nodeId();
    resp.requestOrigin = dec.header.origin;  // send back to requester
    resp.streamEpoch = streamEpoch_;
    resp.tables = tables;
    resp.fromSeq = req.fromSeq;
    resp.pendingArtifactName = req.pendingArtifactName;
    resp.baselineData = art.data;
    resp.sourceMaxSeq = art.sourceMaxSeq;

    PayloadHeader hdr;
    hdr.origin = config_.nodeId();
    hdr.originSeq = nextLocalOriginSeq();
    hdr.streamEpoch = streamEpoch_;
    hdr.schemaVer = config_.schemaVersion();
    hdr.schemaFingerprint = guard_ ? guard_->fingerprint() : QString();
    hdr.routeTag = dec.header.origin;

    QByteArray payload = codec_->encodeBaselineResponse(hdr, resp);

    const QString artifactOut = QStringLiteral("blresp__") + config_.nodeId() +
                                QStringLiteral("__") + dec.header.origin + QStringLiteral("__") +
                                QString::number(QDateTime::currentMSecsSinceEpoch()) +
                                QStringLiteral(".payload");
    QString writeErr;
    if (!outbox_->write(artifactOut, payload, &writeErr)) {
        emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Error,
                            QStringLiteral("baseline_request"), dec.header.origin, writeErr});
        return false;
    }
    return true;
}

// C-01 fix: implement baseline response handler.
// Applies the received baseline data, resets tracking stores, and triggers an inbox rescan
// so any artifacts that were pending the baseline can now be processed.
bool SyncWorker::processBaselineResponseArtifact(const DecodeResult& dec,
                                                 const QString& /*artifactName*/) {
    if (!wconnPtr_ || !hPtr_)
        return false;

    const BaselineResponsePayload& resp = dec.baselineResponse;

    // Verify this response was sent to us.
    if (!resp.requestOrigin.isEmpty() && resp.requestOrigin != config_.nodeId())
        return true;  // not for this node — silently consume

    BaselineManager bm;
    BaselineManager::BaselineArtifact art;
    art.data = resp.baselineData;
    art.sourceMaxSeq = resp.sourceMaxSeq;

    ConsistencyCache cache;
    qint64 newAnchorSeq = 0;
    QString applyErr;
    const QString schemaFp = guard_ ? guard_->fingerprint() : QString();

    if (!bm.applyBaseline(*wconnPtr_, hPtr_, art, *av_, *ts_, *rw_, cache, resp.streamEpoch,
                          resp.origin, schemaFp, &newAnchorSeq, &applyErr)) {
        emit errorOccurred({err::E_SYNC_BASELINE_FAILED, Severity::Error,
                            QStringLiteral("baseline_response"), dec.header.origin, applyErr});
        return false;
    }

    // Remove from in-flight tracking so the next gap scan doesn't re-request.
    baselineRequestsInFlight_.remove(resp.pendingArtifactName);

    // Trigger rescan so any artifacts that were deferred waiting for this baseline are now applied.
    enqueue([this]() { scanInbox(); });

    emit errorOccurred(
        {err::W_SYNC_UNTRACKED_CHANGE, Severity::Warning, QStringLiteral("baseline"),
         dec.header.origin,
         QStringLiteral("Baseline from '%1' applied successfully; anchor seq=%2; rescanning inbox")
             .arg(dec.header.origin)
             .arg(newAnchorSeq)});
    return true;
}

bool SyncWorker::processAckArtifact(const QString& path, const QString& name) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QByteArray data = f.readAll();
    f.close();

    // I-15 fix: parse the sending peer from the artifact file name.
    // ACK file name format: ack__{fromPeer}__{toPeer}__{ms}.ack
    // parts[0]="ack", parts[1]=fromPeer, parts[2]=toPeer, parts[3]=ms+".ack"
    QString peer;
    {
        QStringList parts = name.split(QStringLiteral("__"));
        if (parts.size() >= 3)
            peer = parts[1];  // fromPeer
    }

    // Try changeset ACK first
    ChangesetAck csAck;
    if (codec_->decodeChangesetAck(data, &csAck)) {
        // M-09 fix: the ACK payload is self-describing; ignore ACKs not addressed to this node
        // (e.g. mis-routed or renamed artifacts).
        if (!csAck.toPeer.isEmpty() && csAck.toPeer != config_.nodeId()) {
            emit errorOccurred({err::W_SYNC_UNTRACKED_CHANGE, Severity::Warning,
                                QStringLiteral("ack"), csAck.origin,
                                QStringLiteral("changeset ACK addressed to %1, not this node %2")
                                    .arg(csAck.toPeer, config_.nodeId())});
            return true;  // consumed (not ours), don't reprocess
        }
        // I-15 fix: pass peer (sender) as the first peer argument, not csAck.origin.
        if (!peer.isEmpty())
            ackStore_->updateAcked(*wconnPtr_, peer, csAck.origin, csAck.streamEpoch,
                                   csAck.appliedSeq, nullptr);
        // J-02: ACK arrival satisfies any pending foreground sync() wait → Completed.
        if (ackWaiting_.exchange(false)) {
            SyncProgress p;
            p.state = SyncState::Completed;
            p.percent = 100;
            emit progressUpdated(p);
        }
        evaluatePeers();
        return true;
    }
    // C-05 fix: process PushChunkAck — record per-chunk status and check for push completion.
    PushChunkAck chunkAck;
    if (codec_->decodeChunkAck(data, &chunkAck)) {
        if (chunkAck.pushId.isEmpty())
            return false;
        // M-09 fix: ignore chunk ACKs not addressed to this node.
        if (!chunkAck.toPeer.isEmpty() && chunkAck.toPeer != config_.nodeId()) {
            emit errorOccurred({err::W_SYNC_UNTRACKED_CHANGE, Severity::Warning,
                                QStringLiteral("ack"), chunkAck.pushId,
                                QStringLiteral("chunk ACK addressed to %1, not this node %2")
                                    .arg(chunkAck.toPeer, config_.nodeId())});
            return true;
        }
        // C-04 fix: ignore chunk ACKs that belong to a different (stale) push operation.
        // This prevents an ACK from a previous enqueueSelectionPush() from completing the
        // ackWaiting_ that was armed for the current push.
        if (!pendingPushId_.isEmpty() && chunkAck.pushId != pendingPushId_) {
            return true;  // silently consume — the artifact is valid but for a different operation
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

        // Record this chunk's ACK in push_chunk_progress.
        QSqlQuery markQ(*wconnPtr_);
        markQ.prepare(
            QStringLiteral("INSERT INTO __sync_push_chunk_progress "
                           "(push_id, chunk_seq, status, checksum, applied_ms) "
                           "VALUES (?, ?, 'applied', ?, ?) "
                           "ON CONFLICT(push_id, chunk_seq) DO UPDATE SET "
                           "  status='applied', checksum=excluded.checksum, "
                           "  applied_ms=excluded.applied_ms"));
        markQ.addBindValue(chunkAck.pushId);
        markQ.addBindValue(chunkAck.chunkSeq);
        markQ.addBindValue(chunkAck.checksum);
        markQ.addBindValue(nowMs);
        markQ.exec();

        // Check whether all chunks have been ACKed.
        if (chunkAck.totalChunks > 0) {
            QSqlQuery countQ(*wconnPtr_);
            countQ.prepare(
                QStringLiteral("SELECT COUNT(*) FROM __sync_push_chunk_progress "
                               "WHERE push_id=? AND status='applied'"));
            countQ.addBindValue(chunkAck.pushId);
            if (countQ.exec() && countQ.next()) {
                int ackedCount = countQ.value(0).toInt();
                if (ackedCount >= chunkAck.totalChunks) {
                    // All chunks ACKed — mark push_progress done and complete foreground op.
                    QSqlQuery doneQ(*wconnPtr_);
                    doneQ.prepare(
                        QStringLiteral("UPDATE __sync_push_progress "
                                       "SET status='done', updated_ms=? WHERE push_id=?"));
                    doneQ.addBindValue(nowMs);
                    doneQ.addBindValue(chunkAck.pushId);
                    doneQ.exec();

                    // C-04 fix: clear pendingPushId_ so future unrelated chunk ACKs don't
                    // match this operation (belt-and-suspenders alongside the pushId check above).
                    pendingPushId_.clear();
                    // Transition foreground state to Completed (design §5.4 / FR-11).
                    if (ackWaiting_.exchange(false)) {
                        SyncProgress p;
                        p.state = SyncState::Completed;
                        p.percent = 100;
                        emit progressUpdated(p);
                    }
                }
            }
        }
        evaluatePeers();
        return true;
    }
    return false;
}

bool SyncWorker::broadcast(QString* outErr) {
    bool wroteAny = false;
    for (const QString& peer : config_.peerNodes()) {
        if (isPeerEvicted(peer))
            continue;
        QString peerErr;
        if (broadcastTopeer(peer, &peerErr))
            wroteAny = true;
        if (!peerErr.isEmpty() && outErr && outErr->isEmpty())
            *outErr = peerErr;
    }
    QString ackErr;
    if (!ackChan_->flush(*codec_, &ackErr)) {
        emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Warning, QStringLiteral("ack"),
                            config_.nodeId(), ackErr});
        if (outErr && outErr->isEmpty())
            *outErr = ackErr;
    }
    evaluatePeers();
    return wroteAny;
}

bool SyncWorker::broadcastTopeer(const QString& peer, QString* outErr) {
    // J-01 fix: use readRangeAll(excludeOrigin=peer) so we:
    //   (a) never echo peer's own changes back to it, and
    //   (b) always include our own local changes (which readRange(origin=peer) missed).
    // The watermark is now local_seq-based (lastSentLocalSeq) rather than per-origin-based.
    qint64 afterLocalSeq = ackStore_->lastSentLocalSeq(*wconnPtr_, peer, streamEpoch_);
    if (afterLocalSeq < 0) {
        // First time: start from -1 so all existing entries are eligible.
        afterLocalSeq = -1;
    }

    // Read pending entries from changelog
    QList<ChangelogStore::EntryFull> entries = clog_->readRangeAll(
        *wconnPtr_, peer, afterLocalSeq, static_cast<int>(config_.broadcastThreshold()));

    qint64 bytesSent = 0;
    qint64 lastSentLocal = afterLocalSeq;
    bool wroteAny = false;
    for (const auto& entry : entries) {
        // H-02 fix: shouldRoute must compare entry.originSeq against the per-(peer,origin)
        // acked_seq, NOT against afterLocalSeq which is a local_seq watermark in a different
        // sequence namespace.  Mixing them causes phantom misses and double-sends.
        const qint64 peerOriginAcked =
            ackStore_->ackedSeq(*wconnPtr_, peer, entry.origin, streamEpoch_);
        if (!routing_->shouldRoute(peer, entry.origin, entry.originSeq, peerOriginAcked))
            continue;

        // I-16: Rebase the changeset onto any stored rebase buffer before broadcast.
        QByteArray changesetToSend = entry.changeset;
        QString rebaseKey = entry.origin + QLatin1Char('/') + QString::number(entry.originSeq);
        if (rebaseBuffers_.contains(rebaseKey) && rebaser_) {
            QByteArray rebased;
            QString rebaseErr;
            if (rebaser_->rebase(rebaseBuffers_.value(rebaseKey), entry.changeset, &rebased,
                                 &rebaseErr)) {
                changesetToSend = rebased;
            } else {
                // M-03 fix: rebase failure is Error (not Warning) so SyncEngine's
                // onWorkerError transitions foreground state to Failed when applicable.
                emit errorOccurred({err::E_SYNC_REBASE_FAILED, Severity::Error,
                                    QStringLiteral("broadcast"), peer, rebaseErr});
                continue;  // skip this entry — don't send un-rebased
            }
        }

        // Encode and write to outbox
        PayloadHeader hdr;
        hdr.origin = entry.origin;
        hdr.originSeq = entry.originSeq;
        hdr.streamEpoch = streamEpoch_;
        hdr.schemaVer = config_.schemaVersion();
        hdr.schemaFingerprint = guard_ ? guard_->fingerprint() : QString();
        hdr.routeTag = peer;

        QByteArray payload = codec_->encodeChangeset(hdr, changesetToSend);
        // H-07 / H-08 fix: include target peer so different peers get distinct file names.
        QString artifactName =
            ddl::changesetArtifactName(entry.origin, hdr.streamEpoch, entry.originSeq, peer);
        QString writeErr;
        if (!outbox_->write(artifactName, payload, &writeErr)) {
            emit errorOccurred(
                {err::E_SYNC_TRANSPORT, Severity::Warning, "broadcast", peer, writeErr});
            if (outErr)
                *outErr = writeErr;
            break;
        }
        wroteAny = true;
        bytesSent += payload.size();
        lastSentLocal = qMax(lastSentLocal, entry.localSeq);
        if (bytesSent >= config_.outboxMaxBytesPerPeer())
            break;
    }

    // Advance the send-watermark so the next broadcast starts where we left off.
    if (lastSentLocal > afterLocalSeq)
        ackStore_->updateLastSent(*wconnPtr_, peer, streamEpoch_, lastSentLocal, nullptr);
    return wroteAny;
}

qint64 SyncWorker::computePeerAckedSeq(const QString& peer) {
    // I-15 fix: query the acked seq for this specific peer, not the global min.
    return ackStore_->ackedSeq(*wconnPtr_, peer, config_.nodeId(), streamEpoch_);
}

bool SyncWorker::isPeerEvicted(const QString& peer) {
    if (!wconnPtr_)
        return false;
    QSqlQuery q(*wconnPtr_);
    q.prepare(
        QStringLiteral("SELECT COALESCE(MAX(pending_baseline), 0) "
                       "FROM __sync_outbound_ack WHERE peer = ?"));
    q.addBindValue(peer);
    if (!q.exec() || !q.next())
        return false;
    return q.value(0).toInt() != 0;
}

qint64 SyncWorker::peerLastAckMs(const QString& peer) {
    QSqlQuery q(*wconnPtr_);
    q.prepare(
        QStringLiteral("SELECT COALESCE(MAX(last_ack_ms), 0) "
                       "FROM __sync_outbound_ack "
                       "WHERE peer = ? AND origin != '__broadcast__'"));
    q.addBindValue(peer);
    if (!q.exec() || !q.next())
        return 0;
    return q.value(0).toLongLong();
}

qint64 SyncWorker::peerLagBytes(const QString& peer, qint64 afterLocalSeq) {
    QSqlQuery q(*wconnPtr_);
    q.prepare(
        QStringLiteral("SELECT COALESCE(SUM(byte_size), 0) "
                       "FROM __sync_changelog "
                       "WHERE origin != ? AND local_seq > ?"));
    q.addBindValue(peer);
    q.addBindValue(afterLocalSeq);
    if (!q.exec() || !q.next())
        return 0;
    return q.value(0).toLongLong();
}

void SyncWorker::evaluatePeers() {
    if (!wconnPtr_ || !evictor_)
        return;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 localHead = clog_->maxLocalSeq(*wconnPtr_);
    for (const QString& peer : config_.peerNodes()) {
        const bool alreadyEvicted = isPeerEvicted(peer);
        const qint64 lastSent = ackStore_->lastSentLocalSeq(*wconnPtr_, peer, streamEpoch_);

        DeadPeerEvictor::PeerState state;
        state.peer = peer;
        state.lastAckMs = peerLastAckMs(peer);
        state.lagSeq = qMax<qint64>(0, localHead - lastSent);
        state.lagBytes = peerLagBytes(peer, lastSent);
        state.evicted = alreadyEvicted;

        const auto level = evictor_->evaluate(state, nowMs);
        if (level == DeadPeerEvictor::AlertLevel::Dead && !alreadyEvicted) {
            QString evictErr;
            if (evictor_->evict(*wconnPtr_, peer, *ackStore_, &evictErr)) {
                emit errorOccurred({err::E_SYNC_PEER_DEAD, Severity::Error, QStringLiteral("peer"),
                                    peer,
                                    QStringLiteral("Peer evicted; pending baseline required")});
            } else {
                emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Warning,
                                    QStringLiteral("peer"), peer, evictErr});
            }
        } else if (level == DeadPeerEvictor::AlertLevel::Lagging) {
            emit errorOccurred({err::W_SYNC_PEER_LAGGING, Severity::Warning, QStringLiteral("peer"),
                                peer, QStringLiteral("Peer lag exceeds soft threshold")});
        }
    }
}

bool SyncWorker::runBaselineFallbackFor(const QString& artifactName) {
    if (!wconnPtr_ || !hPtr_)
        return false;

    const QString path = config_.inboxDir() + QDir::separator() + artifactName;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = f.readAll();
    f.close();

    DecodeResult dec;
    QString decErr;
    if (!codec_->decode(data, &dec, &decErr) || dec.kind != PayloadKind::Changeset)
        return false;

    BaselineManager baseline;
    const qint64 applied = av_->current(*wconnPtr_, dec.header.origin, dec.header.streamEpoch);
    if (!baseline.shouldFallbackToBaseline(applied, dec.header.originSeq))
        return false;

    // C-1 fix: request a source-authoritative baseline over the transport channel.
    // If a request is already in-flight for this artifact, don't re-send (debounce).
    if (baselineRequestsInFlight_.contains(artifactName)) {
        // Already requested — artifact stays pending; wait for baseline response.
        return false;
    }

    // Generate a BaselineRequest artifact and write it to the outbox addressed to the origin node.
    // The response handler (processBaselineResponseArtifact) will apply the baseline and then
    // rescan the inbox so this pending artifact gets retried.
    BaselineRequestPayload req;
    req.origin = dec.header.origin;
    req.streamEpoch = dec.header.streamEpoch;
    req.fromSeq = applied;
    req.pendingArtifactName = artifactName;
    req.requestedTables = canonicalSyncTables_;

    PayloadHeader reqHdr;
    reqHdr.origin = config_.nodeId();
    reqHdr.originSeq = nextLocalOriginSeq();
    reqHdr.streamEpoch = streamEpoch_;
    reqHdr.schemaVer = config_.schemaVersion();
    reqHdr.schemaFingerprint = guard_ ? guard_->fingerprint() : QString();
    reqHdr.routeTag = dec.header.origin;

    QByteArray reqPayload = codec_->encodeBaselineRequest(reqHdr, req);
    const QString reqName = QStringLiteral("blreq__") + config_.nodeId() + QStringLiteral("__") +
                            dec.header.origin + QStringLiteral("__") +
                            QString::number(QDateTime::currentMSecsSinceEpoch()) +
                            QStringLiteral(".payload");

    QString writeErr;
    if (!outbox_->write(reqName, reqPayload, &writeErr)) {
        // Cannot send request — quarantine the artifact as before to avoid infinite rescan.
        emit errorOccurred(
            {err::E_SYNC_GAP, Severity::Error, QStringLiteral("baseline"), dec.header.origin,
             QStringLiteral("Gap for origin=%1 seq=%2: cannot send BaselineRequest: %3")
                 .arg(dec.header.origin)
                 .arg(dec.header.originSeq)
                 .arg(writeErr)});
        if (!config_.quarantineDir().isEmpty()) {
            QDir qDir(config_.quarantineDir());
            qDir.mkpath(QStringLiteral("."));
            if (QFile::copy(path, qDir.filePath(artifactName)))
                QFile::remove(path);
        }
        ledger_->markConsumed(*wconnPtr_, artifactName, nullptr);
        return false;
    }

    // Request sent — mark artifact as in-flight and keep it in the pending ledger.
    baselineRequestsInFlight_.insert(artifactName);
    emit errorOccurred(
        {err::E_SYNC_GAP, Severity::Warning, QStringLiteral("baseline"), dec.header.origin,
         QStringLiteral("Gap for origin=%1 seq=%2: BaselineRequest sent, waiting for response")
             .arg(dec.header.origin)
             .arg(dec.header.originSeq)});
    return false;
}

// I-04: Submit import to run on the worker thread using wconn + session capture.
// C-03 fix: accepts pre-snapshotted profile/catalog so the worker never touches
// DataBridge::db_ or its mutable members from the wrong thread.
ImportResult SyncWorker::submitImportSync(const ImportOptions& opts, const QString& xlsxPath,
                                          const detail::ProfileSpec& profile,
                                          const detail::SchemaCatalog& catalog) {
    if (!isRunning() || !wconnPtr_) {
        // Worker not started — return error; cannot fall back to main-thread db_ (C-03).
        ImportResult r;
        RowError e;
        e.code = QLatin1String(err::E_SYNC_INIT);
        e.message = QStringLiteral(
            "SyncWorker not ready; import rejected to avoid cross-thread db_ access");
        r.errors.append(e);
        return r;
    }

    auto sharedPromise = std::make_shared<std::promise<ImportResult>>();
    std::future<ImportResult> future = sharedPromise->get_future();

    // profile/catalog are value-copied into the lambda — worker never touches main-thread objects.
    enqueue([this, opts, xlsxPath, profile, catalog, sp = sharedPromise]() mutable {
        if (!wconnPtr_ || !hPtr_) {
            ImportResult r;
            RowError e;
            e.code = QLatin1String(err::E_SYNC_INIT);
            e.message = QStringLiteral("wconn not available in worker");
            r.errors.append(e);
            sp->set_value(r);
            return;
        }

        // CapturedWriteTemplate branch C: fresh session capture around the UPSERT writes.
        WriteTxn txn(*wconnPtr_);
        QString txnErr;
        if (!txn.begin(&txnErr)) {
            ImportResult r;
            RowError e;
            e.code = QLatin1String(err::E_SYNC_INIT);
            e.message = txnErr;
            r.errors.append(e);
            sp->set_value(r);
            return;
        }

        // C-02 fix: check rec_->begin() return value; if it fails, the import cannot be
        // session-captured and the changelog entry would be missing → reject the whole import.
        QString sessionErr;
        if (!rec_->begin(hPtr_, canonicalSyncTables_, &sessionErr)) {
            txn.rollback();
            ImportResult errResult;
            RowError e;
            e.code = QLatin1String(err::E_SYNC_INIT);
            e.message =
                QStringLiteral("session begin failed (changelog unavailable): %1").arg(sessionErr);
            errResult.errors.append(e);
            sp->set_value(errResult);
            return;
        }

        // C-05 fix: pass manageTransaction=false so ImportService does not open an inner
        // db.transaction() while WriteTxn holds the outer BEGIN IMMEDIATE.
        detail::ImportService svc;
        ImportResult result = svc.run(profile, catalog, xlsxPath, opts, *wconnPtr_,
                                      /*manageTransaction=*/false);

        if (result.ok) {
            qint64 localSeq = 0;
            QString sealErr;
            const QString fp = guard_ ? guard_->fingerprint() : QString();
            const qint64 originSeq = nextLocalOriginSeq();
            // C-02 fix: check sealInto() return value.
            if (!rec_->sealInto(hPtr_, *clog_, *wconnPtr_, txn, config_.nodeId(), streamEpoch_,
                                config_.schemaVersion(), fp, 0, originSeq, &localSeq, &sealErr)) {
                rec_->abort();
                txn.rollback();
                result.ok = false;
                RowError e;
                e.code = QLatin1String(err::E_SYNC_INIT);
                e.message = QStringLiteral("changelog seal failed: %1").arg(sealErr);
                result.errors.append(e);
                sp->set_value(result);
                return;
            }
            // C-02 fix: check txn.commit() return value.
            QString commitErr;
            if (!txn.commit(&commitErr)) {
                result.ok = false;
                RowError e;
                e.code = QLatin1String(err::E_SYNC_INIT);
                e.message = QStringLiteral("transaction commit failed: %1").arg(commitErr);
                result.errors.append(e);
            }
        } else {
            rec_->abort();
            txn.rollback();
        }
        sp->set_value(result);
    });

    // Block until worker executes the task (with timeout).
    // The shared_ptr keeps the promise alive even if we return early.
    if (future.wait_for(std::chrono::seconds(60)) == std::future_status::timeout) {
        ImportResult r;
        RowError e;
        e.code = QLatin1String(err::E_SYNC_INIT);
        e.message = QStringLiteral("submitImportSync timed out after 60s");
        r.errors.append(e);
        return r;
    }
    return future.get();
}

qint64 SyncWorker::nextLocalOriginSeq() {
    return ++localOriginSeq_;
}

// C-05 fix: routes RowMutations through CapturedWriteTemplate (session capture + changelog seal)
// instead of directly through UpsertExecutor. This ensures comparison-session saves produce
// changelog entries that are broadcast to peers, matching the semantics of a normal local write.
bool SyncWorker::submitCaptureWriteSync(const QList<RowMutation>& mutations,
                                        const QStringList& syncTables, QString* err) {
    if (!isRunning() || !wconnPtr_) {
        if (err)
            *err = QStringLiteral("SyncWorker not ready");
        return false;
    }
    if (mutations.isEmpty())
        return true;

    auto sharedPromise = std::make_shared<std::promise<QPair<bool, QString>>>();
    std::future<QPair<bool, QString>> future = sharedPromise->get_future();

    enqueue([this, mutations, syncTables, sp = sharedPromise]() {
        if (!wconnPtr_ || !tpl_) {
            sp->set_value(qMakePair(false, QStringLiteral("wconn/tpl not available")));
            return;
        }

        WriteParams p;
        p.kind = WriteKind::LocalWrite;
        p.origin = config_.nodeId();
        p.epoch = streamEpoch_;
        p.seq = nextLocalOriginSeq();
        p.schemaVer = config_.schemaVersion();
        p.schemaFp = guard_ ? guard_->fingerprint() : QString();
        p.mutations = mutations;
        p.syncTables = syncTables.isEmpty() ? canonicalSyncTables_ : syncTables;

        WriteResult res = tpl_->execute(p);
        sp->set_value(qMakePair(res.ok, res.errorMsg.isEmpty() ? res.errorCode : res.errorMsg));
    });

    if (future.wait_for(std::chrono::seconds(60)) == std::future_status::timeout) {
        if (err)
            *err = QStringLiteral("submitCaptureWriteSync timed out after 60s");
        return false;
    }
    const auto result = future.get();
    if (!result.first && err)
        *err = result.second;
    return result.first;
}

// I-19: Signal worker that a foreground sync() is waiting for ACK.
void SyncWorker::startAckWait() {
    ackWaiting_ = true;
    ackDeadlineMs_ = QDateTime::currentMSecsSinceEpoch() + config_.ackMaxDelayMs();
}

// C-02: Enqueue an immediate drain cycle on the worker thread.
bool SyncWorker::enqueueDrain(QString* err) {
    if (!isRunning()) {
        if (err)
            *err = QStringLiteral("SyncWorker is not running");
        return false;
    }
    auto sharedPromise = std::make_shared<std::promise<bool>>();
    std::future<bool> future = sharedPromise->get_future();
    enqueue([this, sp = sharedPromise]() {
        QString taskErr;
        scanInbox();                             // apply any pending inbound payloads
        const bool wrote = broadcast(&taskErr);  // pack & write outbox artifacts for all peers
        if (!taskErr.isEmpty()) {
            emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Error, QStringLiteral("sync"),
                                config_.nodeId(), taskErr});
        }
        sp->set_value(wrote);
    });
    if (future.wait_for(std::chrono::seconds(60)) == std::future_status::timeout) {
        if (err)
            *err = QStringLiteral("enqueueDrain timed out after 60s");
        return false;
    }
    return future.get();
}

// C-01: Enqueue a selection push — SelectionResolver → FkClosureBuilder → ChunkStreamer
//        → PayloadCodec → OutboxWriter (design §5.5 / §7.3).
void SyncWorker::enqueueSelectionPush(const SyncSelection& selection,
                                      const detail::SchemaCatalog& catalog) {
    enqueue([this, selection, catalog]() {
        if (!wconnPtr_) {
            emit errorOccurred({err::E_SYNC_INIT, Severity::Error, QStringLiteral("syncSelected"),
                                QString(), QStringLiteral("Worker wconn not ready")});
            return;
        }

        // Step 1: open a short-lived read-only connection for snapshot reads.
        const QString rConnName = config_.sqlitePath() + QStringLiteral("_sel_ro_") +
                                  QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        QSqlDatabase rconn = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), rConnName);
        rconn.setDatabaseName(config_.sqlitePath());
        auto cleanupRconn = [&] {
            if (rconn.isOpen())
                rconn.close();
            QSqlDatabase::removeDatabase(rConnName);
        };
        if (!rconn.open()) {
            cleanupRconn();
            emit errorOccurred({err::E_SYNC_INIT, Severity::Error, QStringLiteral("syncSelected"),
                                QString(), QStringLiteral("Cannot open read connection")});
            return;
        }

        // Step 2: resolve PK set.
        SelectionResolver resolver;
        QList<SelectionResolver::ResolveResult> resolved;
        QString resolveErr;
        if (!resolver.resolvePk(rconn, selection, &resolved, &resolveErr)) {
            cleanupRconn();
            emit errorOccurred({err::E_SYNC_SELECTION_EMPTY, Severity::Error,
                                QStringLiteral("syncSelected"), QString(), resolveErr});
            return;
        }
        if (resolved.isEmpty()) {
            cleanupRconn();
            emit errorOccurred({err::E_SYNC_SELECTION_EMPTY, Severity::Error,
                                QStringLiteral("syncSelected"), QString(),
                                QStringLiteral("Selection resolved to zero rows")});
            return;
        }

        // Step 3: FK closure + topo sort.
        ConsistencyCache cache;
        FkClosureBuilder builder;
        QList<FkClosureBuilder::Entry> manifest;
        QString buildErr;
        if (!builder.build(rconn, resolved, catalog, cache, config_.maxSelectionSize(), &manifest,
                           &buildErr)) {
            cleanupRconn();
            const char* code =
                buildErr.contains(QLatin1String("cycle"))   ? err::E_SYNC_FK_CYCLE_UNSUPPORTED
                : buildErr.contains(QLatin1String("large")) ? err::E_SYNC_SELECTION_TOO_LARGE
                                                            : err::E_SYNC_FK_CLOSURE_MISSING;
            emit errorOccurred(
                {code, Severity::Error, QStringLiteral("syncSelected"), QString(), buildErr});
            return;
        }
        cleanupRconn();  // release read snapshot promptly (design §5.5/E-11)

        // Step 4: chunk into outbox artifacts.
        ChunkStreamer streamer;
        QList<ChunkStreamer::Chunk> chunks;
        QString streamErr;
        if (!streamer.stream(manifest, config_.nodeId(), QString(), config_.pushChunkBudgetBytes(),
                             *codec_, &chunks, &streamErr)) {
            emit errorOccurred({err::E_SYNC_SELECTION_TOO_LARGE, Severity::Error,
                                QStringLiteral("syncSelected"), QString(), streamErr});
            return;
        }

        // Step 5: encode and write each chunk to the outbox.
        // H-01 fix: capture schemaFingerprint before entering the loop.
        const QString schemaFp = guard_ ? guard_->fingerprint() : QString();
        const QString pushId = chunks.isEmpty() ? QString() : chunks.first().pushId;

        // C-04 fix: record which push we're waiting to be fully ACKed.
        // processAckArtifact() will only complete ackWaiting_ when ALL chunks of THIS pushId
        // are ACKed, preventing stale or unrelated chunk ACKs from prematurely completing
        // the foreground sync() caller.
        pendingPushId_ = pushId;

        // M-05 / C-02: create push_progress(streaming) on the worker thread BEFORE writing chunks.
        // L-01 fix: ON CONFLICT DO UPDATE so re-sends refresh total_chunks/status/updated_ms.
        if (!pushId.isEmpty() && wconnPtr_) {
            QSqlQuery ins(*wconnPtr_);
            ins.prepare(QStringLiteral(
                "INSERT INTO __sync_push_progress "
                "(push_id, origin, peer, total_chunks, schema_ver, status, updated_ms)"
                " VALUES (?, ?, '', ?, ?, 'streaming', ?)"
                " ON CONFLICT(push_id) DO UPDATE SET"
                "  status='streaming', total_chunks=excluded.total_chunks,"
                "  updated_ms=excluded.updated_ms"));
            ins.addBindValue(pushId);
            ins.addBindValue(config_.nodeId());
            ins.addBindValue(chunks.size());
            ins.addBindValue(config_.schemaVersion());
            ins.addBindValue(QDateTime::currentMSecsSinceEpoch());
            ins.exec();
        }

        for (const ChunkStreamer::Chunk& chunk : chunks) {
            PayloadHeader hdr;
            hdr.origin = config_.nodeId();
            hdr.originSeq = 0;  // SelectionPush has no changelog seq
            hdr.streamEpoch = streamEpoch_;
            hdr.schemaVer = config_.schemaVersion();
            hdr.schemaFingerprint =
                schemaFp;  // H-01 fix: must be filled so receiver SchemaGuard accepts
            hdr.pushId = chunk.pushId;
            hdr.chunkSeq = chunk.chunkSeq;
            hdr.totalChunks = chunk.totalChunks;

            SelectionPushBody body;
            body.totalChunks = chunk.totalChunks;
            body.frozenEntries = chunk.entries;
            for (const QVariantMap& row : chunk.rows)
                body.rows.append(row);

            QByteArray payload = codec_->encodeSelectionPush(hdr, body);
            // H-07 / H-08 fix: include center node as target peer to disambiguate artifacts.
            const QString centerPeer = config_.centerNodeId().isEmpty() ? QStringLiteral("center")
                                                                        : config_.centerNodeId();
            const QString artifactName =
                ddl::selectionPushArtifactName(chunk.pushId, chunk.chunkSeq, centerPeer);
            QString writeErr;
            if (!outbox_->write(artifactName, payload, &writeErr)) {
                emit errorOccurred({err::E_SYNC_TRANSPORT, Severity::Error,
                                    QStringLiteral("syncSelected"), QString(), writeErr});
                return;
            }
        }

        // Report outbox write as "Exporting" — ACK wait was armed by SyncEngine.
        SyncProgress p;
        p.state = SyncState::Exporting;
        p.percent = -1;
        emit progressUpdated(p);
    });
}

}  // namespace dbridge::sync
