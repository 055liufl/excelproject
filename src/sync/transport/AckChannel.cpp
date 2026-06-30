#include "AckChannel.h"

#include <QDateTime>
#include <QStringList>

#include "../SyncDDL.h"  // ddl::ackArtifactName —— 生成带 from/to 路由的 .ack 文件名
#include <limits>        // std::numeric_limits<qint64>::max()（“无截止”哨兵）

// ============================================================================
// AckChannel.cpp — ACK 攒批发送的实现
// 攒批动机、路由（fromPeer/toPeer）与失败保留语义详见 AckChannel.h 文件头注释。
// ============================================================================

namespace dbridge::sync {

AckChannel::AckChannel(OutboxWriter& writer, const QString& nodeId, qint64 ackMaxDelayMs)
    : writer_(writer), nodeId_(nodeId), ackMaxDelayMs_(ackMaxDelayMs) {
    // 把“上次 flush 时刻”初始化为“现在”，于是首个攒批窗口从对象创建那一刻开始计时。
    lastFlushMs_ = QDateTime::currentMSecsSinceEpoch();
}

bool AckChannel::scheduleChangesetAck(const ChangesetAck& ack, PayloadCodec& codec, QString* err) {
    // 仅入队，不立即落盘；随后 maybeFlush 决定是否“到点了顺手刷一批”。
    pendingChangeset_.append(ack);
    return maybeFlush(codec, err);
}

bool AckChannel::schedulePushChunkAck(const PushChunkAck& ack, PayloadCodec& codec, QString* err) {
    pendingChunk_.append(ack);
    return maybeFlush(codec, err);
}

bool AckChannel::flush(PayloadCodec& codec, QString* err) {
    // 用同一个 nowMs 作为本批所有 ACK 文件名里的时间戳（保证同批命名时间一致）。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    bool ok = true;                       // 本批是否“全部成功”
    QStringList failures;                 // 收集每个失败项的诊断（最后拼成 *err）
    QList<ChangesetAck> failedChangeset;  // 失败的 changeset ACK，刷完后留回队列重试
    // —— 逐条发送 changeset ACK ——
    for (const ChangesetAck& ack : qAsConst(pendingChangeset_)) {
        // qAsConst 避免 for-range 对容器触发隐式 detach（深拷贝），纯读遍历更高效。
        QByteArray data = codec.encodeChangesetAck(ack);
        // fromPeer = nodeId_ (this node), toPeer = ack.toPeer (J-01 fix: was hardcoded "self")
        // 译：fromPeer 填本节点 nodeId_，toPeer 填 ack.toPeer（这条 ACK 要回给谁）。
        //   J-01 fix：早先把 fromPeer 写死成 "self"，导致接收方无法识别真正发件人/推错水位。
        const QString name = ddl::ackArtifactName(nodeId_, ack.toPeer, nowMs);
        QString writeErr;
        if (!writer_.writeAck(name, data, &writeErr)) {
            // 单条失败不中断整批：记下来、留回队列下次再发。
            ok = false;
            failedChangeset.append(ack);
            failures.append(QStringLiteral("%1: %2").arg(name, writeErr));
        }
    }

    QList<PushChunkAck> failedChunk;  // 失败的 push-chunk ACK，同样留回队列
    // —— 逐条发送 push-chunk ACK ——
    for (const PushChunkAck& ack : qAsConst(pendingChunk_)) {
        QByteArray data = codec.encodeChunkAck(ack);
        // H-04 fix: use toPeer (the push origin) so the ACK file is routed correctly.
        // 译：H-04 fix —— 用 toPeer（即该推送的发起方 origin）作为路由目标，ACK 文件才能
        //   被正确投递回推送方。回退逻辑：toPeer 为空时退用 pushId 兜底（极端情况下仍有去向）。
        const QString toPeer = ack.toPeer.isEmpty() ? ack.pushId : ack.toPeer;
        const QString name = ddl::ackArtifactName(nodeId_, toPeer, nowMs);
        QString writeErr;
        if (!writer_.writeAck(name, data, &writeErr)) {
            ok = false;
            failedChunk.append(ack);
            failures.append(QStringLiteral("%1: %2").arg(name, writeErr));
        }
    }

    // 关键：用「失败集合」整体替换原队列——成功的就此移除，失败的保留待下次重试。
    // std::move 把局部 list 的内部缓冲直接转移给成员，零拷贝。
    pendingChangeset_ = std::move(failedChangeset);
    pendingChunk_ = std::move(failedChunk);
    lastFlushMs_ = nowMs;  // 不论成败都刷新“上次 flush 时刻”，攒批窗口从此重新计时
    if (!ok && err)
        *err = failures.join(QStringLiteral("; "));  // 把多个失败诊断拼成一条
    return ok;
}

bool AckChannel::maybeFlush(PayloadCodec& codec, QString* err) {
    // 攒批的“到点判定”：距上次 flush 已 >= ackMaxDelayMs_ 才真正刷出，否则继续攒（返回 true）。
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (nowMs - lastFlushMs_ >= ackMaxDelayMs_)
        return flush(codec, err);
    return true;  // 还没到点：本次只入队、不落盘，视为成功
}

qint64 AckChannel::nextDeadlineMs() const {
    // M-03 fix: if there are no pending ACKs, no deadline — return INT64_MAX so the main loop
    // uses its normal broadcastIntervalMs sleep.  When ACKs are pending, return the time at
    // which they must be flushed to meet the ackMaxDelayMs SLA.
    // 译：M-03 fix —— 队列为空 → 无截止 → 返回 INT64_MAX，主循环按常规 broadcastIntervalMs 睡眠；
    //   队列非空 → 返回「最迟必须 flush 的时刻」(lastFlushMs_ + ackMaxDelayMs_)，主循环据此
    //   收紧睡眠以准时兑现 ackMaxDelayMs 的 SLA。
    if (pendingChangeset_.isEmpty() && pendingChunk_.isEmpty())
        return std::numeric_limits<qint64>::max();
    return lastFlushMs_ + ackMaxDelayMs_;
}

}  // namespace dbridge::sync
