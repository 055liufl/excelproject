#pragma once

#include <QAtomicInt>
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QMap>
#include <QString>
#include <QThread>

class QUdpSocket;

// ============================================================================
// udp_transport.h — sync-demo 专用的「基于 UDP 的可靠文件搬运传输层（ARQ 版）」
// ============================================================================
//
// 本版相比旧版的核心变更：
//   · DATA 头新增 type 字节（18B 固定头）；ACK 包（9B）；线格式严格大端。
//   · setMaxTransmitBytes(bytes)：设置单个 UDP 数据报总长上限，默认 60000，< 274 时拒绝。
//   · fragmentMessage()：纯函数，文件 → N 个 ≤ maxBytes 的自描述 DATA 数据报。
//   · FragmentReassembler：有状态机，负责入站校验 / 重组 / 去重 / 超时淘汰 / durable 解耦。
//   · 全量重传 ARQ：发送端按 RTO 周期性重发未 ACK 消息，直到 ACK 或 maxRetries 放弃。
//   · 交付以 durable 落盘为准：ACK 只在「写 inbox 主文件 + .ready 两步均成功」后发送。
//   · 出站状态以磁盘后缀为唯一真源，删除内存 sentFiles_；原子认领 (.ready→.ready.sending)。
//   · 归并键含发送端点 (senderKey, msgId)；msgId 改单调计数器 + 随机起点，消除碰撞。
//
// 线格式（大端）：
//   DATA (type=0x01): magic(4)|0x01|msg_id(4)|frag_idx(2)|frag_count(2)|total_size(4)|fname_len(1)
//                     |fname[fname_len]|payload[M]    ← 总长 ≤ maxTransmitBytes
//   ACK  (type=0x02): magic(4)|0x02|msg_id(4)         ← 精确 9 字节
// ============================================================================

// UdpPeerEndpoint —— 一个对端的 UDP 落点（主机地址 + 端口）。
struct UdpPeerEndpoint {
    QHostAddress host;
    quint16 port = 0;
};

// ── 拆包 ────────────────────────────────────────────────────────────────────

struct FragmentResult {
    QList<QByteArray> datagrams;
    bool ok = false;
    QString error;
};

// fragmentMessage —— 把一条消息切成若干 ≤ maxTransmitBytes 的 DATA 数据报（纯函数，无副作用）。
// 空 data → 1 个空净荷片（ok）；文件名 UTF-8 > 255 / fragCount > 65535 / M < 1 → ok=false。
FragmentResult fragmentMessage(quint32 msgId, const QString& filename, const QByteArray& data,
                               int maxTransmitBytes);

// ── 组包状态机 ───────────────────────────────────────────────────────────────

struct RxEvent {
    enum Kind { None, Completed, NeedAck } kind = None;
    quint32 msgId = 0;
    QString filename;  // Completed 时有效
    QByteArray bytes;  // Completed 时有效
};

// FragmentReassembler —— 接收端重组状态机（可脱离 socket 单测）。
// 承载 §4.4 全部入站校验、乱序存片、complete 检测、assemble + 总长校验、
// 去重短表、pending 超时淘汰。
// feed() 返回 Completed 时不写短表；外层 durable 落盘成功后调 markDelivered() 才入短表。
class FragmentReassembler {
   public:
    explicit FragmentReassembler(int reassemblyTimeoutMs, int completedRetentionMs);

    // 校验 + 存片；组齐 → Completed（不进短表）；已完成消息重收 → NeedAck。
    RxEvent feed(const QString& senderKey, const QByteArray& datagram, qint64 nowMs);

    // 外层 durable 落盘成功后调用 → 把 (senderKey,msgId) 记入已完成短表。
    void markDelivered(const QString& senderKey, quint32 msgId, qint64 nowMs);

    // 淘汰超时 pending + 过期短表项。
    void evictStale(qint64 nowMs);

    // 供测试断言「无泄漏」。
    int pendingCount() const;

   private:
    struct PendingMsg {
        QString filename;
        quint32 totalSize = 0;
        quint16 fragCount = 0;
        quint8 fnLen = 0;
        QMap<quint16, QByteArray> frags;
        qint64 createdAt = 0;
        qint64 lastProgressAt = 0;

        bool complete() const {
            return fragCount > 0 && frags.size() == static_cast<int>(fragCount);
        }
    };

    struct CompletedEntry {
        qint64 completedAt = 0;
    };

    using MsgKey = QPair<QString, quint32>;  // (senderKey, msgId)

    QHash<MsgKey, PendingMsg> pending_;
    QHash<MsgKey, CompletedEntry> completed_;
    int reassemblyTimeoutMs_;
    int completedRetentionMs_;
};

// ── 传输层 ───────────────────────────────────────────────────────────────────

class UdpFileTransport : public QThread {
    Q_OBJECT

   public:
    // 单 peer 构造（edge 节点）。
    UdpFileTransport(quint16 localPort, const QHostAddress& peerHost, quint16 peerPort,
                     const QString& outboxDir, const QString& inboxDir, QObject* parent = nullptr);

    // 多 peer 构造（center 节点）。
    UdpFileTransport(quint16 localPort, const QHash<QString, UdpPeerEndpoint>& peers,
                     const QString& outboxDir, const QString& inboxDir, QObject* parent = nullptr);

    // 设置单个 UDP 数据报总长上限。必须在 start() 之前调用。
    // bytes < 274 时拒绝（返回 false）；否则设置并返回 true。默认 60000。
    bool setMaxTransmitBytes(int bytes);

    // 线程安全的停止请求（置原子标志，须再 wait()）。
    void requestStop();

   protected:
    void run() override;

   private:
    // 线格式常量
    static constexpr quint32 kMagic = 0xDB5ACED0u;
    static constexpr quint8 kTypeData = 0x01;
    static constexpr quint8 kTypeAck = 0x02;
    static constexpr int kFixedHeader =
        18;  // magic(4)+type(1)+msg_id(4)+frag_idx(2)+frag_count(2)+total_size(4)+fname_len(1)
    static constexpr int kAckSize = 9;        // magic(4)+type(1)+msg_id(4)
    static constexpr int kMinMaxBytes = 274;  // 18头 + 255最长文件名 + 1净荷

    // ARQ 编译期常量（不提供 setReliabilityParams 公开接口）
    static constexpr int kRtoMs = 500;
    static constexpr int kMaxRetries = 5;
    static constexpr int kReassemblyTimeoutMs = 8000;
    static constexpr int kCompletedRetentionMs = 8000;
    static constexpr int kPollMs = 50;

    // 出站在途消息
    struct Outbound {
        QHostAddress destHost;
        quint16 destPort = 0;
        quint32 msgId = 0;
        QList<QByteArray> datagrams;  // 所有分片（全量重传不再读盘）
        QString artifactBase;         // 主文件路径（不含后缀，用于终态改名）
        QString readySendingPath;     // .ready.sending 路径
        qint64 lastSentAt = 0;
        int retries = 0;
        int fragCount = 0;  // 记录用于观测日志
    };

    // 从工件文件名解析目标 peer 的 nodeId（多 peer 模式用）。
    static QString extractTargetPeer(const QString& filename);

    // outbox 轮询：原子认领 → 读取 → 拆包 → 发送 → 入 outbound。
    void pollOutbox(QUdpSocket& sock, QHash<quint32, Outbound>& outbound,
                    FragmentReassembler& reasm, qint64 nowMs);

    // 发送 ACK 到指定端点。
    void sendAck(QUdpSocket& sock, quint32 msgId, const QHostAddress& dest, quint16 destPort);

    quint16 localPort_;
    QHash<QString, UdpPeerEndpoint> peers_;
    QString outboxDir_;
    QString inboxDir_;
    QAtomicInt stop_;
    int maxTransmitBytes_ = 60000;
    quint32 msgSeq_ = 0;  // 单调计数器，随机起点，构造时初始化
};
