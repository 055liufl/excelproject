#include "sync/conflict/RoutingTable.h"

// RoutingTable 的实现:无副作用的纯判定逻辑。详见头文件“两条路由规则”。

namespace dbridge::sync {

// 记录本地节点 id 与 peer 列表(覆盖式)。
void RoutingTable::configure(const QString& localNodeId, const QStringList& peers) {
    localNodeId_ = localNodeId;
    peers_ = peers;
}

// 路由判定:是否把(origin, originSeq)这条变更发给 peer。
bool RoutingTable::shouldRoute(const QString& peer, const QString& origin, qint64 originSeq,
                               qint64 peerAckedSeq) const {
    // 规则 1 —— anti-echo:绝不把变更转发回它的来源节点(否则形成回环)。
    if (origin == peer) {
        return false;
    }

    // 规则 2 —— 去重:仅当该 peer 尚未确认过这个序号(originSeq 超过其已 ACK 水位)时才发。
    return originSeq > peerAckedSeq;
}

}  // namespace dbridge::sync
