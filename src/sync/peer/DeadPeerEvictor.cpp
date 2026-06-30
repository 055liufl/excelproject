#include "sync/peer/DeadPeerEvictor.h"

#include <QSqlError>  // lastError() 文本
#include <QSqlQuery>
#include <QVariant>

// ============================================================================
// DeadPeerEvictor.cpp — 对端失联判定与驱逐的实现
// 三维阈值与驱逐语义详见 DeadPeerEvictor.h 文件头注释。
// ============================================================================

namespace dbridge::sync {

void DeadPeerEvictor::configure(qint64 softSeq, qint64 hardSeq, qint64 softBytes, qint64 hardBytes,
                                qint64 softMs, qint64 hardMs) {
    // 简单地用外部配置覆盖六个阈值（不校验“软 < 硬”——由调用方/配置层保证合理性）。
    softSeq_ = softSeq;
    hardSeq_ = hardSeq;
    softBytes_ = softBytes;
    hardBytes_ = hardBytes;
    softMs_ = softMs;
    hardMs_ = hardMs;
}

DeadPeerEvictor::AlertLevel DeadPeerEvictor::evaluate(const PeerState& peer, qint64 nowMs) const {
    // 已被驱逐过的对端：直接维持 Dead，不再重复走阈值判定（驱逐是单向终态）。
    if (peer.evicted) {
        return AlertLevel::Dead;
    }

    // 时间维需要“曾收到过 ACK”才有意义：lastAckMs==0 表示从未收到，此时不能用 nowMs 去减
    // （否则 msLag 会等于一个巨大的“自纪元起的毫秒数”而误判失联）。hasTimeData 即为此守卫。
    const bool hasTimeData = peer.lastAckMs > 0;
    const qint64 msLag = hasTimeData ? (nowMs - peer.lastAckMs) : 0;

    // Hard thresholds → Dead（译：先查硬阈值——任一维命中即判“失联”，应驱逐）
    // 顺序：序号 → 字节 → 时间；用 >= 表示“达到或超过阈值即触发”。
    if (peer.lagSeq >= hardSeq_)
        return AlertLevel::Dead;
    if (peer.lagBytes >= hardBytes_)
        return AlertLevel::Dead;
    if (hasTimeData && msLag >= hardMs_)  // 仅在有时间数据时才用时间维判定
        return AlertLevel::Dead;

    // Soft thresholds → Lagging（译：未触硬阈值，再查软阈值——任一维命中即判“滞后”，仅告警）
    if (peer.lagSeq >= softSeq_)
        return AlertLevel::Lagging;
    if (peer.lagBytes >= softBytes_)
        return AlertLevel::Lagging;
    if (hasTimeData && msLag >= softMs_)
        return AlertLevel::Lagging;

    // 三维都在软阈值以内：健康。
    return AlertLevel::Healthy;
}

bool DeadPeerEvictor::evict(QSqlDatabase& db, const QString& peer, OutboundAckStore& ack,
                            QString* err) {
    // 步骤 1：标记该对端“需要走全量基线重建”（pending_baseline=true）。
    //   失败立即返回——后续步骤不再执行，保持状态一致（要么都改、要么都不改）。
    if (!ack.setPendingBaseline(db, peer, true, err))
        return false;

    // 步骤 2：把该对端在 __sync_outbound_ack 里的 acked_seq 置 -1（即“尚未确认任何东西”）。
    //   关键意义：changelog 截断水位 = 所有对端 acked_seq 的最小值；一个失联对端会把这个
    //   最小值死死压低，导致 changelog 永远无法截断而膨胀。这里不是简单删除该对端记录，而是
    //   置 -1 配合上面的 pending_baseline——既解除它对截断的牵制（它将走基线重来，不再依赖
    //   增量 changelog），又保留其行以便基线流程后续重新对齐。
    //   注意 WHERE peer = ? 会命中该对端在所有 (origin, epoch) 下的行，整体复位。
    QSqlQuery q(db);
    q.prepare(QStringLiteral("UPDATE __sync_outbound_ack SET acked_seq = -1 WHERE peer = ?"));
    q.addBindValue(QVariant(peer));
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();
        return false;
    }
    return true;
}

}  // namespace dbridge::sync
