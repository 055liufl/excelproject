#pragma once
#include <QString>
#include <QStringList>

// ============================================================================
// RoutingTable — 转发路由表(防回环 / anti-echo)
// ============================================================================
//
// 【它解决什么问题】
//   中心节点收到来自某边缘节点的变更后,往往要“转发”给域内其它边缘节点。
//   若不加约束,会出现两类问题:
//     1) 回环(echo):把变更又发回它的来源节点 → 无意义且可能引发震荡;
//     2) 重复发送:把对端早已收到(已 ACK)的变更又发一遍 → 浪费且放大流量。
//   RoutingTable 用两条简单规则杜绝这两点,决定“某条变更是否该发给某个 peer”。
//
// 【两条路由规则】(见 shouldRoute)
//   1. origin != peer            —— 不把变更回送给它的来源节点(anti-echo)。
//   2. originSeq > peerAckedSeq   —— 仅当该 peer 尚未确认过这个序号时才发(去重)。
//
// 【与谁协作】
//   SyncWorker 在打包/转发 outbox 时,对每个候选 (peer, 变更) 调用 shouldRoute()
//   过滤;peerAckedSeq 来自 PeerStateStore 维护的“各 peer 已 ACK 水位”。
// ============================================================================

namespace dbridge::sync {

// 防回环路由:一条变更只有同时满足以下两点才发给某 peer:
//   1. origin != peer(不回送来源)
//   2. originSeq > peerAckedSeq(该 peer 尚未见过它)
class RoutingTable {
   public:
    // 配置本地节点 id 与可转发的 peer 列表。
    void configure(const QString& localNodeId, const QStringList& peers);

    // 判断来源为 `origin`、序号为 `originSeq` 的变更是否应转发给 `peer`。
    // peerAckedSeq 是该 peer 当前的“已确认水位”(它已收到并确认到的最大 originSeq)。
    // 返回 true 表示应当发送。
    bool shouldRoute(const QString& peer, const QString& origin, qint64 originSeq,
                     qint64 peerAckedSeq) const;

   private:
    QString localNodeId_;  // 本地节点 id(预留:用于更复杂的路由判定)
    QStringList peers_;    // 可转发的对端列表(预留:遍历目标时使用)
};

}  // namespace dbridge::sync
