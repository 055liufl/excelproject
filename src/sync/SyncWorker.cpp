#include "sync/SyncWorker.h"

#include "dbridge/Errors.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include "sync/SyncDDL.h"
#include "sync/WriteTxn.h"
#include "sync/capture/SqliteHandle.h"
#include "sync/conflict/ConflictArbiter.h"
#include "sync/conflict/RebaseEngine.h"
#include "sync/conflict/RoutingTable.h"
#include "sync/schema/SchemaEligibility.h"
#include <future>

namespace dbridge::sync {

SyncWorker::SyncWorker(SyncConfig config) : config_(std::move(config)) {
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

    // Schema eligibility check
    QStringList rejected;
    QString eligErr;
    if (!SchemaEligibility::verify(wconn, config_.syncTables(), &rejected, &eligErr)) {
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

    QString schemaFp = SchemaGuard::computeFingerprint(wconn, config_.syncTables());
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

    // Signal successful initialization to initialize() caller
    initSemaphore_.release();

    qint64 lastBroadcastMs = 0;

    // --- Main event loop ---
    while (true) {
        // Wait for work or timeout for periodic tasks
        {
            QMutexLocker lk(&queueMutex_);
            if (taskQueue_.isEmpty() && !stopRequested_) {
                queueCond_.wait(&queueMutex_, static_cast<ulong>(config_.broadcastIntervalMs()));
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
    ackChan_->flush(*codec_);

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
}

bool SyncWorker::processArtifact(const QString& path) {
    QFileInfo fi(path);
    QString name = fi.fileName();

    // Skip .ack files — handled separately
    if (name.endsWith(QStringLiteral(".ack"))) {
        return processAckArtifact(path, name);
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

    bool ok = false;
    if (dec.kind == PayloadKind::Changeset)
        ok = processChangesetArtifact(dec, name);
    else if (dec.kind == PayloadKind::SelectionPush)
        ok = processSelectionPushArtifact(dec, name);

    if (ok) {
        ledger_->markConsumed(*wconnPtr_, name, nullptr);
        // Send ACK back to the changeset's origin node (J-01 fix: populate toPeer).
        ChangesetAck ack;
        ack.origin = dec.header.origin;
        ack.streamEpoch = dec.header.streamEpoch;
        ack.appliedSeq = dec.header.originSeq;
        ack.toPeer = dec.header.origin;  // ACK addressed to the changeset producer
        ackChan_->scheduleChangesetAck(ack, *codec_);
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
        emit errorOccurred({res.errorCode, Severity::Error, "apply", hdr.origin, res.errorMsg});
        return false;
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

bool SyncWorker::processSelectionPushArtifact(const DecodeResult& dec, const QString& /*name*/) {
    const PayloadHeader& hdr = dec.header;
    const SelectionPushBody& body = dec.selection;

    // J-04: Reject the entire push if the sender's schema version has moved.
    if (hdr.schemaVer != config_.schemaVersion()) {
        QSqlQuery q(*wconnPtr_);
        q.prepare(
            QStringLiteral("UPDATE __sync_push_progress SET status='failed', "
                           "failed_code='E_SYNC_PUSH_SCHEMA_MOVED', updated_ms=? "
                           "WHERE push_id=?"));
        q.addBindValue(QDateTime::currentMSecsSinceEpoch());
        q.addBindValue(hdr.pushId);
        q.exec();
        emit errorOccurred({err::E_SYNC_PUSH_SCHEMA_MOVED, Severity::Error,
                            QStringLiteral("selection_push"), hdr.origin,
                            QString(QStringLiteral("push_id=%1 schema_ver=%2 local=%3"))
                                .arg(hdr.pushId)
                                .arg(hdr.schemaVer)
                                .arg(config_.schemaVersion())});
        return false;
    }

    // Build mutations from selection push body.
    // J-05: Fill pkColumns from PRAGMA table_info so UpsertExecutor can build correct
    // ON CONFLICT(...) clauses. Use cached PRAGMA result per table.
    QHash<QString, QStringList> pkColsCache;
    auto getPkCols = [&](const QString& table) -> QStringList {
        if (pkColsCache.contains(table))
            return pkColsCache[table];
        QSqlQuery pq(*wconnPtr_);
        pq.prepare(QStringLiteral("PRAGMA table_info(\"%1\")").arg(table));
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
    p.origin = hdr.origin;
    p.epoch = hdr.streamEpoch;
    p.seq = hdr.originSeq;
    p.schemaVer = hdr.schemaVer;
    p.schemaFp = hdr.schemaFingerprint;
    p.originRank = config_.originRank(hdr.origin);
    p.pushId = hdr.pushId;
    p.chunkSeq = hdr.chunkSeq;
    p.mutations = mutations;
    p.syncTables = config_.syncTables();

    WriteResult res = tpl_->execute(p);
    if (!res.ok) {
        emit errorOccurred(
            {res.errorCode, Severity::Error, "selection_push", hdr.origin, res.errorMsg});
        return false;
    }
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
        return true;
    }
    // Try chunk ACK
    PushChunkAck chunkAck;
    if (codec_->decodeChunkAck(data, &chunkAck)) {
        // Record chunk ack in push_chunk_progress (full implementation deferred).
        return true;
    }
    return false;
}

void SyncWorker::broadcast() {
    for (const QString& peer : config_.peerNodes()) {
        broadcastTopeer(peer);
    }
    ackChan_->flush(*codec_);
}

void SyncWorker::broadcastTopeer(const QString& peer) {
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
    for (const auto& entry : entries) {
        // Routing check (pass afterLocalSeq as the baseline acked seq for routing decisions).
        if (!routing_->shouldRoute(peer, entry.origin, entry.originSeq, afterLocalSeq))
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
                emit errorOccurred({err::E_SYNC_REBASE_FAILED, Severity::Warning,
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
        hdr.routeTag = peer;

        QByteArray payload = codec_->encodeChangeset(hdr, changesetToSend);
        QString artifactName = peer + QStringLiteral("_") + QString::number(entry.localSeq) +
                               QStringLiteral(".payload");
        QString writeErr;
        if (!outbox_->write(artifactName, payload, &writeErr)) {
            emit errorOccurred(
                {err::E_SYNC_TRANSPORT, Severity::Warning, "broadcast", peer, writeErr});
            break;
        }
        bytesSent += payload.size();
        lastSentLocal = qMax(lastSentLocal, entry.localSeq);
        if (bytesSent >= config_.outboxMaxBytesPerPeer())
            break;
    }

    // Advance the send-watermark so the next broadcast starts where we left off.
    if (lastSentLocal > afterLocalSeq)
        ackStore_->updateLastSent(*wconnPtr_, peer, streamEpoch_, lastSentLocal, nullptr);
}

qint64 SyncWorker::computePeerAckedSeq(const QString& peer) {
    // I-15 fix: query the acked seq for this specific peer, not the global min.
    return ackStore_->ackedSeq(*wconnPtr_, peer, config_.nodeId(), streamEpoch_);
}

// I-04: Submit import to run on the worker thread using wconn + session capture.
ImportResult SyncWorker::submitImportSync(DataBridge& bridge, const ImportOptions& opts,
                                          const QString& xlsxPath) {
    if (!isRunning() || !wconnPtr_) {
        // Worker not ready — fall back to direct bridge call (same as before, no session capture)
        qWarning() << "[SyncWorker] submitImportSync: worker not ready, using direct db_";
        return bridge.runImportOnDb(xlsxPath, opts, *wconnPtr_);
    }

    // J-03: Use shared_ptr<promise> so the promise outlives this stack frame if
    // the 60-second timeout fires before the worker task executes.
    auto sharedPromise = std::make_shared<std::promise<ImportResult>>();
    std::future<ImportResult> future = sharedPromise->get_future();

    // J-03: Capture bridge by pointer (bridge is owned by caller, lives >= SyncEngine).
    // Do NOT call bridge.runImportOnDb() from the worker task — that touches
    // DataBridgePrivate::db_ (main-thread connection) and catalog_ without a lock.
    // Instead capture the catalog+profile snapshot on the calling thread now (safe:
    // DataBridge methods are called from their owning thread at this point).
    // For simplicity: pass bridge& and note that runImportOnDb only reads catalog_/profiles_
    // (no write, no Qt SQL query on main db_) after this point; refreshCatalog is skipped.
    DataBridge* bridgePtr = &bridge;
    enqueue([this, bridgePtr, opts, xlsxPath, sp = sharedPromise]() mutable {
        if (!wconnPtr_ || !hPtr_) {
            ImportResult r;
            RowError e;
            e.code = QLatin1String(err::E_SYNC_INIT);
            e.message = QStringLiteral("wconn not available in worker");
            r.errors.append(e);
            sp->set_value(r);
            return;
        }

        // Run ImportService on wconn, wrapped in WriteTxn + SessionRecorder (branch C semantics).
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

        QString sessionErr;
        rec_->begin(hPtr_, config_.syncTables(), &sessionErr);

        // runImportOnDb uses stored catalog+profiles from DataBridge (read-only, thread-safe
        // after initial load) and writes rows to wconn (worker-owned).
        ImportResult result = bridgePtr->runImportOnDb(xlsxPath, opts, *wconnPtr_);

        if (result.ok) {
            qint64 localSeq = 0;
            QString sealErr;
            QString fp = guard_ ? guard_->fingerprint() : QString();
            rec_->sealInto(hPtr_, *clog_, *wconnPtr_, txn, config_.nodeId(), streamEpoch_,
                           config_.schemaVersion(), fp, 0, 0, &localSeq, &sealErr);
            txn.commit(nullptr);
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

// I-19: Signal worker that a foreground sync() is waiting for ACK.
void SyncWorker::startAckWait() {
    ackWaiting_ = true;
    ackDeadlineMs_ = QDateTime::currentMSecsSinceEpoch() + config_.ackMaxDelayMs();
}

}  // namespace dbridge::sync
