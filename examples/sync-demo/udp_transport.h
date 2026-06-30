#pragma once

#include <QAtomicInt>
#include <QHash>
#include <QHostAddress>
#include <QMap>
#include <QSet>
#include <QString>
#include <QThread>

class QUdpSocket;

// Per-peer UDP endpoint (host + port).
struct UdpPeerEndpoint {
    QHostAddress host;
    quint16 port = 0;
};

// Background thread: file-transfer transport over UDP.
//
// Watches outboxDir for new files, determines the target peer from the artifact
// filename, sends the file to the corresponding endpoint, and simultaneously
// listens on localPort, reassembling incoming datagrams into inboxDir.
// Designed for loopback use; no retransmission.
//
// Multi-peer mode (center node): pass a QHash<nodeId, endpoint> map.
//   The target peer is extracted from each artifact's filename:
//     changeset  : {origin}__{epoch}__changeset__{seq12}__{peer}-{uuid}.payload
//     selectionpush: ...selectionpush...__peer-{peer}-{uuid}.payload
//     baselineresponse: {from}__{to}__{epoch}__...__baselineresponse.payload  → to
//     blreq      : blreq__{from}__{to}__{ms}.payload  → to
//     ack        : ack__{from}__{to}__{ms}__{uuid}.ack → to
//
// Single-peer mode (edge node): use the two-peerHost/peerPort constructor.
//   All outbox files are sent to that single peer without filename parsing.
//
// Datagram wire format (all integers big-endian):
//   [magic:4u][msg_id:4u][frag_idx:2u][frag_count:2u][total_size:4u]
//   [fname_len:1u][fname:fname_len bytes][payload:remainder]
//
// After sending all fragments of a file, the outbox file is renamed to
// "<name>.sent" so it is not re-sent on the next poll.
class UdpFileTransport : public QThread {
    Q_OBJECT

   public:
    // Single-peer constructor (edge nodes: one peer = center).
    UdpFileTransport(quint16 localPort, const QHostAddress& peerHost, quint16 peerPort,
                     const QString& outboxDir, const QString& inboxDir, QObject* parent = nullptr);

    // Multi-peer constructor (center node: sends to multiple edge endpoints).
    // Key = peer nodeId (e.g. "edge_b"), Value = UDP endpoint.
    UdpFileTransport(quint16 localPort, const QHash<QString, UdpPeerEndpoint>& peers,
                     const QString& outboxDir, const QString& inboxDir, QObject* parent = nullptr);

    // Thread-safe stop: sets flag and returns immediately. Call wait() afterwards.
    void requestStop();

   protected:
    void run() override;

   private:
    static constexpr quint32 kMagic = 0xDB5ACED0u;
    static constexpr int kFragSize = 60'000;  // fragment payload bytes
    static constexpr int kPollMs = 50;        // outbox poll / receive-wait interval

    struct PendingMsg {
        QString filename;
        quint32 totalSize = 0;
        quint16 fragCount = 0;
        QMap<quint16, QByteArray> frags;

        bool complete() const {
            return frags.size() == static_cast<int>(fragCount);
        }
        QByteArray assemble() const {
            QByteArray data;
            data.reserve(static_cast<int>(totalSize));
            for (quint16 i = 0; i < fragCount; ++i)
                data += frags.value(i);
            return data;
        }
    };

    // Extract the target peer nodeId from an artifact filename.
    // Returns an empty string if the peer cannot be determined.
    static QString extractTargetPeer(const QString& filename);

    void pollOutbox(QUdpSocket& sock);
    void sendFile(QUdpSocket& sock, const QString& filePath, const QString& filename,
                  const QHostAddress& destHost, quint16 destPort);
    void handleDatagram(const QByteArray& dg, QMap<quint32, PendingMsg>& pending);

    quint16 localPort_;
    QHash<QString, UdpPeerEndpoint> peers_;  // nodeId → endpoint (1 entry = single-peer)
    QString outboxDir_;
    QString inboxDir_;
    QAtomicInt stop_;
    QSet<QString> sentFiles_;  // basenames sent this session
};
