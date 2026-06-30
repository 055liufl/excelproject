#pragma once
#include "dbridge/Export.h"
#include "dbridge/sync/SyncTypes.h"

#include <QStringList>

#include <algorithm>

// ============================================================================
// SyncConfig.h — 同步引擎的不可变配置对象（+ 校验型 Builder）
// ============================================================================
//
// 【这个文件是什么】
//   一次性传给 ISyncEngine::initialize() 的全部配置。SyncConfig 故意做成“只读值对象”：
//     · 私有构造 + 全部成员私有，外部只能通过友元 SyncConfig::Builder 链式设置后
//       调用 build() 构造；
//     · build() 内做集中、严格的校验，只有全部通过才把 valid_ 置 true。
//       这样“非法配置根本造不出一个 isValid()==true 的对象”，把错误挡在最前面。
//
// 【为什么用 Builder 模式】
//   配置项多达二十余个、且彼此有约束（如 soft<=hard、rank 全局唯一、Edge 必须有
//   centerNode）。Builder 让调用方按需链式设置可选项、用默认值兜底其余项，并把所有
//   跨字段一致性检查收口在 build() 一处，便于阅读与维护。
//
// 【配置项分组速览】
//   · 拓扑身份：role / nodeId / centerNodeId / peerNodes
//   · 库与目录：sqlitePath / syncTables / outboxDir / inboxDir / quarantineDir
//   · 冲突仲裁：conflictPolicy / originRank（rank 优先级表）
//   · 对端滞后阈值（软=告警 / 硬=驱逐）：seq / bytes / ms 三个维度各一对
//   · 发件箱配额：outboxMaxBytesPerPeer / outboxMaxArtifactsPerPeer
//   · 时序参数：ackMaxDelayMs / broadcastIntervalMs / broadcastThreshold / gapTimeoutMs
//   · 体量与切分：baselineSizeWarnBytes / maxSelectionSize / pushChunkBudgetBytes
//   · 表结构与保留：schemaVersion / verifySchemaFingerprint / changelogRetention
//   · 行为开关：autoSyncAfterImport / consistencyCacheDurable
//
// 注：getter 旁的“默认值”均见下方私有成员的初始化器；约束规则见 Builder::build()。
// ============================================================================

namespace dbridge::sync {

// SyncConfig —— 同步引擎的只读配置快照。线程安全：构造后不可变，可被多线程共享读取。
class DBRIDGE_EXPORT SyncConfig {
   public:
    class Builder;

    // ── 拓扑身份 ──────────────────────────────────────────────────────────
    NodeRole role() const {  // 本节点角色：Center（中心）/ Edge（边缘）。默认 Edge。
        return role_;
    }
    QString nodeId() const {  // 本节点唯一 id（必填）；也是本地产生变更的 origin。
        return nodeId_;
    }
    QString centerNodeId() const {  // 中心节点 id；role==Edge 时必填（边缘只与中心交换）。
        return centerNodeId_;
    }
    QStringList peerNodes() const {  // 对端节点列表（不含自身，不含重复，不含空串）。
        return peerNodes_;
    }

    // ── 库与目录 ──────────────────────────────────────────────────────────
    QString sqlitePath() const {  // 被同步的 SQLite 库文件路径（必填）。
        return sqlitePath_;
    }
    QStringList syncTables() const {  // 纳入同步的表清单；留空 = 全部用户表（由 worker 展开）。
        return syncTables_;
    }
    QString outboxDir() const {  // 发件箱目录：本地产生的 artifact 写到这里待投递（必填）。
        return outboxDir_;
    }
    QString inboxDir() const {  // 收件箱目录：从对端收到的 artifact 落在这里待应用（必填）。
        return inboxDir_;
    }
    QString quarantineDir() const {  // 隔离区目录：损坏/暂不可应用（如有空洞）的负载暂存于此。
        return quarantineDir_;
    }

    // ── 冲突仲裁 ──────────────────────────────────────────────────────────
    ConflictPolicy conflictPolicy() const {  // 默认 SourceWins（来源胜）。见 SyncTypes.h。
        return conflictPolicy_;
    }
    // 取某 origin 的仲裁优先级；未显式配置者默认 0。冲突时先比 rank、再比 seq。
    int originRank(const QString& origin) const {
        return originRank_.value(origin, 0);
    }
    QHash<QString, int> allRanks() const {  // 取全部已配置的 origin→rank 映射。
        return originRank_;
    }

    // ── 对端滞后阈值（soft=仅告警 lagging；hard=驱逐 evicted，停止为其堆 outbox）──
    // 三个维度各一对（soft<=hard，由 build() 强制）：序号差 / 字节差 / 时间差(毫秒)。
    qint64 peerLagSoftSeq() const {  // 软：未确认序号差超过此值 → 标记 lagging。默认 1e4。
        return peerLagSoftSeq_;
    }
    qint64 peerLagHardSeq() const {  // 硬：序号差超过此值 → 驱逐对端。默认 1e5。
        return peerLagHardSeq_;
    }
    qint64 peerLagSoftBytes() const {  // 软：积压字节超过此值 → lagging。默认 50 MiB。
        return peerLagSoftBytes_;
    }
    qint64 peerLagHardBytes() const {  // 硬：积压字节超过此值 → 驱逐。默认 500 MiB。
        return peerLagHardBytes_;
    }
    qint64 peerLagSoftMs() const {  // 软：距上次 ACK 超过此毫秒 → lagging。默认 5 分钟。
        return peerLagSoftMs_;
    }
    qint64 peerLagHardMs() const {  // 硬：距上次 ACK 超过此毫秒 → 驱逐。默认 1 小时。
        return peerLagHardMs_;
    }

    // ── 发件箱配额（防止某个慢/失联对端把 outbox 撑爆）────────────────────────
    qint64 outboxMaxBytesPerPeer() const {  // 单对端 outbox 字节上限。默认 1 GiB。须 >0。
        return outboxMaxBytesPerPeer_;
    }
    int outboxMaxArtifactsPerPeer() const {  // 单对端 outbox 文件个数上限。默认 1e4。须 >0。
        return outboxMaxArtifactsPerPeer_;
    }

    // ── 时序参数 ──────────────────────────────────────────────────────────
    qint64 ackMaxDelayMs() const {  // 等待对端 ACK 的最长毫秒；超时 → E_SYNC_ACK_TIMEOUT。默认 5s。
        return ackMaxDelayMs_;
    }
    qint64 baselineSizeWarnBytes()
        const {  // 基线体积告警阈值（W_SYNC_BASELINE_LARGE）。默认 100 MiB。
        return baselineSizeWarnBytes_;
    }
    qint64 schemaVersion() const {  // 本端声明的表结构版本号（>=1），随负载传播做版本闸。默认 1。
        return schemaVersion_;
    }
    int changelogRetention() const {  // __sync_changelog 保留条数（超出可裁剪）。默认 1e5；0=不限。
        return changelogRetention_;
    }
    bool verifySchemaFingerprint() const {  // 应用前是否校验两端表结构指纹一致。默认 true。
        return verifySchemaFingerprint_;
    }
    bool autoSyncAfterImport() const {  // ETL 导入后是否自动触发一次 sync()。默认 false。
        return autoSyncAfterImport_;
    }
    qint64 broadcastIntervalMs() const {  // 后台广播轮询间隔（毫秒）。默认 5s。须 >0。
        return broadcastIntervalMs_;
    }
    qint64 broadcastThreshold() const {  // 累积多少条未广播变更即触发一次广播。默认 100。须 >0。
        return broadcastThreshold_;
    }
    qint64 maxSelectionSize()
        const {  // 选择性推送一次最多行数；超出 → E_SYNC_SELECTION_TOO_LARGE。默认 1e5。
        return maxSelectionSize_;
    }
    qint64 pushChunkBudgetBytes() const {  // 选择性推送单分片字节预算（超出即切片）。默认 2 MiB。
        return pushChunkBudgetBytes_;
    }
    bool consistencyCacheDurable()
        const {  // 一致性缓存是否持久化（true=落库，跨重启复用）。默认 true。
        return consistencyCacheDurable_;
    }
    // M-1 fix：序号空洞等待超时现在可经 SyncConfig 配置（旧版硬编码 30s）。
    // 含义：检测到 origin 序号空洞后，最多等待此毫秒补齐，仍不齐 → E_SYNC_GAP。默认 30s。
    qint64 gapTimeoutMs() const {
        return gapTimeoutMs_;
    }

    // 配置是否有效。仅当 Builder::build() 全部校验通过时为 true；默认构造/校验失败均为 false。
    bool isValid() const {
        return valid_;
    }

   private:
    friend class Builder;    // 只有 Builder 能写入私有成员并最终置 valid_。
    SyncConfig() = default;  // 私有构造：外部无法绕过 Builder 直接造对象。

    // ↓ 下列成员的“= 值”即各配置项的默认值（getter 注释中引用的“默认”）。
    bool valid_ = false;
    NodeRole role_ = NodeRole::Edge;
    QString nodeId_;
    QString centerNodeId_;
    QStringList peerNodes_;
    QString sqlitePath_;
    QStringList syncTables_;
    QString outboxDir_;
    QString inboxDir_;
    QString quarantineDir_;
    ConflictPolicy conflictPolicy_ = ConflictPolicy::SourceWins;
    QHash<QString, int> originRank_;
    qint64 peerLagSoftSeq_ = 10000;
    qint64 peerLagHardSeq_ = 100000;
    qint64 peerLagSoftBytes_ = 50 * 1024 * 1024;         // 50 MiB
    qint64 peerLagHardBytes_ = 500 * 1024 * 1024;        // 500 MiB
    qint64 peerLagSoftMs_ = 300000;                      // 5 分钟
    qint64 peerLagHardMs_ = 3600000;                     // 1 小时
    qint64 outboxMaxBytesPerPeer_ = 1024 * 1024 * 1024;  // 1 GiB
    int outboxMaxArtifactsPerPeer_ = 10000;
    qint64 ackMaxDelayMs_ = 5000;
    qint64 baselineSizeWarnBytes_ = 100 * 1024 * 1024;  // 100 MiB
    qint64 schemaVersion_ = 1;
    int changelogRetention_ = 100000;
    bool verifySchemaFingerprint_ = true;
    bool autoSyncAfterImport_ = false;
    qint64 broadcastIntervalMs_ = 5000;
    qint64 broadcastThreshold_ = 100;
    qint64 maxSelectionSize_ = 100000;
    qint64 pushChunkBudgetBytes_ = 2 * 1024 * 1024;  // 2 MiB
    bool consistencyCacheDurable_ = true;
    qint64 gapTimeoutMs_ =
        30 * 1000;  // M-1: default 30 s; configurable via Builder（默认 30s，可由 Builder 配置）
};

// SyncConfig::Builder —— 链式配置 + 一处校验的构造器。
// 用法：SyncConfig cfg = SyncConfig::Builder().nodeId("A").database(path)
//                          .outboxDir(o).inboxDir(i)....build(&err);
// 每个 setter 返回 *this 以便链式书写；真正的合法性判定全部集中在 build()。
class DBRIDGE_EXPORT SyncConfig::Builder {
   public:
    Builder() = default;
    Builder& nodeId(const QString& id) {  // 设本节点 id（必填，build() 强制非空）。
        cfg_.nodeId_ = id;
        return *this;
    }
    Builder& role(NodeRole r) {  // 设角色 Center/Edge（默认 Edge）。
        cfg_.role_ = r;
        return *this;
    }
    Builder& centerNodeId(const QString& id) {  // 设中心节点 id（Edge 角色必填）。
        cfg_.centerNodeId_ = id;
        return *this;
    }
    Builder& addPeerNode(const QString& id) {  // 追加一个对端（可多次调用；不得含自身/空/重复）。
        cfg_.peerNodes_ << id;
        return *this;
    }
    Builder& database(const QString& path) {  // 设 SQLite 库路径（必填）。
        cfg_.sqlitePath_ = path;
        return *this;
    }
    Builder& syncTables(const QStringList& t) {  // 设同步表清单（留空 = 全部用户表）。
        cfg_.syncTables_ = t;
        return *this;
    }
    Builder& outboxDir(const QString& d) {  // 设发件箱目录（必填）。
        cfg_.outboxDir_ = d;
        return *this;
    }
    Builder& inboxDir(const QString& d) {  // 设收件箱目录（必填）。
        cfg_.inboxDir_ = d;
        return *this;
    }
    Builder& quarantineDir(const QString& d) {  // 设隔离区目录（损坏/暂不可应用负载暂存处）。
        cfg_.quarantineDir_ = d;
        return *this;
    }
    Builder& conflictPolicy(ConflictPolicy p) {  // 设冲突策略（默认 SourceWins）。
        cfg_.conflictPolicy_ = p;
        return *this;
    }
    // 为某 origin 设仲裁优先级 rank。build() 强制“所有显式配置的 rank 全局唯一”，
    // 否则 seq 也打平时冲突结果将不确定。
    Builder& originPriority(const QString& origin, int rank) {
        cfg_.originRank_[origin] = rank;
        return *this;
    }
    Builder& peerLagSoftLimit(qint64 s) {
        cfg_.peerLagSoftSeq_ = s;
        return *this;
    }
    Builder& peerLagHardLimit(qint64 s) {
        cfg_.peerLagHardSeq_ = s;
        return *this;
    }
    Builder& peerLagSoftBytes(qint64 b) {
        cfg_.peerLagSoftBytes_ = b;
        return *this;
    }
    Builder& peerLagHardBytes(qint64 b) {
        cfg_.peerLagHardBytes_ = b;
        return *this;
    }
    Builder& peerLagSoftMs(qint64 ms) {
        cfg_.peerLagSoftMs_ = ms;
        return *this;
    }
    Builder& peerLagHardMs(qint64 ms) {
        cfg_.peerLagHardMs_ = ms;
        return *this;
    }
    Builder& outboxMaxBytesPerPeer(qint64 b) {
        cfg_.outboxMaxBytesPerPeer_ = b;
        return *this;
    }
    Builder& outboxMaxArtifactsPerPeer(int n) {
        cfg_.outboxMaxArtifactsPerPeer_ = n;
        return *this;
    }
    Builder& ackMaxDelayMs(qint64 ms) {
        cfg_.ackMaxDelayMs_ = ms;
        return *this;
    }
    Builder& baselineSizeWarnBytes(qint64 b) {
        cfg_.baselineSizeWarnBytes_ = b;
        return *this;
    }
    Builder& schemaVersion(qint64 v) {
        cfg_.schemaVersion_ = v;
        return *this;
    }
    Builder& changelogRetention(int n) {
        cfg_.changelogRetention_ = n;
        return *this;
    }
    Builder& verifySchemaFingerprint(bool on) {
        cfg_.verifySchemaFingerprint_ = on;
        return *this;
    }
    Builder& autoSyncAfterImport(bool on) {
        cfg_.autoSyncAfterImport_ = on;
        return *this;
    }
    Builder& broadcastIntervalMs(qint64 ms) {
        cfg_.broadcastIntervalMs_ = ms;
        return *this;
    }
    Builder& broadcastThreshold(qint64 n) {
        cfg_.broadcastThreshold_ = n;
        return *this;
    }
    Builder& maxSelectionSize(qint64 n) {
        cfg_.maxSelectionSize_ = n;
        return *this;
    }
    Builder& pushChunkBudgetBytes(qint64 b) {
        cfg_.pushChunkBudgetBytes_ = b;
        return *this;
    }
    Builder& consistencyCacheDurable(bool on) {
        cfg_.consistencyCacheDurable_ = on;
        return *this;
    }
    Builder& gapTimeoutMs(qint64 ms) {
        cfg_.gapTimeoutMs_ = ms;
        return *this;
    }

    SyncConfig build(QString* err = nullptr) {
        if (cfg_.nodeId_.isEmpty()) {
            if (err)
                *err = QStringLiteral("nodeId is required");
            return {};
        }
        if (cfg_.sqlitePath_.isEmpty()) {
            if (err)
                *err = QStringLiteral("database path is required");
            return {};
        }
        if (cfg_.outboxDir_.isEmpty() || cfg_.inboxDir_.isEmpty()) {
            if (err)
                *err = QStringLiteral("outboxDir and inboxDir are required");
            return {};
        }
        // M-01 fix: additional validation.
        // peerNodes must not contain the local nodeId.
        if (cfg_.peerNodes_.contains(cfg_.nodeId_)) {
            if (err)
                *err = QStringLiteral("nodeId must not appear in peerNodes");
            return {};
        }
        // peerNodes must not contain empty strings.
        for (const QString& p : cfg_.peerNodes_) {
            if (p.isEmpty()) {
                if (err)
                    *err = QStringLiteral("peerNodes must not contain empty strings");
                return {};
            }
        }
        // peerNodes must not contain duplicates.
        {
            QStringList sorted = cfg_.peerNodes_;
            std::sort(sorted.begin(), sorted.end());
            for (int i = 1; i < sorted.size(); ++i) {
                if (sorted[i] == sorted[i - 1]) {
                    if (err)
                        *err = QStringLiteral("peerNodes contains duplicate entry '%1'")
                                   .arg(sorted[i]);
                    return {};
                }
            }
        }
        // Edge nodes must specify a centerNode.
        if (cfg_.role_ == NodeRole::Edge && cfg_.centerNodeId_.isEmpty()) {
            if (err)
                *err = QStringLiteral("centerNodeId is required for Edge role");
            return {};
        }
        // centerNode must not appear in peerNodes (for non-Edge roles).
        if (cfg_.role_ != NodeRole::Edge && !cfg_.centerNodeId_.isEmpty() &&
            cfg_.peerNodes_.contains(cfg_.centerNodeId_)) {
            if (err)
                *err = QStringLiteral("centerNodeId must not appear in peerNodes");
            return {};
        }
        // outboxMaxBytesPerPeer must be positive.
        if (cfg_.outboxMaxBytesPerPeer_ <= 0) {
            if (err)
                *err = QStringLiteral("outboxMaxBytesPerPeer must be positive");
            return {};
        }
        // gapTimeoutMs must be positive.
        if (cfg_.gapTimeoutMs_ <= 0) {
            if (err)
                *err = QStringLiteral("gapTimeoutMs must be positive");
            return {};
        }
        if (cfg_.ackMaxDelayMs_ <= 0) {
            if (err)
                *err = QStringLiteral("ackMaxDelayMs must be positive");
            return {};
        }
        if (cfg_.broadcastIntervalMs_ <= 0) {
            if (err)
                *err = QStringLiteral("broadcastIntervalMs must be positive");
            return {};
        }
        if (cfg_.broadcastThreshold_ <= 0) {
            if (err)
                *err = QStringLiteral("broadcastThreshold must be positive");
            return {};
        }
        if (cfg_.maxSelectionSize_ <= 0) {
            if (err)
                *err = QStringLiteral("maxSelectionSize must be positive");
            return {};
        }
        if (cfg_.pushChunkBudgetBytes_ <= 0) {
            if (err)
                *err = QStringLiteral("pushChunkBudgetBytes must be positive");
            return {};
        }
        if (cfg_.peerLagSoftMs_ <= 0) {
            if (err)
                *err = QStringLiteral("peerLagSoftMs must be positive");
            return {};
        }
        if (cfg_.schemaVersion_ < 1) {
            if (err)
                *err = QStringLiteral("schemaVersion must be >= 1");
            return {};
        }
        // M-03 fix: validate each lag threshold is positive before checking soft <= hard.
        // Non-positive values would be silently treated as disabled/zero and cause incorrect
        // peer eviction or lag logic.
        if (cfg_.peerLagSoftSeq_ <= 0) {
            if (err)
                *err = QStringLiteral("peerLagSoftSeq must be > 0");
            return {};
        }
        if (cfg_.peerLagHardSeq_ <= 0) {
            if (err)
                *err = QStringLiteral("peerLagHardSeq must be > 0");
            return {};
        }
        if (cfg_.peerLagSoftBytes_ <= 0) {
            if (err)
                *err = QStringLiteral("peerLagSoftBytes must be > 0");
            return {};
        }
        if (cfg_.peerLagHardBytes_ <= 0) {
            if (err)
                *err = QStringLiteral("peerLagHardBytes must be > 0");
            return {};
        }
        if (cfg_.peerLagHardMs_ <= 0) {
            if (err)
                *err = QStringLiteral("peerLagHardMs must be > 0");
            return {};
        }
        // M-01 fix: validate soft <= hard threshold relationships and positive-only fields.
        if (cfg_.peerLagSoftSeq_ > cfg_.peerLagHardSeq_) {
            if (err)
                *err = QStringLiteral("peerLagSoftSeq (%1) must be <= peerLagHardSeq (%2)")
                           .arg(cfg_.peerLagSoftSeq_)
                           .arg(cfg_.peerLagHardSeq_);
            return {};
        }
        if (cfg_.peerLagSoftBytes_ > cfg_.peerLagHardBytes_) {
            if (err)
                *err = QStringLiteral("peerLagSoftBytes (%1) must be <= peerLagHardBytes (%2)")
                           .arg(cfg_.peerLagSoftBytes_)
                           .arg(cfg_.peerLagHardBytes_);
            return {};
        }
        if (cfg_.peerLagSoftMs_ > cfg_.peerLagHardMs_) {
            if (err)
                *err = QStringLiteral("peerLagSoftMs (%1) must be <= peerLagHardMs (%2)")
                           .arg(cfg_.peerLagSoftMs_)
                           .arg(cfg_.peerLagHardMs_);
            return {};
        }
        if (cfg_.outboxMaxArtifactsPerPeer_ <= 0) {
            if (err)
                *err = QStringLiteral("outboxMaxArtifactsPerPeer must be > 0");
            return {};
        }
        if (cfg_.baselineSizeWarnBytes_ != 0 && cfg_.baselineSizeWarnBytes_ <= 0) {
            if (err)
                *err = QStringLiteral("baselineSizeWarnBytes must be > 0 when set");
            return {};
        }
        if (cfg_.changelogRetention_ != 0 && cfg_.changelogRetention_ <= 0) {
            if (err)
                *err = QStringLiteral("changelogRetention must be > 0 when set");
            return {};
        }
        // H-01 fix: validate that all explicitly configured ranks are globally unique
        // across all participating nodes (nodeId + peerNodes). Two origins with the same
        // rank would make conflict resolution non-deterministic when seq also ties.
        {
            // Collect all node ids that have an explicitly configured rank.
            QList<int> ranks;
            for (auto it = cfg_.originRank_.begin(); it != cfg_.originRank_.end(); ++it) {
                ranks.append(it.value());
            }
            // Sort and find duplicates.
            std::sort(ranks.begin(), ranks.end());
            for (int i = 1; i < ranks.size(); ++i) {
                if (ranks[i] == ranks[i - 1]) {
                    if (err)
                        *err = QStringLiteral(
                                   "originRank contains duplicate rank value %1; "
                                   "every participating origin must have a unique rank")
                                   .arg(ranks[i]);
                    return {};
                }
            }
        }
        cfg_.valid_ = true;
        return cfg_;
    }

   private:
    SyncConfig cfg_;
};

}  // namespace dbridge::sync
