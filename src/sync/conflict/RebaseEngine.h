#pragma once
#include <QByteArray>
#include <QString>

// ============================================================================
// RebaseEngine — changeset 变基(把被冲突改写过的变更“重写”到正确基础上)
// ============================================================================
//
// 【背景:为什么需要 rebase】
//   应用对端 changeset 时,SQLite 的 sqlite3changeset_apply_v2 可能对部分行做出
//   “冲突处理决定”(例如按策略 OMIT 跳过某些更改)。这些决定会被记录进一个
//   “rebase 缓冲(rebase buffer)”。如果本节点之后还要把同一批变更**继续转发**
//   给其它节点,直接转发原始 changeset 会与本地的冲突处理结果不一致。
//   rebase 的作用,就是用本地的冲突处理结果(rebase buffer)去“改写”原始 changeset,
//   得到一个与本地实际落库结果一致、可安全继续传播的新 changeset。
//
// 【数据来源】
//   rebaseBuffer 来自 ChangesetApplier::ApplyOutcome.rebaseBuffer,由
//   sqlite3changeset_apply_v2 配合 SQLITE_CHANGESETAPPLY_NOSAVEPOINT 产出。
//
// 【底层 API】
//   封装 SQLite 的 sqlite3rebaser_create / _configure / _rebase / _delete 一族函数。
//   本类无状态,每次调用自管资源,线程安全。
// ============================================================================

namespace dbridge::sync {

// 用 sqlite3rebaser_* 把 changeset 变基到权威的 rebase buffer 之上。
// rebaseBuffer 来自 ChangesetApplier::ApplyOutcome.rebaseBuffer
// （由 sqlite3changeset_apply_v2 在 SQLITE_CHANGESETAPPLY_NOSAVEPOINT 模式下产出)。
class RebaseEngine {
   public:
    // 将 `changeset` 针对 `rebaseBuffer` 变基。
    // 成功:把变基后的数据写入 *rebased 并返回 true。
    // 失败:设置 *err 并返回 false。
    bool rebase(const QByteArray& rebaseBuffer, const QByteArray& changeset, QByteArray* rebased,
                QString* err);
};

}  // namespace dbridge::sync
