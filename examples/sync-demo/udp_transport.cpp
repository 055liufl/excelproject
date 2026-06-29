#include "udp_transport.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QUdpSocket>

UdpFileTransport::UdpFileTransport(quint16 localPort, const QHostAddress& peerHost,
                                   quint16 peerPort, const QString& outboxDir,
                                   const QString& inboxDir, QObject* parent)
    : QThread(parent),
      localPort_(localPort),
      peerHost_(peerHost),
      peerPort_(peerPort),
      outboxDir_(outboxDir),
      inboxDir_(inboxDir) {
}

void UdpFileTransport::requestStop() {
    stop_.storeRelease(1);
}

void UdpFileTransport::run() {
    QUdpSocket sock;
    if (!sock.bind(QHostAddress::LocalHost, localPort_)) {
        qWarning("UdpFileTransport[port=%u]: bind failed — %s", localPort_,
                 qPrintable(sock.errorString()));
        return;
    }

    QDir(inboxDir_).mkpath(QStringLiteral("."));

    QMap<quint32, PendingMsg> pending;

    while (!stop_.loadAcquire()) {
        // Wait up to kPollMs for an incoming datagram, then poll outbox regardless.
        if (sock.waitForReadyRead(kPollMs)) {
            while (sock.hasPendingDatagrams()) {
                QByteArray dg(static_cast<int>(sock.pendingDatagramSize()), Qt::Uninitialized);
                QHostAddress sender;
                quint16 senderPort = 0;
                sock.readDatagram(dg.data(), dg.size(), &sender, &senderPort);
                handleDatagram(dg, pending);
            }
        }
        pollOutbox(sock);
    }
}

// OutboxWriter protocol:
//   {name}.payload       — the actual artifact data
//   {name}.payload.ready — empty sentinel written after the payload is fully persisted
//
// InboxWatcher only looks for *.ready sentinels to decide a file has fully arrived.
// This transport mirrors the exact same convention on the receive side:
//   receive {name}.payload data → write to inbox/{name}.payload
//   then create inbox/{name}.payload.ready (empty)
void UdpFileTransport::pollOutbox(QUdpSocket& sock) {
    QDir dir(outboxDir_);
    if (!dir.exists())
        return;

    // Trigger on *.ready sentinels (written by OutboxWriter AFTER the payload is stable)
    const QStringList readyFiles =
        dir.entryList(QStringList{QStringLiteral("*.ready")}, QDir::Files, QDir::Name);

    for (const QString& readyName : readyFiles) {
        // Derive the payload basename by removing the trailing ".ready" (6 chars).
        // e.g. "foo.payload.ready" → "foo.payload"
        const QString payloadName = readyName.left(readyName.size() - 6);
        const QString payloadPath = dir.filePath(payloadName);
        const QString readyPath = dir.filePath(readyName);

        // Skip already-sent entries (keyed on the payload name)
        if (sentFiles_.contains(payloadName))
            continue;

        // Payload file must exist (the .ready can transiently appear before the rename completes)
        if (!QFile::exists(payloadPath))
            continue;

        sendFile(sock, payloadPath, payloadName);
        // Mark both files so we don't re-send on the next poll
        sentFiles_.insert(payloadName);
        QFile::rename(payloadPath, payloadPath + QLatin1String(".sent"));
        QFile::rename(readyPath, readyPath + QLatin1String(".sent"));
    }
}

void UdpFileTransport::sendFile(QUdpSocket& sock, const QString& filePath,
                                const QString& filename) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray data = f.readAll();
    f.close();

    const QByteArray fnUtf8 = filename.toUtf8();
    if (fnUtf8.size() > 255) {
        qWarning("UdpFileTransport: filename too long (%d bytes), skipping", fnUtf8.size());
        return;
    }

    const auto totalSize = static_cast<quint32>(data.size());
    const int fragCount =
        data.isEmpty() ? 1 : (static_cast<int>(data.size()) + kFragSize - 1) / kFragSize;
    const auto msgId = static_cast<quint32>(QRandomGenerator::global()->generate());

    for (int i = 0; i < fragCount; ++i) {
        const QByteArray frag = data.mid(i * kFragSize, kFragSize);

        QByteArray dg;
        {
            QDataStream ds(&dg, QIODevice::WriteOnly);
            ds.setByteOrder(QDataStream::BigEndian);
            ds << kMagic << msgId << static_cast<quint16>(i) << static_cast<quint16>(fragCount)
               << totalSize << static_cast<quint8>(fnUtf8.size());
        }
        dg.append(fnUtf8);
        dg.append(frag);

        const qint64 sent = sock.writeDatagram(dg, peerHost_, peerPort_);
        if (sent != dg.size()) {
            qWarning("UdpFileTransport[port=%u]: short write frag %d of %s", localPort_, i,
                     qPrintable(filename));
        }
    }
}

void UdpFileTransport::handleDatagram(const QByteArray& dg, QMap<quint32, PendingMsg>& pending) {
    // Minimum header: magic(4)+msg_id(4)+frag_idx(2)+frag_count(2)+total(4)+fnlen(1) = 17
    if (dg.size() < 17)
        return;

    QDataStream ds(dg);
    ds.setByteOrder(QDataStream::BigEndian);

    quint32 magic, msgId, totalSize;
    quint16 fragIdx, fragCount;
    quint8 fnLen;
    ds >> magic >> msgId >> fragIdx >> fragCount >> totalSize >> fnLen;

    if (magic != kMagic || ds.status() != QDataStream::Ok)
        return;

    constexpr int kFixedHeader = 17;
    if (dg.size() < kFixedHeader + static_cast<int>(fnLen))
        return;

    const QString filename =
        QString::fromUtf8(dg.constData() + kFixedHeader, static_cast<int>(fnLen));
    const QByteArray payload = dg.mid(kFixedHeader + static_cast<int>(fnLen));

    auto& msg = pending[msgId];
    if (msg.fragCount == 0) {
        msg.filename = filename;
        msg.totalSize = totalSize;
        msg.fragCount = fragCount;
    }
    msg.frags[fragIdx] = payload;

    if (!msg.complete())
        return;

    // All fragments received — write reassembled payload to inbox, then touch the .ready
    // sentinel so InboxWatcher recognises the file as fully arrived.
    const QByteArray fileData = msg.assemble();
    const QString payloadDest = QDir(inboxDir_).filePath(msg.filename);
    const QString readyDest = payloadDest + QLatin1String(".ready");

    QFile out(payloadDest);
    if (!out.open(QIODevice::WriteOnly)) {
        qWarning("UdpFileTransport[port=%u]: cannot write inbox file %s", localPort_,
                 qPrintable(payloadDest));
        pending.remove(msgId);
        return;
    }
    out.write(fileData);
    out.flush();
    out.close();

    // Create the .ready sentinel (empty file) so InboxWatcher picks it up.
    QFile ready(readyDest);
    ready.open(QIODevice::WriteOnly);
    ready.close();

    pending.remove(msgId);
}
