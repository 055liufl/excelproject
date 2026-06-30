#pragma once

#include <QAtomicInt>
#include <QHash>
#include <QHostAddress>
#include <QMap>
#include <QSet>
#include <QString>
#include <QThread>

class QUdpSocket;

// ============================================================================
// udp_transport.h — sync-demo 专用的「基于 UDP 的文件搬运传输层」
// ============================================================================
//
// 【这个文件是什么】
//   一个仅供 demo 使用的、跑在后台线程里的「传输适配器」。dbridge 同步引擎本身不关心
//   工件怎么过网络——它只把要发的工件写进 outbox 目录、从 inbox 目录读收到的工件。本类
//   就充当二者之间的「搬运工」：监视本节点 outbox 里的新文件，按文件名判出目标 peer，
//   分片经 UDP 发出去；同时监听本地端口，把收到的数据报重组还原成文件、落进 inbox。
//
// 【为什么需要它（在 demo 中的价值）】
//   真实部署里传输层可能是消息队列 / HTTP / 共享盘等；demo 选 UDP loopback 是为了用最少
//   代码演示「多节点真的在网络上互发工件」。它故意做得很朴素：仅 loopback、无重传、无拥塞
//   控制——因为 demo 只需在本机几个端口间可靠到达即可，复杂度留给生产实现。
//
// 【两种工作模式（由构造函数区分）】
//   · 单 peer 模式（edge 节点）：只有一个对端（center），outbox 里的文件无脑全发给它，
//     无需解析文件名。用「两参 peerHost/peerPort」构造。
//   · 多 peer 模式（center 节点）：一个线程覆盖多个 edge 方向，必须从每个工件的【文件名】
//     里解析出目标 peer 再分别投递。用「QHash<nodeId, endpoint>」构造。
//
// 【工件文件名 → 目标 peer 的约定（与 SyncDDL.h 的命名契约一致）】
//   changeset       : {origin}__{epoch}__changeset__{seq12}__{peer}-{uuid}.payload
//   selectionpush   : ...selectionpush...__peer-{peer}-{uuid}.payload
//   baselineresponse: {from}__{to}__{epoch}__...__baselineresponse.payload  → to
//   blreq           : blreq__{from}__{to}__{ms}.payload                      → to
//   ack             : ack__{from}__{to}__{ms}__{uuid}.ack                    → to
//   （即：文件名自带路由信息，传输层仅凭文件名即可决定发往谁——见 extractTargetPeer。）
//
// 【数据报线格式（所有整数大端 big-endian）】
//   [magic:4u][msg_id:4u][frag_idx:2u][frag_count:2u][total_size:4u]
//   [fname_len:1u][fname:fname_len 字节][payload:剩余字节]
//   一个文件可能超过单个 UDP 报文上限，故按 kFragSize 切片，每片带同一 msg_id 与
//   (frag_idx/frag_count) 供接收端重组（见 .cpp handleDatagram/PendingMsg）。
//
// 【发送后的去重】发完一个文件的所有分片后，把 outbox 里的源文件改名为 "<name>.sent"，
//   使下一轮轮询不再重发它（再叠加内存里的 sentFiles_ 双保险）。
//
// 【线程模型】本类是 QThread 子类：run() 在独立后台线程里跑「收发轮询」主循环。停止靠
//   原子标志 stop_（requestStop 置位、run 循环检测），跨线程安全；调用方 requestStop() 后
//   须再 wait() 等线程真正退出。socket 仅在 run() 所在线程内创建与使用。
// ============================================================================

// UdpPeerEndpoint —— 一个对端的 UDP 落点（主机地址 + 端口）。
// Per-peer UDP endpoint (host + port).
struct UdpPeerEndpoint {
    QHostAddress host;  // 对端主机（demo 中恒为 loopback 127.0.0.1）
    quint16 port = 0;   // 对端监听端口
};

// UdpFileTransport —— 后台线程：基于 UDP 的文件搬运传输层（详见上方文件头）。
// Background thread: file-transfer transport over UDP.
//
// Watches outboxDir for new files, determines the target peer from the artifact
// filename, sends the file to the corresponding endpoint, and simultaneously
// listens on localPort, reassembling incoming datagrams into inboxDir.
// Designed for loopback use; no retransmission.
// （监视 outboxDir 的新文件，从工件文件名判出目标 peer，发往对应落点；同时在 localPort 上
//  监听，把收到的数据报重组写入 inboxDir。仅为 loopback 设计，无重传。）
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
    // 单 peer 构造（edge 节点：唯一对端 = center）。outbox 文件全发给该对端，不解析文件名。
    // Single-peer constructor (edge nodes: one peer = center).
    UdpFileTransport(quint16 localPort, const QHostAddress& peerHost, quint16 peerPort,
                     const QString& outboxDir, const QString& inboxDir, QObject* parent = nullptr);

    // 多 peer 构造（center 节点：向多个 edge 落点发送）。键=对端 nodeId（如 "edge_b"），
    //   值=该对端 UDP 落点；发送时按文件名解析目标 peer 后查此表得落点。
    // Multi-peer constructor (center node: sends to multiple edge endpoints).
    // Key = peer nodeId (e.g. "edge_b"), Value = UDP endpoint.
    UdpFileTransport(quint16 localPort, const QHash<QString, UdpPeerEndpoint>& peers,
                     const QString& outboxDir, const QString& inboxDir, QObject* parent = nullptr);

    // 线程安全的停止请求：置原子标志后立即返回（不阻塞）。之后须调用 wait() 等线程退出。
    // Thread-safe stop: sets flag and returns immediately. Call wait() afterwards.
    void requestStop();

   protected:
    // QThread 线程入口：bind 本地端口后进入「收（重组入 inbox）+ 发（轮询 outbox）」主循环，
    //   直到 stop_ 被置位。具体见 .cpp run()。
    void run() override;

   private:
    static constexpr quint32 kMagic = 0xDB5ACED0u;  // 数据报魔数：收端据此快速甄别/丢弃异包
    static constexpr int kFragSize =
        60'000;  // fragment payload bytes（单分片净荷字节，留足 UDP 余量）
    static constexpr int kPollMs = 50;  // outbox poll / receive-wait interval（轮询/收等间隔毫秒）

    // PendingMsg —— 接收端「半成品消息」：同一 msg_id 的各分片到齐前的暂存装配区。
    //   收到一片就按 frag_idx 存进 frags；frags 凑满 fragCount 即可 assemble() 还原整文件。
    struct PendingMsg {
        QString filename;       // 该消息还原后的目标文件名（取自首片头部）
        quint32 totalSize = 0;  // 整文件总字节（来自头部，用于 reserve 预分配）
        quint16 fragCount = 0;  // 总分片数（==0 表示尚未收到任何片、刚创建）
        QMap<quint16, QByteArray> frags;  // frag_idx → 该片净荷（用有序 map 便于按序拼接）

        // 是否已收齐所有分片（已到片数 == 期望片数）。
        bool complete() const {
            return frags.size() == static_cast<int>(fragCount);
        }
        // 按 0..fragCount-1 顺序拼接各片净荷，还原出完整文件字节。
        QByteArray assemble() const {
            QByteArray data;
            data.reserve(static_cast<int>(totalSize));  // 预留总长，避免逐片 append 反复扩容
            for (quint16 i = 0; i < fragCount; ++i)
                data += frags.value(i);  // 缺片则 value() 返回空（complete() 已保证不缺）
            return data;
        }
    };

    // 从工件文件名解析目标 peer 的 nodeId；无法判定时返回空串（多 peer 模式下据此跳过）。
    // Extract the target peer nodeId from an artifact filename.
    // Returns an empty string if the peer cannot be determined.
    static QString extractTargetPeer(const QString& filename);

    // 轮询 outbox：对每个「就绪」工件判出目标落点并 sendFile，发完改名 .sent。详见 .cpp。
    void pollOutbox(QUdpSocket& sock);
    // 把单个文件切片为若干 UDP 数据报发往 destHost:destPort。详见 .cpp。
    void sendFile(QUdpSocket& sock, const QString& filePath, const QString& filename,
                  const QHostAddress& destHost, quint16 destPort);
    // 处理一个收到的数据报：解析头部、归入对应 PendingMsg；集齐后写 inbox 并建 .ready 哨兵。
    void handleDatagram(const QByteArray& dg, QMap<quint32, PendingMsg>& pending);

    quint16 localPort_;                      // 本节点 UDP 监听端口
    QHash<QString, UdpPeerEndpoint> peers_;  // nodeId → endpoint（仅 1 项 = 单 peer 模式）
    QString outboxDir_;  // 出站目录：同步引擎把待发工件写在这里
    QString inboxDir_;   // 入站目录：本类把收齐的工件还原到这里供引擎读取
    QAtomicInt stop_;  // 停止标志（原子，跨线程；requestStop 置位、run 循环检测）
    QSet<QString> sentFiles_;  // basenames sent this session（本次会话已发过的文件名，防重发）
};
