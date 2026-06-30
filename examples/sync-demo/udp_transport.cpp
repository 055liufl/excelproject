#include "udp_transport.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QUdpSocket>

// ── Constructors ──────────────────────────────────────────────────────────────

UdpFileTransport::UdpFileTransport(quint16 localPort, const QHostAddress& peerHost,
                                   quint16 peerPort, const QString& outboxDir,
                                   const QString& inboxDir, QObject* parent)
    : QThread(parent), localPort_(localPort), outboxDir_(outboxDir), inboxDir_(inboxDir) {
    // Single-peer mode: store as a 1-entry map with a sentinel key.
    // pollOutbox detects peers_.size()==1 and skips filename-based peer extraction.
    peers_.insert(QStringLiteral("__single__"), {peerHost, peerPort});
}

UdpFileTransport::UdpFileTransport(quint16 localPort, const QHash<QString, UdpPeerEndpoint>& peers,
                                   const QString& outboxDir, const QString& inboxDir,
                                   QObject* parent)
    : QThread(parent),
      localPort_(localPort),
      peers_(peers),
      outboxDir_(outboxDir),
      inboxDir_(inboxDir) {
}

void UdpFileTransport::requestStop() {
    stop_.storeRelease(1);
}

// ── Run loop ──────────────────────────────────────────────────────────────────

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

// ── Peer extraction from artifact filename ────────────────────────────────────
//
// Artifact naming conventions (SyncDDL.h):
//   changeset        : {origin}__{epoch}__changeset__{seq12}__{peer}-{uuid}.payload
//   selectionpush    : {origin}__{epoch}__selectionpush__{id.chunk}__peer-{peer}-{uuid}.payload
//   baselineresponse : {from}__{to}__{epoch}__{maxSeq12}__{uuid}__baselineresponse.payload → to
//   blreq            : blreq__{from}__{to}__{ms}.payload                                   → to
//   ack              : ack__{from}__{to}__{ms}__{uuid}.ack                                 → to

QString UdpFileTransport::extractTargetPeer(const QString& filename) {
    QString name = filename;
    if (name.endsWith(QStringLiteral(".payload")))
        name.chop(8);
    else if (name.endsWith(QStringLiteral(".ack")))
        name.chop(4);

    const QStringList parts = name.split(QStringLiteral("__"));
    if (parts.isEmpty())
        return {};

    // ack__{from}__{to}__{ms}__{uuid}
    if (parts.at(0) == QStringLiteral("ack") && parts.size() >= 3)
        return parts.at(2);

    // blreq__{from}__{to}__{ms}
    if (parts.at(0) == QStringLiteral("blreq") && parts.size() >= 3)
        return parts.at(2);

    // {from}__{to}__...__baselineresponse  or  __baselinerequest
    const QString last = parts.last();
    if ((last == QStringLiteral("baselineresponse") || last == QStringLiteral("baselinerequest")) &&
        parts.size() >= 2)
        return parts.at(1);

    // {origin}__{epoch}__changeset__{seq12}__{peer}-{uuid}
    if (parts.size() >= 5 && parts.at(2) == QStringLiteral("changeset")) {
        const QString seg = parts.at(4);
        const int dash = seg.lastIndexOf(QLatin1Char('-'));
        if (dash > 0)
            return seg.left(dash);
    }

    // {origin}__{epoch}__selectionpush__{id.chunk}__peer-{peer}-{uuid}
    if (parts.size() >= 5 && parts.at(2) == QStringLiteral("selectionpush")) {
        const QString seg = parts.at(4);
        const QString prefix = QStringLiteral("peer-");
        if (seg.startsWith(prefix)) {
            const QString rest = seg.mid(prefix.size());
            const int dash = rest.lastIndexOf(QLatin1Char('-'));
            if (dash > 0)
                return rest.left(dash);
        }
    }

    return {};
}

// ── Outbox polling ────────────────────────────────────────────────────────────
//
// OutboxWriter protocol:
//   {name}.payload       — artifact data (stable before .ready appears)
//   {name}.payload.ready — empty sentinel: worker may now read the payload
//   Same for .ack / .ack.ready
//
// InboxWatcher only triggers on *.ready sentinels.  This transport mirrors the
// same convention on the receive side.

void UdpFileTransport::pollOutbox(QUdpSocket& sock) {
    QDir dir(outboxDir_);
    if (!dir.exists())
        return;

    const QStringList readyFiles =
        dir.entryList(QStringList{QStringLiteral("*.ready")}, QDir::Files, QDir::Name);

    for (const QString& readyName : readyFiles) {
        const QString payloadName = readyName.left(readyName.size() - 6);  // strip ".ready"
        const QString payloadPath = dir.filePath(payloadName);
        const QString readyPath = dir.filePath(readyName);

        if (sentFiles_.contains(payloadName))
            continue;
        if (!QFile::exists(payloadPath))
            continue;

        // Determine destination endpoint.
        QHostAddress destHost;
        quint16 destPort = 0;

        if (peers_.size() == 1) {
            // Single-peer mode: always send to the only endpoint.
            const auto ep = peers_.constBegin().value();
            destHost = ep.host;
            destPort = ep.port;
        } else {
            // Multi-peer mode: derive target peer from artifact filename.
            const QString peer = extractTargetPeer(payloadName);
            if (peer.isEmpty() || !peers_.contains(peer))
                continue;  // unknown or unregistered target — skip
            destHost = peers_[peer].host;
            destPort = peers_[peer].port;
        }

        sendFile(sock, payloadPath, payloadName, destHost, destPort);
        sentFiles_.insert(payloadName);
        QFile::rename(payloadPath, payloadPath + QLatin1String(".sent"));
        QFile::rename(readyPath, readyPath + QLatin1String(".sent"));
    }
}

// ── Send a single file as UDP fragments ──────────────────────────────────────

void UdpFileTransport::sendFile(QUdpSocket& sock, const QString& filePath, const QString& filename,
                                const QHostAddress& destHost, quint16 destPort) {
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

        const qint64 sent = sock.writeDatagram(dg, destHost, destPort);
        if (sent != dg.size()) {
            qWarning("UdpFileTransport[port=%u]: short write frag %d of %s", localPort_, i,
                     qPrintable(filename));
        }
    }
}

// ── Datagram reassembly ───────────────────────────────────────────────────────

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

    // All fragments received — write to inbox, then create .ready sentinel.
    const QByteArray fileData = msg.assemble();
    const QString destPath = QDir(inboxDir_).filePath(msg.filename);
    const QString readyPath = destPath + QLatin1String(".ready");

    QFile out(destPath);
    if (!out.open(QIODevice::WriteOnly)) {
        qWarning("UdpFileTransport[port=%u]: cannot write inbox file %s", localPort_,
                 qPrintable(destPath));
        pending.remove(msgId);
        return;
    }
    out.write(fileData);
    out.flush();
    out.close();

    QFile ready(readyPath);
    ready.open(QIODevice::WriteOnly);
    ready.close();

    pending.remove(msgId);
}
