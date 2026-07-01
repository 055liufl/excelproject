// ============================================================================
// udp_transport.cpp — 可靠 UDP 文件搬运传输层（ARQ 版）实现
// ============================================================================
// 实现三件事：
//   ① fragmentMessage()    — 纯函数，文件 → N 个 ≤ maxBytes 的 DATA 数据报
//   ② FragmentReassembler  — 有状态机，入站校验 / 重组 / 去重 / 超时淘汰
//   ③ UdpFileTransport     — 后台线程，ARQ 发送 + 交付状态机 + run() 编排
// ============================================================================
#include "udp_transport.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QUdpSocket>

// ── ① fragmentMessage ────────────────────────────────────────────────────────

// DATA 固定头
// 18B：magic(4)|type=0x01(1)|msg_id(4)|frag_idx(2)|frag_count(2)|total_size(4)|fname_len(1)
// 每片净荷 M = maxTransmitBytes − 18 − fname_len。
// 守卫（任一不过 → ok=false）：fname UTF-8 > 255 / M < 1 / fragCount > 65535。
// 空 data → 1 片空净荷（确保空文件也被传输并触发接收端落 inbox）。
FragmentResult fragmentMessage(quint32 msgId, const QString& filename, const QByteArray& data,
                               int maxTransmitBytes) {
    const QByteArray fnUtf8 = filename.toUtf8();
    if (fnUtf8.size() > 255)
        return {{}, false, QStringLiteral("filename UTF-8 > 255 bytes")};

    const auto fnLen = static_cast<quint8>(fnUtf8.size());
    const int M = maxTransmitBytes - 18 - static_cast<int>(fnLen);
    if (M < 1)
        return {{}, false, QStringLiteral("maxTransmitBytes too small: no room for payload")};

    // Qt5 QByteArray::size() 为 int，不可能超过 INT_MAX < UINT32_MAX，此防御分支不可达但保留语义

    const auto totalSize = static_cast<quint32>(data.size());
    const qint64 fragCount64 = data.isEmpty() ? 1 : (static_cast<qint64>(data.size()) + M - 1) / M;
    if (fragCount64 > 65535)
        return {{}, false, QStringLiteral("fragCount > 65535")};

    const auto fragCount = static_cast<quint16>(fragCount64);
    QList<QByteArray> datagrams;
    datagrams.reserve(static_cast<int>(fragCount));

    for (quint16 i = 0; i < fragCount; ++i) {
        const QByteArray payload = data.isEmpty() ? QByteArray() : data.mid(i * M, M);
        QByteArray dg;
        dg.reserve(18 + static_cast<int>(fnLen) + payload.size());
        {
            QDataStream ds(&dg, QIODevice::WriteOnly);
            ds.setByteOrder(QDataStream::BigEndian);
            ds << static_cast<quint32>(0xDB5ACED0u) << static_cast<quint8>(0x01u) << msgId << i
               << fragCount << totalSize << fnLen;
        }
        dg.append(fnUtf8);
        dg.append(payload);
        datagrams.append(dg);
    }

    return {datagrams, true, {}};
}

// ── ② FragmentReassembler ────────────────────────────────────────────────────

FragmentReassembler::FragmentReassembler(int reassemblyTimeoutMs, int completedRetentionMs)
    : reassemblyTimeoutMs_(reassemblyTimeoutMs), completedRetentionMs_(completedRetentionMs) {
}

// 文件名安全校验（防路径穿越）：非空、无 '/'/'\'、不是绝对路径、不是 ".."。
static bool isFilenameSafe(const QString& name) {
    if (name.isEmpty())
        return false;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;
    if (QDir::isAbsolutePath(name))
        return false;
    if (name == QStringLiteral(".."))
        return false;
    return true;
}

RxEvent FragmentReassembler::feed(const QString& senderKey, const QByteArray& datagram,
                                  qint64 nowMs) {
    // §4.4 校验 1：最小长度 18 + magic + type=0x01
    if (datagram.size() < 18)
        return {};

    QDataStream ds(datagram);
    ds.setByteOrder(QDataStream::BigEndian);
    quint32 magic;
    quint8 type;
    quint32 msgId;
    quint16 fragIdx, fragCount;
    quint32 totalSize;
    quint8 fnLen;
    ds >> magic >> type >> msgId >> fragIdx >> fragCount >> totalSize >> fnLen;
    if (ds.status() != QDataStream::Ok)
        return {};
    if (magic != 0xDB5ACED0u || type != 0x01u)
        return {};

    // §4.4 校验 2：fragCount > 0 且 fragIdx < fragCount（拒绝越界片）
    if (fragCount == 0 || fragIdx >= fragCount)
        return {};

    // §4.4 校验 3：文件名长度不超报文
    if (datagram.size() < 18 + static_cast<int>(fnLen))
        return {};

    // §4.4 校验 4：文件名安全（防路径穿越）
    const QString filename = QString::fromUtf8(datagram.constData() + 18, static_cast<int>(fnLen));
    if (!isFilenameSafe(filename))
        return {};

    const MsgKey key(senderKey, msgId);

    // 已完成短表命中 → 重复消息（ACK 丢失导致的重发）→ 只回 ACK，不落盘
    if (completed_.contains(key)) {
        RxEvent ev;
        ev.kind = RxEvent::NeedAck;
        ev.msgId = msgId;
        return ev;
    }

    // 新消息 → 建 pending；已存在 → 校验元数据一致性（§4.4 校验 5）
    if (!pending_.contains(key)) {
        PendingMsg pm;
        pm.filename = filename;
        pm.totalSize = totalSize;
        pm.fragCount = fragCount;
        pm.fnLen = fnLen;
        pm.createdAt = nowMs;
        pm.lastProgressAt = nowMs;
        pending_.insert(key, pm);
    } else {
        const PendingMsg& pm = pending_[key];
        if (pm.fragCount != fragCount || pm.totalSize != totalSize || pm.fnLen != fnLen ||
            pm.filename != filename)
            return {};  // 元数据不一致 → 丢弃该片
    }

    PendingMsg& pm = pending_[key];
    const QByteArray payload = datagram.mid(18 + static_cast<int>(fnLen));

    // 存片：仅新片刷新 lastProgressAt（重复片不刷新进度）
    if (!pm.frags.contains(fragIdx)) {
        pm.frags.insert(fragIdx, payload);
        pm.lastProgressAt = nowMs;
    }

    if (!pm.complete())
        return {};

    // 组齐 → assemble + 总长校验（§4.4 校验 6）
    QByteArray assembled;
    assembled.reserve(static_cast<int>(pm.totalSize));
    for (quint16 i = 0; i < pm.fragCount; ++i)
        assembled += pm.frags.value(i);

    if (static_cast<quint32>(assembled.size()) != pm.totalSize) {
        qWarning(
            "FragmentReassembler[%s]: assembled size %d != totalSize %u, msgId=%u — discarding",
            qPrintable(senderKey), assembled.size(), pm.totalSize, msgId);
        pending_.remove(key);
        return {};
    }

    const QString fn = pm.filename;
    pending_.remove(key);

    RxEvent ev;
    ev.kind = RxEvent::Completed;
    ev.msgId = msgId;
    ev.filename = fn;
    ev.bytes = assembled;
    return ev;
}

void FragmentReassembler::markDelivered(const QString& senderKey, quint32 msgId, qint64 nowMs) {
    CompletedEntry entry;
    entry.completedAt = nowMs;
    completed_.insert(MsgKey(senderKey, msgId), entry);
}

void FragmentReassembler::evictStale(qint64 nowMs) {
    {
        QList<MsgKey> toRemove;
        for (auto it = pending_.constBegin(); it != pending_.constEnd(); ++it) {
            if (nowMs - it.value().lastProgressAt > reassemblyTimeoutMs_)
                toRemove.append(it.key());
        }
        for (const MsgKey& k : toRemove)
            pending_.remove(k);
    }
    {
        QList<MsgKey> toRemove;
        for (auto it = completed_.constBegin(); it != completed_.constEnd(); ++it) {
            if (nowMs - it.value().completedAt > completedRetentionMs_)
                toRemove.append(it.key());
        }
        for (const MsgKey& k : toRemove)
            completed_.remove(k);
    }
}

int FragmentReassembler::pendingCount() const {
    return pending_.size();
}

// ── ③ UdpFileTransport ───────────────────────────────────────────────────────

UdpFileTransport::UdpFileTransport(quint16 localPort, const QHostAddress& peerHost,
                                   quint16 peerPort, const QString& outboxDir,
                                   const QString& inboxDir, QObject* parent)
    : QThread(parent), localPort_(localPort), outboxDir_(outboxDir), inboxDir_(inboxDir) {
    peers_.insert(QStringLiteral("__single__"), {peerHost, peerPort});
    msgSeq_ = QRandomGenerator::global()->generate();
}

UdpFileTransport::UdpFileTransport(quint16 localPort, const QHash<QString, UdpPeerEndpoint>& peers,
                                   const QString& outboxDir, const QString& inboxDir,
                                   QObject* parent)
    : QThread(parent),
      localPort_(localPort),
      peers_(peers),
      outboxDir_(outboxDir),
      inboxDir_(inboxDir) {
    msgSeq_ = QRandomGenerator::global()->generate();
}

bool UdpFileTransport::setMaxTransmitBytes(int bytes) {
    if (bytes < kMinMaxBytes)
        return false;
    maxTransmitBytes_ = qMin(bytes, 65507);
    return true;
}

void UdpFileTransport::requestStop() {
    stop_.storeRelease(1);
}

// ── 从工件文件名解析目标 peer（保持不变）─────────────────────────────────────

QString UdpFileTransport::extractTargetPeer(const QString& filename) {
    QString name = filename;
    // 去掉可能存在的 .sending 后缀再去扩展名
    if (name.endsWith(QStringLiteral(".sending")))
        name.chop(8);
    if (name.endsWith(QStringLiteral(".payload")))
        name.chop(8);
    else if (name.endsWith(QStringLiteral(".ack")))
        name.chop(4);

    const QStringList parts = name.split(QStringLiteral("__"));
    if (parts.isEmpty())
        return {};

    if (parts.at(0) == QStringLiteral("ack") && parts.size() >= 3)
        return parts.at(2);

    if (parts.at(0) == QStringLiteral("blreq") && parts.size() >= 3)
        return parts.at(2);

    const QString last = parts.last();
    if ((last == QStringLiteral("baselineresponse") || last == QStringLiteral("baselinerequest")) &&
        parts.size() >= 2)
        return parts.at(1);

    if (parts.size() >= 5 && parts.at(2) == QStringLiteral("changeset")) {
        const QString seg = parts.at(4);
        const int dash = seg.lastIndexOf(QLatin1Char('-'));
        if (dash > 0)
            return seg.left(dash);
    }

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

// ── sendAck ──────────────────────────────────────────────────────────────────

void UdpFileTransport::sendAck(QUdpSocket& sock, quint32 msgId, const QHostAddress& dest,
                               quint16 destPort) {
    QByteArray ack;
    ack.reserve(kAckSize);
    {
        QDataStream ds(&ack, QIODevice::WriteOnly);
        ds.setByteOrder(QDataStream::BigEndian);
        ds << kMagic << kTypeAck << msgId;
    }
    sock.writeDatagram(ack, dest, destPort);
}

// ── pollOutbox ───────────────────────────────────────────────────────────────
//
// 出站状态机（§5.2）：
//   *.ready → ① 原子认领(.ready→.ready.sending) → ② 主文件→.sending → ③ 读取
//           → ④ 拆包 → ⑤ 发送 → 入 outbound
//   任一 ②③④ 失败 → 两文件改 .failed + 告警，不进 outbound
//
// 「主文件」= readyName 去掉 ".ready"，可能是 .payload 或 .ack，统一处理。

void UdpFileTransport::pollOutbox(QUdpSocket& sock, QHash<quint32, Outbound>& outbound,
                                  FragmentReassembler& /*reasm*/, qint64 nowMs) {
    QDir dir(outboxDir_);
    if (!dir.exists())
        return;

    // 只列以 ".ready" 结尾的文件；".ready.sending" 以 ".sending" 结尾，不会被命中
    const QStringList readyFiles =
        dir.entryList(QStringList{QStringLiteral("*.ready")}, QDir::Files, QDir::Name);

    for (const QString& readyName : readyFiles) {
        const QString artifactName = readyName.left(readyName.size() - 6);  // 去 ".ready"
        const QString artifactPath = dir.filePath(artifactName);
        const QString readyPath = dir.filePath(readyName);

        if (!QFile::exists(artifactPath))
            continue;

        // ① 原子认领：.ready → .ready.sending；失败 = 被并发认领 → 跳过
        const QString readySendingPath = readyPath + QLatin1String(".sending");
        if (!QFile::rename(readyPath, readySendingPath))
            continue;

        // ② 主文件 → .sending（发送前先落定终态对象）
        const QString artifactSendingPath = artifactPath + QLatin1String(".sending");
        if (!QFile::rename(artifactPath, artifactSendingPath)) {
            qWarning("UdpFileTransport[port=%u]: rename to .sending failed: %s", localPort_,
                     qPrintable(artifactPath));
            // 罕见 FS 错误 → .failed
            QFile::rename(readySendingPath, readySendingPath.left(readySendingPath.size() - 8) +
                                                QLatin1String(".failed"));
            continue;
        }

        // ③ 读取主文件
        QByteArray fileData;
        bool readOk = false;
        {
            QFile f(artifactSendingPath);
            if (f.open(QIODevice::ReadOnly)) {
                fileData = f.readAll();
                readOk = (f.error() == QFile::NoError);
                f.close();
            }
        }
        if (!readOk) {
            qWarning("UdpFileTransport[port=%u]: cannot read %s", localPort_,
                     qPrintable(artifactSendingPath));
            QFile::rename(artifactSendingPath, artifactPath + QLatin1String(".failed"));
            QFile::rename(readySendingPath, readySendingPath.left(readySendingPath.size() - 8) +
                                                QLatin1String(".failed"));
            continue;
        }

        // 路由决策
        QHostAddress destHost;
        quint16 destPort = 0;
        if (peers_.size() == 1) {
            const auto ep = peers_.constBegin().value();
            destHost = ep.host;
            destPort = ep.port;
        } else {
            const QString peer = extractTargetPeer(artifactName);
            if (peer.isEmpty() || !peers_.contains(peer)) {
                qWarning("UdpFileTransport[port=%u]: cannot route %s", localPort_,
                         qPrintable(artifactName));
                QFile::rename(artifactSendingPath, artifactPath + QLatin1String(".failed"));
                QFile::rename(readySendingPath, readySendingPath.left(readySendingPath.size() - 8) +
                                                    QLatin1String(".failed"));
                continue;
            }
            destHost = peers_[peer].host;
            destPort = peers_[peer].port;
        }

        // ④ 拆包
        ++msgSeq_;
        const quint32 msgId = msgSeq_;
        FragmentResult fr = fragmentMessage(msgId, artifactName, fileData, maxTransmitBytes_);
        if (!fr.ok) {
            qWarning("UdpFileTransport[port=%u]: fragmentMessage failed for %s: %s", localPort_,
                     qPrintable(artifactName), qPrintable(fr.error));
            QFile::rename(artifactSendingPath, artifactPath + QLatin1String(".failed"));
            QFile::rename(readySendingPath, readySendingPath.left(readySendingPath.size() - 8) +
                                                QLatin1String(".failed"));
            continue;
        }

        // ⑤ 发送所有分片
        for (const QByteArray& dg : fr.datagrams)
            sock.writeDatagram(dg, destHost, destPort);

        // 观测日志
        int maxDgSize = 0;
        for (const QByteArray& dg : fr.datagrams)
            maxDgSize = qMax(maxDgSize, dg.size());
        qDebug("UdpFileTransport[port=%u]: SEND msgId=%u artifact=%s fragCount=%d maxDg=%d",
               localPort_, msgId, qPrintable(artifactName), fr.datagrams.size(), maxDgSize);

        // 记入 outbound
        Outbound ob;
        ob.destHost = destHost;
        ob.destPort = destPort;
        ob.msgId = msgId;
        ob.datagrams = fr.datagrams;
        ob.artifactBase = dir.filePath(artifactName);
        ob.readySendingPath = readySendingPath;
        ob.lastSentAt = nowMs;
        ob.retries = 0;
        ob.fragCount = fr.datagrams.size();
        outbound.insert(msgId, ob);
    }
}

// ── run() ────────────────────────────────────────────────────────────────────
//
// 主循环（单线程，收发交替，§5.1④）：
//   while not stop:
//     收（waitForReadyRead + 逐报文 DATA/ACK 分派）
//     pollOutbox（新工件原子认领 → 拆包 → 发送 → 入 outbound）
//     RTO 兜底（全量重发 or giveUp → .failed）
//     evictStale（淘汰超时 pending + 过期短表）

void UdpFileTransport::run() {
    QUdpSocket sock;
    if (!sock.bind(QHostAddress::LocalHost, localPort_)) {
        qWarning("UdpFileTransport[port=%u]: bind failed — %s", localPort_,
                 qPrintable(sock.errorString()));
        return;
    }

    QDir(inboxDir_).mkpath(QStringLiteral("."));

    QElapsedTimer clk;
    clk.start();

    FragmentReassembler reasm(kReassemblyTimeoutMs, kCompletedRetentionMs);
    QHash<quint32, Outbound> outbound;

    while (!stop_.loadAcquire()) {
        const qint64 now = clk.elapsed();

        // ── 收 ────────────────────────────────────────────────────────────────
        if (sock.waitForReadyRead(kPollMs)) {
            while (sock.hasPendingDatagrams()) {
                QByteArray dg(static_cast<int>(sock.pendingDatagramSize()), Qt::Uninitialized);
                QHostAddress sender;
                quint16 senderPort = 0;
                sock.readDatagram(dg.data(), dg.size(), &sender, &senderPort);

                // 最短 5 字节：magic(4) + type(1)
                if (dg.size() < 5)
                    continue;

                // 快速读 magic + type（大端，手动解析避免 QDataStream 开销）
                const auto* raw = reinterpret_cast<const unsigned char*>(dg.constData());
                const quint32 magic =
                    (static_cast<quint32>(raw[0]) << 24) | (static_cast<quint32>(raw[1]) << 16) |
                    (static_cast<quint32>(raw[2]) << 8) | static_cast<quint32>(raw[3]);
                if (magic != kMagic)
                    continue;
                const quint8 pktType = raw[4];

                if (pktType == kTypeData) {
                    // DATA → 喂给组包器
                    const QString senderKey =
                        sender.toString() + QLatin1Char(':') + QString::number(senderPort);
                    RxEvent ev = reasm.feed(senderKey, dg, now);

                    if (ev.kind == RxEvent::Completed) {
                        // durable 落盘：先写主文件，再写 .ready
                        const QString destPath = QDir(inboxDir_).filePath(ev.filename);
                        const QString readyPath = destPath + QLatin1String(".ready");

                        bool writeOk = false;
                        {
                            QFile out(destPath);
                            if (out.open(QIODevice::WriteOnly)) {
                                out.write(ev.bytes);
                                out.flush();
                                out.close();
                                writeOk = (out.error() == QFile::NoError);
                            }
                        }
                        bool readyOk = false;
                        if (writeOk) {
                            QFile ready(readyPath);
                            readyOk = ready.open(QIODevice::WriteOnly);
                            if (readyOk)
                                ready.close();
                        }

                        if (writeOk && readyOk) {
                            // 两步都成功 → markDelivered + 回 ACK
                            reasm.markDelivered(senderKey, ev.msgId, now);
                            sendAck(sock, ev.msgId, sender, senderPort);
                        } else {
                            qWarning(
                                "UdpFileTransport[port=%u]: inbox write failed for %s "
                                "(writeOk=%d readyOk=%d) — not ACKing, sender will retransmit",
                                localPort_, qPrintable(ev.filename), static_cast<int>(writeOk),
                                static_cast<int>(readyOk));
                            // 不回 ACK；发送端 RTO 重传后会重新组齐并重试落盘
                        }
                    } else if (ev.kind == RxEvent::NeedAck) {
                        // 已完成短表命中（ACK 丢失导致的重发）→ 只回 ACK
                        sendAck(sock, ev.msgId, sender, senderPort);
                    }

                } else if (pktType == kTypeAck) {
                    // ACK 校验：精确 9 字节（§4.2）
                    if (dg.size() != kAckSize)
                        continue;
                    // 手动读 msgId（bytes 5-8，大端）
                    const quint32 ackMsgId = (static_cast<quint32>(raw[5]) << 24) |
                                             (static_cast<quint32>(raw[6]) << 16) |
                                             (static_cast<quint32>(raw[7]) << 8) |
                                             static_cast<quint32>(raw[8]);
                    if (!outbound.contains(ackMsgId))
                        continue;
                    const Outbound& ob = outbound[ackMsgId];
                    // 来源端点必须与 outbound 记录的目标匹配（防误包/旧包）
                    if (ob.destHost != sender || ob.destPort != senderPort)
                        continue;

                    qDebug("UdpFileTransport[port=%u]: ACK msgId=%u artifact=%s retries=%d",
                           localPort_, ackMsgId, qPrintable(ob.artifactBase), ob.retries);

                    // 主文件 .sending → .sent；.ready.sending → .ready.sent
                    QFile::rename(ob.artifactBase + QLatin1String(".sending"),
                                  ob.artifactBase + QLatin1String(".sent"));
                    // readySendingPath 末尾是 ".sending"（8 chars），替换为 ".sent"
                    QFile::rename(ob.readySendingPath,
                                  ob.readySendingPath.left(ob.readySendingPath.size() - 8) +
                                      QLatin1String(".sent"));
                    outbound.remove(ackMsgId);
                }
            }
        }

        // ── 发（新工件认领） ──────────────────────────────────────────────────
        pollOutbox(sock, outbound, reasm, now);

        // ── RTO 兜底（全量重传 / giveUp）──────────────────────────────────────
        // 收集需处理的 msgId（避免在遍历中修改 outbound）
        QList<quint32> rtoKeys;
        for (auto it = outbound.constBegin(); it != outbound.constEnd(); ++it) {
            if (now - it.value().lastSentAt > kRtoMs)
                rtoKeys.append(it.key());
        }
        for (quint32 mid : rtoKeys) {
            if (!outbound.contains(mid))
                continue;
            Outbound& ob = outbound[mid];
            if (ob.retries < kMaxRetries) {
                // 全量重发（从内存 datagrams，不读盘）
                for (const QByteArray& dg : ob.datagrams)
                    sock.writeDatagram(dg, ob.destHost, ob.destPort);
                ob.retries++;
                ob.lastSentAt = now;
                qDebug("UdpFileTransport[port=%u]: RTO retransmit msgId=%u artifact=%s retries=%d",
                       localPort_, mid, qPrintable(ob.artifactBase), ob.retries);
            } else {
                // giveUp → .failed + 告警
                qWarning(
                    "UdpFileTransport[port=%u]: E_UDP_DELIVERY_GIVEUP msgId=%u artifact=%s "
                    "after %d retries",
                    localPort_, mid, qPrintable(ob.artifactBase), ob.retries);
                QFile::rename(ob.artifactBase + QLatin1String(".sending"),
                              ob.artifactBase + QLatin1String(".failed"));
                QFile::rename(ob.readySendingPath,
                              ob.readySendingPath.left(ob.readySendingPath.size() - 8) +
                                  QLatin1String(".failed"));
                outbound.remove(mid);
            }
        }

        // ── 淘汰超时半成品 + 过期短表 ─────────────────────────────────────────
        reasm.evictStale(now);
    }
}
