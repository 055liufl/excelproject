#pragma once

#include <QAtomicInt>
#include <QHostAddress>
#include <QMap>
#include <QSet>
#include <QString>
#include <QThread>

class QUdpSocket;

// Background thread that implements a simple file-transfer transport over UDP.
//
// Watches outboxDir for new files, sends each file to the peer via UDP datagrams,
// and receives peer datagrams, reassembling them into inboxDir.  Designed for
// loopback (same-host) use so retransmission is omitted.
//
// Datagram wire format (all integers big-endian):
//   [magic:4u][msg_id:4u][frag_idx:2u][frag_count:2u][total_size:4u]
//   [fname_len:1u][fname:fname_len bytes][payload:remainder]
//
// After sending all fragments of a file, the outbox file is renamed to
// "<name>.sent" so it is not re-sent on the next poll iteration.
class UdpFileTransport : public QThread {
    Q_OBJECT

   public:
    UdpFileTransport(quint16 localPort, const QHostAddress& peerHost, quint16 peerPort,
                     const QString& outboxDir, const QString& inboxDir, QObject* parent = nullptr);

    // Thread-safe: sets a stop flag, returns immediately.  Call wait() afterwards.
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

    void pollOutbox(QUdpSocket& sock);
    void sendFile(QUdpSocket& sock, const QString& filePath, const QString& filename);
    void handleDatagram(const QByteArray& dg, QMap<quint32, PendingMsg>& pending);

    quint16 localPort_;
    QHostAddress peerHost_;
    quint16 peerPort_;
    QString outboxDir_;
    QString inboxDir_;
    QAtomicInt stop_;
    QSet<QString> sentFiles_;  // basenames sent this session (not re-sent)
};
