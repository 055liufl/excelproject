#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QByteArray>
#include <QSqlDatabase>
#include <QString>

#include "RowWinnerStore.h"
#include <sqlite3.h>

// ============================================================================
// ChangesetApplier.h — 把「SQLite session changeset」应用到本地库的执行器
// ============================================================================
//
// 【在同步管线中的位置】
//   capture（捕获变更）→ payload（打包）→ transport（收发）→ **apply（本文件）**
//   → conflict（仲裁）→ peer（ACK）。
//   远端节点的写操作被 SQLite session 扩展捕获为二进制「changeset」，经传输层送达
//   本节点后，最终由本类负责「解码并应用到本地数据库」。它是 apply 阶段最底层、
//   最核心的一环：直接调用 sqlite3changeset_apply_v2() 把行级变更落到磁盘。
//
// 【核心难点：冲突仲裁 + 低 rank DELETE 保护】
//   多节点并发写同一行时，session 的 apply 会报「冲突」（本地当前值 ≠ changeset 里
//   记录的 old 值）。本类用 rank+seq+origin 三元组仲裁谁是胜者（高 rank 胜；rank 同
//   则高 seq 胜；再同则 origin 字符串大者胜——纯粹为「确定性」，与到达顺序无关）。
//   胜负结果持久化在 RowWinnerStore（__sync_row_winner 表）里。
//
//   一个微妙的难题：SQLite 没有可靠的公共 API 能「按行预过滤」一个 changeset，
//   所以无法在 apply 之前剔除「会输的 DELETE」。本类的策略是——先无条件 apply，
//   再在 **同一个事务内** 由 updateWinnersFromChangeset() 做「事后补救」：若发现某个
//   低 rank 的 DELETE 误删了高 rank 的胜者行，就用 RowWinnerStore 里缓存的行内容
//   （winningContent，JSON）把它重新插回去。补救失败 → apply() 返回 false → 调用方
//   必须回滚整个事务，从而保证「低 rank 的 DELETE 永远赢不了」(G-01 / FR-6)。
//
// 【协作者】
//   · RowWinnerStore   —— 持久化每个 (table, pkHash) 当前胜者的 rank/seq/内容。
//   · sqlite3 C API    —— 直接用底层句柄 + changeset 迭代器（Qt 的 QSqlQuery 拿不到）。
//   · SqlBuilder       —— 安全地给标识符加引号，拼接补救用的 INSERT/UPSERT SQL。
//   · CapturedWriteTemplate —— 上层「三分支写模板」的分支 A 调用本类。
//
// 【关键 SQLite 概念】
//   · changeset：session 录下的一段二进制差异，含每行的 op(INSERT/UPDATE/DELETE)、
//     old 值、new 值、PK 掩码。用 sqlite3changeset_start/next/op/old/new/pk 迭代。
//   · apply_v2 的两个回调：xFilter(filterCb) 决定「这张表收不收」；
//     xConflict(conflictCb) 决定「冲突这一行怎么处理」(REPLACE/OMIT/ABORT)。
//   · authoritative（权威/下行）：中心→边缘的下发被视为绝对真理，跳过一切仲裁，
//     冲突一律 REPLACE，也不更新 RowWinnerStore。
// ============================================================================

namespace dbridge::sync {

// ApplyOptions —— 一次 apply 的策略开关。
struct ApplyOptions {
    // 权威下行（authoritative down-link）：恒为 REPLACE，不查 RowWinnerStore。
    // 用于中心节点向边缘节点下发「绝对真理」的场景（边缘无条件接受）。
    bool authoritative = false;
    // M-01 fix：非权威 changeset 的冲突解决策略。
    // SourceWins（默认）= 进一步交给 rank/seq 仲裁；TargetWins/Manual = 本地行直接获胜。
    ConflictPolicy conflictPolicy = ConflictPolicy::SourceWins;
};

// ApplyOutcome —— 一次 apply 的统计结果（回填给调用方，用于进度与诊断）。
struct ApplyOutcome {
    int applied = 0;    // 实际落库的行数（含冲突里判胜后 REPLACE 的）
    int conflicts = 0;  // 触发冲突回调的行数（DATA/CONFLICT）
    int ignored = 0;    // 被 OMIT 跳过的行数（判负 / NOTFOUND / 策略否决）
    // 变基输出——仅当 authoritative=false 时由 apply_v2 填充。
    // 它是「本地胜出后，需要回送给对端让其变基(rebase)」的差异缓冲。
    QByteArray rebaseBuffer;
};

// ChangesetApplier —— 通过 sqlite3changeset_apply_v2 应用一段原始 SQLite changeset。
// 冲突解决委托给静态回调 conflictCb()：非权威路径会查 RowWinnerStore（G-01）做仲裁。
//
// 【生命周期/线程】本类无成员状态（除回调内的临时上下文 ConflictCtx），可被多次调用；
//   但每次 apply 必须在调用方已开启的写事务内进行，且不可跨线程并发用同一个 sqlite3 句柄。
class ChangesetApplier {
   public:
    // H-04 fix: syncTables limits which tables xFilter accepts; empty = accept all (test only).
    bool apply(sqlite3* h, QSqlDatabase& wconn, const QByteArray& changeset, const QString& origin,
               int originRank, qint64 originSeq, RowWinnerStore& winners, const ApplyOptions& opts,
               const QStringList& syncTables, ApplyOutcome* out, QString* err);

    // H-01 fix: shared allow-list predicate — same logic as filterCb() so all three paths
    // (xFilter, updateWinnersFromChangeset, extractMutations) reject the same tables.
    // Public so CapturedWriteTemplate can reuse the same predicate.
    static bool isAllowedSyncTable(const QString& table, const QStringList& syncTables);

   private:
    struct ConflictCtx {
        ChangesetApplier* self;
        sqlite3* h;
        QSqlDatabase* wconn;
        QString origin;
        int rank;
        qint64 seq;
        RowWinnerStore* winners;
        bool authoritative;
        ApplyOutcome* outcome;
        // H-04: tables allowed by xFilter; empty = accept all.
        const QStringList* syncTables = nullptr;
        // M-01 fix: conflict resolution policy for non-authoritative changesets.
        ConflictPolicy conflictPolicy = ConflictPolicy::SourceWins;
        // H-05 fix: column name cache for canonical pkHash computation.
        // Populated lazily per table via PRAGMA table_info inside conflictCb.
        QMap<QString, QStringList> colNameCache;
    };

    static int conflictCb(void* ctx, int conflict, sqlite3_changeset_iter* iter);
    static int filterCb(void* ctx, const char* tblName);  // H-04: xFilter

    // C-11/C-12: post-apply, update row_winner for INSERT/UPDATE and restore any high-rank row
    // erased by a dominated low-rank DELETE. Returns false (with *err set) if a required restore
    // failed — the caller MUST roll back the transaction so the low-rank DELETE never wins.
    // H-01 fix: syncTables is the same allow-list used by filterCb; tables not in the list
    // (and __sync_* tables) are skipped so row_winner is not polluted.
    bool updateWinnersFromChangeset(const QByteArray& changeset, const QString& origin, int rank,
                                    qint64 seq, RowWinnerStore& winners, QSqlDatabase& wconn,
                                    const QStringList& syncTables, QString* err);
};

}  // namespace dbridge::sync
