#pragma once
#include <QHash>
#include <QString>

// ============================================================================
// ConflictArbiter — 多源冲突仲裁器（决定“同一行的多个版本,谁说了算”）
// ============================================================================
//
// 【它解决什么问题】
//   在「中心-边缘」多节点同步中,同一行数据可能被不同节点(origin)并发修改。
//   当一个变更要应用到本地、却发现本地当前值与该变更的“前值(old)”不一致时,
//   即发生 DATA 冲突。此时必须有一套**全局确定、与到达顺序无关**的规则来裁定
//   “保留谁的版本”,否则不同节点最终会收敛到不同结果(分叉)。
//
// 【裁定规则:规范序(canonical ordering)】
//   按 (rank 降序, originSeq 降序, originId 字典序降序) 比较两个候选版本:
//     1) rank 高者胜      —— rank 来自 SyncConfig 的 originPriority,全局唯一;
//                            体现“权威等级”(如中心节点 rank 最高)。
//     2) rank 相同则 seq 高者胜 —— seq 是每个 origin 自增的序号,越大越新。
//     3) 仍相同则 originId 字典序大者胜 —— 纯粹的稳定决胜手段(见 .cpp H-01)。
//   这套全序保证:无论变更以什么顺序到达,各节点独立仲裁都得到同一个胜者 →
//   全域最终一致(收敛)。
//
// 【与谁协作】
//   ChangesetApplier 在应用每一行遇到冲突时调用 beats() 判断胜负;
//   RowWinnerStore 记录每行“当前胜者”的 (origin, rank, seq) 以便后续比较。
//   rankMap_ 由 SyncWorker 用 SyncConfig::allRanks() 初始化。
// ============================================================================

namespace dbridge::sync {

// 多源冲突仲裁器:规范序(rank 降序, originSeq 降序)。rank 高者胜;rank 相同则 seq 高者胜。
class ConflictArbiter {
   public:
    // 设置“origin → rank”映射表(来自 SyncConfig 的 originPriority)。
    void setRankMap(const QHash<QString, int>& rankMap);

    // 查询某个 origin 的 rank;未配置的 origin 视为 0(最低)。
    int rankOf(const QString& origin) const;

    // 判断候选 a 是否“击败”候选 b(即 a 应成为胜者)。
    // 参数:aOrigin/aSeq = 候选 a 的来源节点与其序号;bOrigin/bSeq = 候选 b 的。
    // 返回:a 胜返回 true,否则 false。比较为严格全序,a==b 各字段时按 originId 决胜。
    bool beats(const QString& aOrigin, qint64 aSeq, const QString& bOrigin, qint64 bSeq) const;

   private:
    QHash<QString, int> rankMap_;  // origin → rank;rankOf() 查不到时返回 0
};

}  // namespace dbridge::sync
