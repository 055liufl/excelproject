#include "sync/SyncWorker.h"

#include "dbridge/Errors.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>

#include "sync/WriteTxn.h"
#include "sync/conflict/ConflictArbiter.h"
#include "sync/conflict/RebaseEngine.h"
#include "sync/conflict/RoutingTable.h"

namespace dbridge::sync {

SyncWorker::SyncWorker(QSqlDatabase& wconn, sqlite3* h, SyncConfig config)
    : wconn_(wconn), h_(h), config_(std::move(config)) {
    av_ = std::make_unique<AppliedVectorStore>();
    rw_ = std::make_unique<RowWinnerStore>();
    ts_ = std::make_unique<TableStateStore>();
    clog_ = std::make_unique<ChangelogStore>();
    rec_ = std::make_unique<SessionRecorder>();
    guard_ = std::make_unique<SchemaGuard>();
    applier_ = std::make_unique<ChangesetApplier>();
    outbox_ = std::make_unique<OutboxWriter>(config_.outboxDir());
    ledger_ = std::make_unique<InboxLedger>();
    ackChan_ = std::make_unique<AckChannel>(*outbox_, config_.ackMaxDelayMs());
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

void SyncWorker::run() {
    QString initErr;

    // --- One-time initialization on the worker thread ---
    streamEpoch_ = QDateTime::currentMSecsSinceEpoch();

    if (!av_->init(wconn_, &initErr)) {
        emit errorOccurred({err::E_SYNC_INIT, Severity::Fatal, "init", config_.nodeId(), initErr});
        return;
    }
    if (!rw_->init(wconn_, &initErr)) {
        emit errorOccurred({err::E_SYNC_INIT, Severity::Fatal, "init", config_.nodeId(), initErr});
        return;
    }
    if (!ts_->init(wconn_, &initErr)) {
        emit errorOccurred({err::E_SYNC_INIT, Severity::Fatal, "init", config_.nodeId(), initErr});
        return;
    }
    if (!clog_->init(wconn_, &initErr)) {
        emit errorOccurred({err::E_SYNC_INIT, Severity::Fatal, "init", config_.nodeId(), initErr});
        return;
    }
    if (!ledger_->init(wconn_, &initErr)) {
        emit errorOccurred({err::E_SYNC_INIT, Severity::Fatal, "init", config_.nodeId(), initErr});
        return;
    }
    if (!ackStore_->init(wconn_, &initErr)) {
        emit errorOccurred({err::E_SYNC_INIT, Severity::Fatal, "init", config_.nodeId(), initErr});
        return;
    }
    if (!quarantine_->init(wconn_, &initErr)) {
        emit errorOccurred({err::E_SYNC_INIT, Severity::Fatal, "init", config_.nodeId(), initErr});
        return;
    }

    QString schemaFp = SchemaGuard::computeFingerprint(wconn_, config_.syncTables());
    guard_->setLocal(config_.schemaVersion(), schemaFp);

    routing_->configure(config_.nodeId(), config_.peerNodes());
    arbiter_->setRankMap(config_.allRanks());

    // Initialize CapturedWriteTemplate now that all stores are ready
    tpl_ = std::make_unique<CapturedWriteTemplate>(wconn_, h_, *av_, *rw_, *ts_, *clog_, *rec_,
                                                   *guard_, *applier_, config_.nodeId(),
                                                   streamEpoch_, schemaFp, config_.schemaVersion());

    // Create InboxWatcher on this thread
    watcher_ = std::make_unique<InboxWatcher>(config_.inboxDir(), wconn_, *ledger_);
    connect(
        watcher_.get(), &InboxWatcher::artifactReady, this,
        [this](const QString& path) { pendingArtifacts_.append(path); }, Qt::DirectConnection);
    watcher_->start();

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
    }

    watcher_->stop();
    ackChan_->flush(*codec_);
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
    // Also check for newly-discovered artifacts from InboxWatcher
    QStringList toProcess;
    toProcess.swap(pendingArtifacts_);

    // Additionally poll ledger for any 'seen' artifacts not yet consumed
    QStringList seenPending = ledger_->pendingSeen(wconn_);
    for (const QString& name : seenPending) {
        QString fullPath = config_.inboxDir() + QDir::separator() + name;
        if (!toProcess.contains(fullPath))
            toProcess.append(fullPath);
    }

    for (const QString& path : toProcess) {
        processArtifact(path);
    }
}

bool SyncWorker::processArtifact(const QString& path) {
    QFileInfo fi(path);
    QString name = fi.fileName();

    // Skip .ack files — handled separately
    if (name.endsWith(QStringLiteral(".ack"))) {
        return processAckArtifact(path, name);
    }

    // Check ledger
    LedgerStatus st = ledger_->status(wconn_, name);
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
        ledger_->markCorrupt(wconn_, name, &markErr);
        emit errorOccurred(
            {err::E_SYNC_PAYLOAD_CORRUPT, Severity::Error, "inbox", dec.header.origin, decErr});
        return false;
    }

    ledger_->markSeen(wconn_, name, nullptr);

    bool ok = false;
    if (dec.kind == PayloadKind::Changeset)
        ok = processChangesetArtifact(dec, name);
    else if (dec.kind == PayloadKind::SelectionPush)
        ok = processSelectionPushArtifact(dec, name);

    if (ok) {
        ledger_->markConsumed(wconn_, name, nullptr);
        // Send ACK back
        ChangesetAck ack;
        ack.origin = dec.header.origin;
        ack.streamEpoch = dec.header.streamEpoch;
        ack.appliedSeq = dec.header.originSeq;
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
        quarantine_->quarantine(wconn_, hdr.origin, hdr.originSeq, hdr.streamEpoch, hdr.schemaVer,
                                dec.changeset, nullptr);
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

    // Update local origin seq if this is our own changeset echoed back
    if (hdr.origin == config_.nodeId() && hdr.originSeq > localOriginSeq_)
        localOriginSeq_ = hdr.originSeq;

    return true;
}

bool SyncWorker::processSelectionPushArtifact(const DecodeResult& dec, const QString& /*name*/) {
    const PayloadHeader& hdr = dec.header;
    const SelectionPushBody& body = dec.selection;

    // Build mutations from selection push body
    QList<RowMutation> mutations;
    for (int i = 0; i < body.rows.size(); ++i) {
        const QVariantMap& rowMap = body.rows[i];
        RowMutation m;
        m.table = (i < body.frozenEntries.size()) ? body.frozenEntries[i].table : QString();
        m.columns = rowMap.keys();
        m.values = rowMap.values();
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

bool SyncWorker::processAckArtifact(const QString& path, const QString& /*name*/) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QByteArray data = f.readAll();
    f.close();

    // Try changeset ACK first
    ChangesetAck csAck;
    if (codec_->decodeChangesetAck(data, &csAck)) {
        ackStore_->updateAcked(wconn_, csAck.origin, csAck.origin, csAck.streamEpoch,
                               csAck.appliedSeq, nullptr);
        return true;
    }
    // Try chunk ACK
    PushChunkAck chunkAck;
    if (codec_->decodeChunkAck(data, &chunkAck)) {
        // Record chunk ack — just log for now; push progress tracking is in
        // __sync_push_chunk_progress
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
    qint64 peerAckedSeq = computePeerAckedSeq(peer);

    // Read pending entries from changelog
    QList<ChangelogStore::Entry> entries = clog_->readRange(
        wconn_, peer, peerAckedSeq, static_cast<int>(config_.broadcastThreshold()));

    qint64 bytesSent = 0;
    for (const auto& entry : entries) {
        // Routing check
        if (!routing_->shouldRoute(peer, entry.origin, entry.originSeq, peerAckedSeq))
            continue;

        // Encode and write to outbox
        PayloadHeader hdr;
        hdr.origin = entry.origin;
        hdr.originSeq = entry.originSeq;
        hdr.streamEpoch = streamEpoch_;
        hdr.schemaVer = config_.schemaVersion();
        hdr.routeTag = peer;

        QByteArray payload = codec_->encodeChangeset(hdr, entry.changeset);
        QString artifactName = peer + QStringLiteral("_") + QString::number(entry.localSeq) +
                               QStringLiteral(".payload");
        QString writeErr;
        if (!outbox_->write(artifactName, payload, &writeErr)) {
            emit errorOccurred(
                {err::E_SYNC_TRANSPORT, Severity::Warning, "broadcast", peer, writeErr});
            break;
        }
        bytesSent += payload.size();
        if (bytesSent >= config_.outboxMaxBytesPerPeer())
            break;
    }
}

qint64 SyncWorker::computePeerAckedSeq(const QString& peer) {
    return ackStore_->minAckedSeq(wconn_, config_.nodeId(), streamEpoch_);
}

}  // namespace dbridge::sync
