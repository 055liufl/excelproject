#pragma once
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

// ============================================================================
// InboxLedger.h — inbox「已处理台账」：保证每个 artifact 只被消费一次（幂等去重）
// ============================================================================
//
// 【这个文件是什么 / 为什么需要】
//   文件搬运层（UDP/rsync/共享盘…）是「至少一次」语义：同一个 artifact 文件可能
//   因重传、重扫、目录复制等原因「重复出现」在本节点的 inbox 里。如果每次出现都重新
//   应用一遍变更，就会造成重复写入/状态错乱。InboxLedger 在元数据表
//   __sync_inbox_ledger 里为每个 artifact 名字记一条状态，把「是否已处理」持久化下来，
//   从而强制实现「幂等消费」：一旦某 artifact 被标为 Consumed，无论它再来多少次都不再处理。
//   去重的键是「artifact 文件名」——文件名里已编码 origin/epoch/kind/seq/UUID，全局唯一
//   （命名契约见 SyncDDL.h），所以同名 == 同一笔东西。
//
// 【状态机（artifact 在台账中的生命周期）】
//        (未在台账)  → markSeen →  Seen（已发现、待消费）
//        Seen        → markConsumed → Consumed（已成功处理；终态，永不再处理）
//        Seen        → markCorrupt  → Corrupt（损坏/无法应用；终态，不自动重试）
//   状态只前进、不回退。InboxWatcher 扫描时先查 status()：Consumed/Corrupt 直接跳过。
//
// 【在管线中的位置】
//   InboxWatcher.scan() 发现 .ready → markSeen → 上层取出处理 → 成功则 markConsumed，
//   损坏则 markCorrupt。pendingSeen/stalePending 供「漏收/空洞检测」用。
//
// 【协作者】
//   · SyncDDL —— 建 __sync_inbox_ledger 表（init() 只校验可访问）；
//   · InboxWatcher —— 调 status()/markSeen() 决定是否把文件交给上层；
//   · 上层 apply 逻辑 —— 处理完后 markConsumed / markCorrupt；
//   · 空洞检测逻辑 —— 用 stalePending 触发 E_SYNC_GAP / 基线回退。
//
// 【线程模型】无可变成员（纯方法类），状态全在传入的 QSqlDatabase。
// ============================================================================

namespace dbridge::sync {

// LedgerStatus —— 某 artifact 在台账中的状态。
//   Seen     已发现、登记在册但尚未消费；
//   Consumed 已成功消费（终态，幂等去重的依据：再来直接跳过）；
//   Corrupt  已判定损坏/无法应用（终态，不自动重试，留待人工排查）；
//   Unknown  台账里根本没有这一条（= 从未见过，可放心首次处理）。
enum class LedgerStatus { Seen, Consumed, Corrupt, Unknown };

// CRUD wrapper for __sync_inbox_ledger.
// Enforces idempotent artifact consumption: once an artifact is Consumed it
// is never re-processed regardless of how many times it arrives.
// 译：__sync_inbox_ledger 的增删改查封装。
//   强制「幂等消费」：一旦某 artifact 被标记 Consumed，无论它再到达多少次都不会被重复处理。
class InboxLedger {
   public:
    // init —— 启动期自检：确认 __sync_inbox_ledger 表可访问（表由 SyncDDL 建，本类不建表）。
    bool init(QSqlDatabase& db, QString* err);

    // markSeen —— 记录某 artifact「首次被发现」。
    // 幂等：用 INSERT OR IGNORE，若该名字已在册则什么也不做（不会把 Consumed 退回 Seen）。
    // 同时写入 first_seen_ms 时间戳，供 stalePending 的“滞留多久”判定。
    bool markSeen(QSqlDatabase& db, const QString& artifactName, QString* err);

    // markConsumed —— 把状态推进到 Consumed（已成功处理），并记录 consumed_ms。
    // 这是「幂等去重」生效的关键一步：此后该 artifact 永不再被处理。
    bool markConsumed(QSqlDatabase& db, const QString& artifactName, QString* err);

    // markCorrupt —— 把状态标为 Corrupt（损坏/无法应用）。
    // will not be retried automatically（译：不会被自动重试）—— 终态，等待人工介入/排查。
    bool markCorrupt(QSqlDatabase& db, const QString& artifactName, QString* err);

    // status —— 查询某 artifact 当前状态；台账中无此条目时返回 Unknown。
    // 注意：本方法不返回错误码——查询失败也归并为 Unknown（保守地“当作没见过”，
    //   宁可让上层再处理一次也不假装已消费；真正的去重靠 Consumed 终态把守）。
    LedgerStatus status(QSqlDatabase& db, const QString& artifactName);

    // pendingSeen —— 返回所有 status == 'seen'（已发现但尚未消费）的 artifact 名字。
    // 用于了解“还有哪些在排队等处理”。
    QStringList pendingSeen(QSqlDatabase& db);

    // M-01: Return artifact names that have been 'seen' longer than gapTimeoutMs.
    // These represent gaps that should trigger E_SYNC_GAP / baseline fallback.
    // 译：M-01 —— 返回那些「停留在 seen 状态超过 gapTimeoutMs」的 artifact 名字。
    //   它们代表「迟迟无法消费的空洞」（多半是缺了前序变更而卡住），应触发 E_SYNC_GAP
    //   或退回基线（baseline fallback）来恢复——而不是无限期干等下去。
    QStringList stalePending(QSqlDatabase& db, qint64 gapTimeoutMs);
};

}  // namespace dbridge::sync
