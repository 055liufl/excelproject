// ============================================================================
// udp_transport.cpp — UDP 文件搬运传输层的实现（构造 / 主循环 / 解析 / 收发）
// ============================================================================
// 详见 udp_transport.h 头注释。本文件实现六件事：
//   ① 两个构造（单 peer / 多 peer）；②requestStop（原子置停）；③run（收发主循环）；
//   ④extractTargetPeer（从文件名判目标）；⑤pollOutbox（扫 outbox 并发送）；
//   ⑥sendFile（切片发送）与 handleDatagram（重组写 inbox）。
// ============================================================================
#include "udp_transport.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QUdpSocket>

// ── Constructors ──────────────────────────────────────────────────────────────

// 单 peer 构造：edge 节点用。把唯一对端塞进一个「只有 1 项」的 peers_ map，键用哨兵
//   "__single__"。pollOutbox 据 peers_.size()==1 识别单 peer 模式，从而跳过「按文件名
//   解析目标」这一步——反正只有一个对端，发就完了。
UdpFileTransport::UdpFileTransport(quint16 localPort, const QHostAddress& peerHost,
                                   quint16 peerPort, const QString& outboxDir,
                                   const QString& inboxDir, QObject* parent)
    : QThread(parent), localPort_(localPort), outboxDir_(outboxDir), inboxDir_(inboxDir) {
    // Single-peer mode: store as a 1-entry map with a sentinel key.
    // pollOutbox detects peers_.size()==1 and skips filename-based peer extraction.
    // 单 peer 模式：用「1 项 + 哨兵键」的 map 承载；pollOutbox 见 size()==1 即跳过文件名解析。
    peers_.insert(QStringLiteral("__single__"), {peerHost, peerPort});
}

// 多 peer 构造：center 节点用。直接保存整张 nodeId→落点表，发送时按文件名解析目标后查表。
UdpFileTransport::UdpFileTransport(quint16 localPort, const QHash<QString, UdpPeerEndpoint>& peers,
                                   const QString& outboxDir, const QString& inboxDir,
                                   QObject* parent)
    : QThread(parent),
      localPort_(localPort),
      peers_(peers),
      outboxDir_(outboxDir),
      inboxDir_(inboxDir) {
}

// requestStop —— 跨线程置「停止」原子标志。storeRelease 配合 run() 里的 loadAcquire 构成
//   release/acquire 内存序：保证置位前的写对感知到标志的线程可见。本身不阻塞，须再 wait()。
void UdpFileTransport::requestStop() {
    stop_.storeRelease(1);
}

// ── Run loop ──────────────────────────────────────────────────────────────────

// run —— 后台线程主体：绑定本地端口，进入「收 + 发」轮询循环直到被请求停止。
//   流程：① bind 本地 UDP 端口（失败则告警并退出线程）；② 确保 inbox 目录存在；
//   ③ 循环：先用 waitForReadyRead(kPollMs) 等至多 50ms 看有没有入站报文，有则把所有待
//   处理数据报逐个 handleDatagram 重组；随后 pollOutbox 扫一遍出站目录把新工件发出去。
//   收发同线程交替进行——demo 流量小，单线程轮询足矣，无需独立收发线程。
//   pending：本线程私有的「按 msg_id 暂存半成品消息」表，跨多次循环累积分片直到集齐。
void UdpFileTransport::run() {
    QUdpSocket sock;
    // bind 到 loopback 的指定端口；该 socket 只在本线程内使用（QUdpSocket 非线程安全）。
    if (!sock.bind(QHostAddress::LocalHost, localPort_)) {
        qWarning("UdpFileTransport[port=%u]: bind failed — %s", localPort_,
                 qPrintable(sock.errorString()));
        return;  // 绑定失败：无法收发，直接结束线程（调用方 wait() 会立即返回）
    }

    QDir(inboxDir_).mkpath(QStringLiteral("."));  // 确保 inbox 存在，否则写入会失败

    QMap<quint32, PendingMsg> pending;  // msg_id → 装配中的消息（分片暂存区）

    while (!stop_.loadAcquire()) {  // 检测停止标志（acquire 序，见 requestStop）
        // 收：等至多 kPollMs；期间有报文则把内核缓冲里的全部数据报一次性收干净。
        if (sock.waitForReadyRead(kPollMs)) {
            while (sock.hasPendingDatagrams()) {
                // 按实际报文大小分配缓冲（Qt::Uninitialized 跳过清零，纯性能优化）。
                QByteArray dg(static_cast<int>(sock.pendingDatagramSize()), Qt::Uninitialized);
                QHostAddress sender;
                quint16 senderPort = 0;
                sock.readDatagram(dg.data(), dg.size(), &sender, &senderPort);
                handleDatagram(dg, pending);  // 解析并归入 pending；集齐则落 inbox
            }
        }
        pollOutbox(sock);  // 发：扫 outbox，新工件切片发出
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

// extractTargetPeer —— 仅多 peer 模式用：从工件文件名里「读出」该发给谁。
//   做什么：先去掉扩展名（.payload/.ack），按分隔符 "__" 切成字段，再按工件种类的命名规则
//     取出目标 peer 字段。不同种类的目标位置不同（见下逐分支），故按 parts[0]/parts[2]/
//     parts.last() 等线索逐一识别。
//   为什么把路由信息编进文件名：传输层无需理解工件内容，仅凭文件名即可分派——这让传输层
//     与同步协议解耦（命名契约权威定义见 SyncDDL.h）。
//   返回：目标 peer 的 nodeId；任何无法判定的情形返回空串（调用方据此跳过该文件）。
//   纯函数、静态、无副作用。
QString UdpFileTransport::extractTargetPeer(const QString& filename) {
    QString name = filename;
    // 去扩展名：.payload 占 8 字符、.ack 占 4 字符（chop 从尾部裁掉 n 个字符）。
    if (name.endsWith(QStringLiteral(".payload")))
        name.chop(8);
    else if (name.endsWith(QStringLiteral(".ack")))
        name.chop(4);

    const QStringList parts = name.split(QStringLiteral("__"));  // 按双下划线拆字段
    if (parts.isEmpty())
        return {};

    // ack__{from}__{to}__{ms}__{uuid} → 目标在第 3 字段（下标 2）。
    if (parts.at(0) == QStringLiteral("ack") && parts.size() >= 3)
        return parts.at(2);

    // blreq__{from}__{to}__{ms} → 目标在下标 2。
    if (parts.at(0) == QStringLiteral("blreq") && parts.size() >= 3)
        return parts.at(2);

    // {from}__{to}__...__baselineresponse 或 ...__baselinerequest → 目标(to)在下标 1。
    //   这两类以「最后一个字段」为种类标记，故先看 last 再取 to。
    const QString last = parts.last();
    if ((last == QStringLiteral("baselineresponse") || last == QStringLiteral("baselinerequest")) &&
        parts.size() >= 2)
        return parts.at(1);

    // {origin}__{epoch}__changeset__{seq12}__{peer}-{uuid}
    //   种类标记在下标 2；目标 peer 嵌在下标 4 的「{peer}-{uuid}」里，取最后一个 '-' 之前的部分。
    //   用 lastIndexOf('-')：peer 名本身一般不含 '-'，而 uuid 在其后，故末尾的 '-' 即分界。
    if (parts.size() >= 5 && parts.at(2) == QStringLiteral("changeset")) {
        const QString seg = parts.at(4);
        const int dash = seg.lastIndexOf(QLatin1Char('-'));
        if (dash > 0)
            return seg.left(dash);  // '-' 左侧即 peer 名
    }

    // {origin}__{epoch}__selectionpush__{id.chunk}__peer-{peer}-{uuid}
    //   下标 4 形如 "peer-{peer}-{uuid}"：先剥掉 "peer-" 前缀，再取剩余串里最后一个 '-' 之前。
    if (parts.size() >= 5 && parts.at(2) == QStringLiteral("selectionpush")) {
        const QString seg = parts.at(4);
        const QString prefix = QStringLiteral("peer-");
        if (seg.startsWith(prefix)) {
            const QString rest = seg.mid(prefix.size());  // 去掉 "peer-"
            const int dash = rest.lastIndexOf(QLatin1Char('-'));
            if (dash > 0)
                return rest.left(dash);  // 最后一个 '-' 左侧即 peer 名
        }
    }

    return {};  // 无法识别 → 空串（跳过）
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

// pollOutbox —— 扫一遍 outbox，把每个「就绪」的新工件投递出去。
//   就绪协议（与 OutboxWriter / InboxWatcher 一致，见上方 .ready 说明）：写工件分两步——先写
//     {name}.payload（内容），再写空的 {name}.payload.ready 哨兵；只有看到 .ready 才说明
//     payload 已写完整、可安全读取。故本函数只遍历 *.ready，再回推对应 payload 名。
//   去重：内存 sentFiles_ 记本会话已发过的文件名，避免同一文件在改名生效前被重复扫描发送；
//     发送成功后再把 payload 与 ready 双双改名加 .sent 后缀，使下轮 *.ready 列表不再包含它。
//   选目标：单 peer 模式恒发唯一落点；多 peer 模式按文件名解析目标并查 peers_，无法识别或
//     未登记的目标直接跳过（continue）。
void UdpFileTransport::pollOutbox(QUdpSocket& sock) {
    QDir dir(outboxDir_);
    if (!dir.exists())
        return;

    // 只列就绪哨兵，按文件名排序（QDir::Name）使发送顺序稳定可预期。
    const QStringList readyFiles =
        dir.entryList(QStringList{QStringLiteral("*.ready")}, QDir::Files, QDir::Name);

    for (const QString& readyName : readyFiles) {
        const QString payloadName =
            readyName.left(readyName.size() - 6);  // strip ".ready"
                                                   // 去掉 ".ready" 6 字符得 payload 名
        const QString payloadPath = dir.filePath(payloadName);
        const QString readyPath = dir.filePath(readyName);

        if (sentFiles_.contains(payloadName))
            continue;  // 本会话已发过 → 跳过（防改名生效前的重复发送）
        if (!QFile::exists(payloadPath))
            continue;  // 有哨兵却无 payload（异常/竞态）→ 跳过

        // Determine destination endpoint.
        // 决定投递落点（主机 + 端口）。
        QHostAddress destHost;
        quint16 destPort = 0;

        if (peers_.size() == 1) {
            // Single-peer mode: always send to the only endpoint.
            // 单 peer 模式：恒发往唯一落点，无需解析文件名。
            const auto ep = peers_.constBegin().value();
            destHost = ep.host;
            destPort = ep.port;
        } else {
            // Multi-peer mode: derive target peer from artifact filename.
            // 多 peer 模式：从工件文件名解析目标 peer，再查 peers_ 取落点。
            const QString peer = extractTargetPeer(payloadName);
            if (peer.isEmpty() || !peers_.contains(peer))
                continue;  // unknown or unregistered target — skip
                           // 目标无法识别或未登记 → 跳过该文件
            destHost = peers_[peer].host;
            destPort = peers_[peer].port;
        }

        sendFile(sock, payloadPath, payloadName, destHost, destPort);  // 切片发送
        sentFiles_.insert(payloadName);                                // 记入已发集合
        // 双双改名加 .sent：使下一轮 *.ready 不再命中它，达成「发过即不重发」。
        QFile::rename(payloadPath, payloadPath + QLatin1String(".sent"));
        QFile::rename(readyPath, readyPath + QLatin1String(".sent"));
    }
}

// ── Send a single file as UDP fragments ──────────────────────────────────────

// sendFile —— 把一个文件按 kFragSize 切片，逐片封成数据报发往 destHost:destPort。
//   头部每片都带：magic（甄别）、msgId（同一文件所有片共享，供接收端归并）、frag_idx/
//     frag_count（重组顺序与是否齐全）、total_size（预分配）、fname_len + fname（接收端据此
//     还原目标文件名）。净荷即该片对应的文件字节段。
//   设计要点：
//     · 空文件特判 fragCount=1：仍发一片（净荷为空），否则空文件不会被传输（如空 .ready 体）。
//     · msgId 用随机数：同一进程可能连发多个文件，随机 id 让接收端的 pending 表互不串扰
//       （碰撞概率极低，demo 量级可忽略）。
//     · fname_len 是 1 字节（quint8），故文件名 UTF-8 编码后不得超 255 字节，超则告警跳过。
//     · QDataStream 显式设 BigEndian，与接收端 handleDatagram 的读取字节序严格一致。
//   错误处理：writeDatagram 返回值 != 报文长视为「短写」，仅告警（无重传——demo 容忍）。
void UdpFileTransport::sendFile(QUdpSocket& sock, const QString& filePath, const QString& filename,
                                const QHostAddress& destHost, quint16 destPort) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly))
        return;  // 读不开（被并发改名等）→ 放弃本文件
    const QByteArray data = f.readAll();
    f.close();

    const QByteArray fnUtf8 = filename.toUtf8();
    if (fnUtf8.size() > 255) {  // 文件名长度需能塞进 1 字节的 fname_len
        qWarning("UdpFileTransport: filename too long (%d bytes), skipping", fnUtf8.size());
        return;
    }

    const auto totalSize = static_cast<quint32>(data.size());
    // 分片数 = ceil(size/kFragSize)；空文件特判为 1 片（确保空体也被传输并触发接收端落 inbox）。
    const int fragCount =
        data.isEmpty() ? 1 : (static_cast<int>(data.size()) + kFragSize - 1) / kFragSize;
    // 本文件的消息 id：随机生成，使接收端能把属于本文件的各片聚合、与其它文件区分。
    const auto msgId = static_cast<quint32>(QRandomGenerator::global()->generate());

    for (int i = 0; i < fragCount; ++i) {
        const QByteArray frag = data.mid(i * kFragSize, kFragSize);  // 本片净荷（末片可能不足整块）

        QByteArray dg;
        {
            // 写定长头部（大端）。字段顺序/类型必须与 handleDatagram 的读取完全对应。
            QDataStream ds(&dg, QIODevice::WriteOnly);
            ds.setByteOrder(QDataStream::BigEndian);
            ds << kMagic << msgId << static_cast<quint16>(i) << static_cast<quint16>(fragCount)
               << totalSize << static_cast<quint8>(fnUtf8.size());
        }
        dg.append(fnUtf8);  // 变长：文件名字节（长度由头部 fname_len 指明）
        dg.append(frag);    // 变长：本片净荷

        const qint64 sent = sock.writeDatagram(dg, destHost, destPort);
        if (sent != dg.size()) {  // 短写：仅告警（UDP + demo，无重传/确认机制）
            qWarning("UdpFileTransport[port=%u]: short write frag %d of %s", localPort_, i,
                     qPrintable(filename));
        }
    }
}

// ── Datagram reassembly ───────────────────────────────────────────────────────

// handleDatagram —— 处理一个收到的数据报：校验头部 → 归入对应 PendingMsg → 集齐则落 inbox。
//   解析与 sendFile 的写入严格对称（同样 BigEndian、同样字段顺序）。逐步：
//     ① 长度/魔数/流状态校验：任一不过即丢弃（防野包/截断包破坏装配）；
//     ② 取文件名与净荷：文件名长度由头部 fnLen 指明，净荷是其后的剩余字节；
//     ③ 归并：用 msgId 定位（或新建）PendingMsg；首片(fragCount==0 占位)时填元数据，再按
//        frag_idx 存净荷。重复片会被同 key 覆盖（幂等，无害）；
//     ④ 收齐（complete）则 assemble 还原整文件，先写 {name}（内容）再建空 {name}.ready 哨兵
//        ——这一「先内容后哨兵」顺序与 InboxWatcher「只认 .ready」的约定配合，确保引擎读到的
//        永远是完整文件；最后从 pending 移除该 msgId 释放暂存。
//   pending 引用由 run() 持有并跨多次调用累积——分片可能分布在多轮轮询中陆续到达。
void UdpFileTransport::handleDatagram(const QByteArray& dg, QMap<quint32, PendingMsg>& pending) {
    // Minimum header: magic(4)+msg_id(4)+frag_idx(2)+frag_count(2)+total(4)+fnlen(1) = 17
    // 最小头部 = 4+4+2+2+4+1 = 17 字节；不足即非法包，丢弃。
    if (dg.size() < 17)
        return;

    QDataStream ds(dg);
    ds.setByteOrder(QDataStream::BigEndian);  // 必须与发送端字节序一致

    quint32 magic, msgId, totalSize;
    quint16 fragIdx, fragCount;
    quint8 fnLen;
    ds >> magic >> msgId >> fragIdx >> fragCount >> totalSize >> fnLen;  // 顺序对应 sendFile

    if (magic != kMagic || ds.status() != QDataStream::Ok)
        return;  // 魔数不符或流读取出错 → 非本协议/损坏包，丢弃

    constexpr int kFixedHeader = 17;
    if (dg.size() < kFixedHeader + static_cast<int>(fnLen))
        return;  // 头部声称的文件名长度超出实际报文 → 截断/伪造，丢弃

    // 文件名紧跟固定头部，长度为 fnLen；净荷是文件名之后的全部剩余字节。
    const QString filename =
        QString::fromUtf8(dg.constData() + kFixedHeader, static_cast<int>(fnLen));
    const QByteArray payload = dg.mid(kFixedHeader + static_cast<int>(fnLen));

    auto& msg = pending[msgId];  // 取或新建该 msgId 的装配条目（operator[] 不存在则默认构造）
    if (msg.fragCount == 0) {  // 首次见到该消息：填入元数据（fragCount==0 即「尚未初始化」）
        msg.filename = filename;
        msg.totalSize = totalSize;
        msg.fragCount = fragCount;
    }
    msg.frags[fragIdx] = payload;  // 按片序号存净荷（重复片覆盖，幂等）

    if (!msg.complete())
        return;  // 还没集齐 → 等后续报文（msg 留在 pending 里继续累积）

    // All fragments received — write to inbox, then create .ready sentinel.
    // 全部分片到齐——还原整文件写入 inbox，再创建 .ready 哨兵（先内容后哨兵，见函数头说明）。
    const QByteArray fileData = msg.assemble();
    const QString destPath = QDir(inboxDir_).filePath(msg.filename);
    const QString readyPath = destPath + QLatin1String(".ready");

    QFile out(destPath);
    if (!out.open(QIODevice::WriteOnly)) {
        qWarning("UdpFileTransport[port=%u]: cannot write inbox file %s", localPort_,
                 qPrintable(destPath));
        pending.remove(msgId);  // 写不了也要清掉装配条目，避免内存里残留半成品
        return;
    }
    out.write(fileData);
    out.flush();  // flush+close 确保内容在哨兵出现前已落盘
    out.close();

    QFile ready(readyPath);  // 创建空哨兵文件，通知 InboxWatcher「此工件已就绪可读」
    ready.open(QIODevice::WriteOnly);
    ready.close();

    pending.remove(msgId);  // 该消息已完成 → 释放暂存
}
