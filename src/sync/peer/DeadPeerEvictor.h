#pragma once
#include <QSqlDatabase>
#include <QString>

#include "../anchor/OutboundAckStore.h"  // 驱逐时改写对端的 ACK 水位/基线标志

// ============================================================================
// DeadPeerEvictor.h — 对端「失联」判定与驱逐（E_SYNC_PEER_DEAD）
// ============================================================================
//
// 【这个文件是什么 / 解决什么问题】
//   广播式同步里，本节点会不断把变更发给各对端并等其回 ACK。若某个对端长时间不回 ACK、
//   或其「已确认进度」落后本端太多，就会拖累整个系统——最直接的危害是：changelog 的
//   截断水位取所有对端 acked_seq 的最小值（minAckedSeq），一个掉队的对端会让 changelog
//   永远无法截断而无限膨胀。DeadPeerEvictor 负责「健康度评估 + 驱逐」：先把对端按三个维度
//   的「软/硬阈值」判成 健康/滞后/失联 三档，对判定为失联（Dead）的对端执行「驱逐」，
//   令其退出正常增量同步、改走基线重建。对应错误码 E_SYNC_PEER_DEAD（见 Errors.h）。
//
// 【三维阈值：为什么用三个维度而不是单一超时】
//   单看时间会误杀「只是暂时安静但仍在跟进」的对端，也会漏判「一直在回 ACK 但进度严重
//   落后」的对端。故用三维联合判定，任一维触发硬阈值即判 Dead，任一维触发软阈值即判 Lagging：
//     · 序号滞后 lagSeq   —— 落后本端头部多少个序列号（衡量“漏了多少笔变更”）；
//     · 字节滞后 lagBytes —— 落后多少字节（衡量积压的数据量，序号少但单笔巨大也危险）；
//     · 时间滞后 msLag    —— 距上次收到该对端 ACK 过了多久（衡量“是否还活着”）。
//   软阈值 → Lagging（仅告警，对应 W_SYNC_PEER_LAGGING）；硬阈值 → Dead（触发驱逐）。
//
// 【驱逐做了什么 / 为什么这样做】
//   evict() 对失联对端：① 置 pending_baseline=true（标记“该对端需要走一次全量基线重建”
//   才能重新对齐）；② 把它的 acked_seq 归零/置 -1，让它不再压低 changelog 的截断水位，
//   从而解放 changelog 截断、避免无限膨胀。等对端恢复后会经基线流程重新加入。
//
// 【协作者】
//   · OutboundAckStore —— 持久化对端 ACK 水位与 pending_baseline 标志；
//   · SyncWorker —— 周期性地为每个对端构造 PeerState 调 evaluate()，对 Dead 者调 evict()。
//
// 【线程模型】configure/evaluate 不触 DB（纯计算，evaluate 为 const）；evict 走传入的
//   db（须为调用线程自己的连接）。阈值成员在 evaluate 期间应视为只读（configure 与
//   evaluate 不应并发）。
// ============================================================================

namespace dbridge::sync {

// DeadPeerEvictor —— 基于「三维阈值」的对端健康评估器与驱逐器。
class DeadPeerEvictor {
   public:
    // PeerState —— 评估单个对端所需的输入快照（由 SyncWorker 从各处汇总后填好传入）。
    struct PeerState {
        QString peer;  // 对端标识
        qint64 lastAckMs = 0;  // epoch ms of last ACK received（译：最近一次收到该对端 ACK 的 epoch
                               // 毫秒；0 表示从未收到过）
        qint64 lagSeq = 0;  // sequences behind local head（译：落后本端头部的序列号数）
        qint64 lagBytes = 0;  // bytes behind local head（译：落后本端头部的字节数）
        bool evicted = false;  // 是否已被驱逐过（若是，evaluate 直接判 Dead，不再重复评估）
    };

    // AlertLevel —— 健康度三档：健康 / 滞后（软阈值，仅告警）/ 失联（硬阈值，应驱逐）。
    enum class AlertLevel { Healthy, Lagging, Dead };

    // configure —— 设置六个阈值（三维各一对软/硬）。不传则用下方的默认值。
    // 参数顺序：序号软/硬、字节软/硬、时间(毫秒)软/硬。
    void configure(qint64 softSeq, qint64 hardSeq, qint64 softBytes, qint64 hardBytes,
                   qint64 softMs, qint64 hardMs);

    // evaluate —— 评估对端健康度，返回 Healthy/Lagging/Dead。
    // 【参数】peer 输入快照；nowMs 当前时刻（外部传入而非内部取，便于测试与一致快照）。
    // 【判定顺序】先看 evicted → 再逐维查硬阈值（命中即 Dead）→ 再逐维查软阈值（命中即 Lagging）
    //   → 全不命中为 Healthy。const：纯函数，不改任何状态。
    AlertLevel evaluate(const PeerState& peer, qint64 nowMs) const;

    // Evict a peer: mark pending_baseline=true, zero its acked_seq.
    // 译：驱逐一个对端——置 pending_baseline=true、并把它的 acked_seq 归零（实现里置 -1）。
    // 【副作用】改写 __sync_outbound_ack：解放 changelog 截断水位、并标记其需走基线重建。
    // 【返回】成功 true；任一 DB 写失败 false 并经 err 上报。
    bool evict(QSqlDatabase& db, const QString& peer, OutboundAckStore& ack, QString* err);

   private:
    // —— 六个阈值的默认值（可被 configure 覆盖）——
    qint64 softSeq_ = 10000;   // 序号滞后软阈值：落后 1 万个序号 → 告警
    qint64 hardSeq_ = 100000;  // 序号滞后硬阈值：落后 10 万个序号 → 判失联
    qint64 softBytes_ = 50LL * 1024 * 1024;   // 字节滞后软阈值：50 MiB
    qint64 hardBytes_ = 500LL * 1024 * 1024;  // 字节滞后硬阈值：500 MiB
    qint64 softMs_ = 300000;                  // 时间滞后软阈值：5 分钟无 ACK → 告警
    qint64 hardMs_ = 3600000;  // 时间滞后硬阈值：60 分钟无 ACK → 判失联
};

}  // namespace dbridge::sync
