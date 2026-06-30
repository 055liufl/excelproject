#pragma once
#include "dbridge/sync/SyncTypes.h"  // ChangesetAck / PushChunkAck 等 ACK 数据结构

#include <QList>

#include "../payload/PayloadCodec.h"  // 把 ACK 结构编码成 artifact 字节
#include "OutboxWriter.h"             // 把编码后的 ACK 原子发布到 outbox

// ============================================================================
// AckChannel.h — ACK（确认）发送端：批量攒一攒再统一刷到 outbox
// ============================================================================
//
// 【这个文件是什么 / 在管线中的位置】
//   节点收到对端的变更包（changeset）或选择性推送分片（push chunk）并成功处理后，
//   需要回一个 ACK 让对端推进它的「已确认水位」（outbound_ack），以便对端安全地
//   截断 changelog、判断本端是否健康。AckChannel 就是「ACK 的发送缓冲器」：
//   它把待发的 ACK 先排进内存队列，等到「攒够时间」（ackMaxDelayMs）或被显式 flush()
//   时，再一次性编码并通过 OutboxWriter 落盘成 .ack artifact。
//
// 【为什么要「批量延迟发送」而不是每个 ACK 立刻发】
//   ACK 流量极大且高度可合并：短时间内往往要给同一对端确认许多笔。逐个落盘会产生
//   大量小文件与 fsync 开销。攒一个最大延迟窗口（默认 5s）能显著降低 IO，又把确认
//   延迟限制在可接受的 SLA 内。代价是引入一个“最迟必须刷出”的截止时刻——见
//   nextDeadlineMs()，它让 SyncWorker 主循环据此收紧睡眠间隔，按时兑现这个 SLA。
//
// 【ACK 的路由：fromPeer / toPeer 为何如此重要】
//   ACK 也是一个 artifact，其文件名同样靠 SyncDDL 的命名 helper 生成，名字里编码了
//   from/to 两端。必须把 fromPeer 设为「本节点 nodeId_」、toPeer 设为「该 ACK 要回给
//   的对端」，接收方才能正确解析“这是谁发来的、确认的是我吗”，从而推进正确那一条
//   outbound_ack 水位（见 J-01 / H-04 fix；早期 bug 曾把 fromPeer 写死成 "self"，
//   或把 chunk ACK 路由错对端）。
//
// 【失败重试语义】
//   flush() 对每条 ACK 单独尝试落盘；失败的 ACK 会被「留在队列里」下次再发，成功的
//   则移除（见 .cpp）。即“尽力而为 + 失败保留”，不会因个别写失败而丢确认。
//
// 【线程模型】
//   非线程安全：队列与水位是普通成员，约定只在「单个 SyncWorker 线程」内调用。
//
// 【协作者】
//   · OutboxWriter —— 真正把 ACK 原子写入 outbox；
//   · PayloadCodec —— 把 ChangesetAck/PushChunkAck 序列化成字节；
//   · SyncDDL::ackArtifactName —— 生成带 from/to 路由信息的 .ack 文件名；
//   · SyncWorker —— 驱动 flush()，并用 nextDeadlineMs() 计算主循环睡眠时长。
// ============================================================================

namespace dbridge::sync {

// AckChannel —— 攒批发送 ACK artifact：达到 ackMaxDelayMs 或被显式 flush() 时统一刷出。
class AckChannel {
   public:
    // nodeId: this node's own identifier, used as the "fromPeer" in ACK artifact
    // names so that receivers can parse the sender correctly (J-01 fix).
    // 译：nodeId 是本节点自己的标识，会被填进 ACK artifact 文件名的 fromPeer 字段，
    //   接收方据此正确解析「发件人是谁」（J-01 fix：修复早先写死 "self" 的错误）。
    // writer 以引用持有（外部拥有其生命周期）；ackMaxDelayMs 是攒批最大延迟（默认 5000ms）。
    explicit AckChannel(OutboxWriter& writer, const QString& nodeId, qint64 ackMaxDelayMs = 5000);

    // scheduleChangesetAck —— 把一条 changeset ACK 入队。
    // May trigger an automatic flush（译：可能触发一次自动 flush——若已攒够 ackMaxDelayMs）。
    bool scheduleChangesetAck(const ChangesetAck& ack, PayloadCodec& codec, QString* err = nullptr);

    // schedulePushChunkAck —— 把一条 push-chunk ACK 入队（同样可能触发自动 flush）。
    bool schedulePushChunkAck(const PushChunkAck& ack, PayloadCodec& codec, QString* err = nullptr);

    // flush —— 立刻把队列里所有 ACK 编码并写入 outbox。
    // 返回 true 表示全部成功；任一失败返回 false，失败者保留在队列等下次重试（见 .cpp）。
    bool flush(PayloadCodec& codec, QString* err = nullptr);

    // M-03 fix: return the earliest time at which a pending ACK batch must be flushed
    // (lastFlushTime + ackMaxDelayMs), so the SyncWorker main loop can use it to compute
    // a tighter sleep interval instead of always sleeping broadcastIntervalMs.
    // Returns INT64_MAX when no ACKs are pending.
    // 译：M-03 fix —— 返回「当前待发批次最迟必须 flush 的时刻」(= lastFlushMs_ + ackMaxDelayMs_)。
    //   SyncWorker 主循环据此把睡眠时间收紧到这个截止点，而不是一律睡 broadcastIntervalMs，
    //   从而保证 ackMaxDelayMs 的 SLA 被准时兑现。队列为空时返回 INT64_MAX（无截止 → 用默认睡眠）。
    qint64 nextDeadlineMs() const;

   private:
    // maybeFlush —— “够时间了才刷”：若距上次 flush 已 >= ackMaxDelayMs_ 则 flush，否则不动。
    // schedule* 入队后都会调它，实现“攒批 + 到点自动刷出”。
    bool maybeFlush(PayloadCodec& codec, QString* err);

    OutboxWriter& writer_;                  // ACK 的落盘器（外部拥有，引用持有）
    QString nodeId_;                        // 本节点标识（充当所有 ACK 的 fromPeer）
    qint64 ackMaxDelayMs_;                  // 攒批最大延迟（SLA 上限）
    QList<ChangesetAck> pendingChangeset_;  // 待发的 changeset ACK 队列
    QList<PushChunkAck> pendingChunk_;      // 待发的 push-chunk ACK 队列
    qint64 lastFlushMs_ = 0;  // 上次 flush 的时刻（构造时初始化为“现在”）
};

}  // namespace dbridge::sync
