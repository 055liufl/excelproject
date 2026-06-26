#pragma once
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

namespace dbridge::sync {

enum class NodeRole { Center, Edge };

enum class SyncState {
    Idle,
    Capturing,
    Exporting,
    Importing,
    Broadcasting,
    Completed,
    Stopped,
    Failed
};

enum class ConflictPolicy { SourceWins, TargetWins, Manual };

enum class Severity { Info, Warning, Error, Fatal };

struct SyncProgress {
    SyncState state = SyncState::Idle;
    int percent = -1;
    qint64 bytesPacked = 0;
    qint64 bytesApplied = 0;
    int changesApplied = 0;
    int conflicts = 0;
};

struct PeerSyncState {
    QString nodeId;
    qint64 lastAckedSeq = -1;
    qint64 lastSentSeq = -1;
    bool lagging = false;
    bool evicted = false;
};

struct SyncLogEntry {
    qint64 epochMs = 0;
    Severity severity = Severity::Info;
    QString phase;
    QString message;
};

struct SyncError {
    QString code;
    Severity severity = Severity::Error;
    QString phase;
    QString nodeId;
    QString message;
};

struct SyncResult {
    bool ok = false;
    SyncState finalState = SyncState::Idle;
    int payloadsSent = 0;
    int payloadsApplied = 0;
    int changesApplied = 0;
    int conflicts = 0;
    QList<PeerSyncState> peers;
    QList<SyncError> errors;
};

// Payload header carried in every artifact.
struct PayloadHeader {
    QString origin;
    qint64 originSeq = 0;
    qint64 parentSeq = 0;
    QString schemaFingerprint;
    qint64 schemaVer = 0;
    qint64 streamEpoch = 0;
    QString routeTag;
    QString pushId;  // non-empty for SelectionPush
    int chunkSeq = 0;
    int totalChunks = 0;
    // C-05 fix: physical sender of this artifact (the node that wrote the file to its outbox).
    // May differ from 'origin' when a center node forwards a remote-origin changeset.
    // Receivers use senderPeer as ACK.toPeer so the center's outbound_ack watermark advances.
    QString senderPeer;
};

enum class PayloadKind { Changeset, SelectionPush, BaselineRequest, BaselineResponse };

struct FrozenEntry {
    QString table;
    QString primaryKey;
    QString pkHash;
    QString recordKind;  // "selected" | "dependency"
    int topoIndex = 0;
    QByteArray fingerprint;
};

struct SelectionPushBody {
    QList<FrozenEntry> frozenEntries;
    // rows[i] matches frozenEntries[i]
    QList<QVariantMap> rows;
    QString pushId;
    int chunkSeq = 0;
    int totalChunks = 0;
};

struct BaselineRequestPayload {
    QString origin;
    qint64 streamEpoch = 0;
    QStringList requestedTables;
    qint64 fromSeq = 0;
    QString pendingArtifactName;
};

// C-03 fix: per-origin applied-vector cut exported in a baseline response.
// Carries the stream_epoch so the receiver can call av.resetTo(origin, epoch, seq, generation)
// with the correct epoch rather than guessing from the local streamEpoch_.
struct BaselineOriginCut {
    QString origin;
    qint64 streamEpoch = 0;
    qint64 appliedSeq = 0;
};

struct BaselineResponsePayload {
    QString origin;
    QString requestOrigin;
    qint64 streamEpoch = 0;
    QStringList tables;
    qint64 fromSeq = 0;
    QString pendingArtifactName;
    QByteArray baselineData;
    qint64 sourceMaxSeq = 0;
    // C-03 fix: per-origin applied-vector snapshot at baseline export time (includes epoch).
    // Replaces the previous QHash<QString,qint64> which lacked stream_epoch.
    QVector<BaselineOriginCut> originCuts;
};

struct DecodeResult {
    PayloadHeader header;
    PayloadKind kind = PayloadKind::Changeset;
    QByteArray changeset;
    // C-5 fix: preserve the complete encoded payload so quarantine can replay via codec->decode().
    QByteArray rawPayload;
    SelectionPushBody selection;
    BaselineRequestPayload baselineRequest;
    BaselineResponsePayload baselineResponse;
};

// Typed ACK artifacts
struct ChangesetAck {
    QString origin;
    qint64 streamEpoch = 0;
    qint64 appliedSeq = 0;
    QString toPeer;  // target peer this ACK is addressed to (J-01 fix)
};

struct PushChunkAck {
    QString pushId;
    int chunkSeq = 0;
    int totalChunks = 0;
    QString checksum;
    bool ok = false;
    QString toPeer;  // H-04 fix: peer to which this ACK is routed (= original push origin)
};

// Tracks which peers and (origin, epoch, target_seq) tuples must all be ACKed
// before a foreground sync() call is considered complete.
struct PendingAckEntry {
    QString peer;
    QString origin;
    qint64 epoch = 0;
    qint64 targetSeq = 0;
};

enum class UpsertMode { DoUpdate, DoNothing };

struct RowMutation {
    QString table;
    QStringList columns;
    QVariantList values;
    QStringList pkColumns;
    UpsertMode mode = UpsertMode::DoUpdate;
    QVariantMap originMeta;
};

}  // namespace dbridge::sync
