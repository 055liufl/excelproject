#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QByteArray>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "../WriteTxn.h"
#include "../capture/ChangelogStore.h"
#include "../capture/SessionRecorder.h"
#include "../schema/SchemaGuard.h"
#include "../schema/TableStateStore.h"
#include "AppliedVectorStore.h"
#include "ChangesetApplier.h"
#include "RowWinnerStore.h"
#include "UpsertExecutor.h"
#include <sqlite3.h>

// ============================================================================
// CapturedWriteTemplate.h — “被捕获的写”统一模板（apply 写库 + 变更捕获的总枢纽）
// ============================================================================
//
// 【这个类是什么 / 解决什么问题】
//   dbridge 同步子系统里，几乎所有「会改动本地库」的路径最终都收敛到这里。它把
//   “一次写”抽象成统一模板 execute()，内部按写的来源分成三条分支（设计编号 G-03）：
//
//     · 分支 A —— InboundChangeset（收到对端的原始 changeset 二进制）：
//         直接把对端录好的 changeset 整块 apply 到本地库（走 ChangesetApplier，
//         不重新经 session 捕获——因为我们已经持有原始 blob，原样转存即可）。
//
//     · 分支 B —— InboundSelectionPush（收到对端“选择性推送”的一批行）：
//         对端发来的是行级 RowMutation（而非 changeset blob），本地需要重新执行这些
//         写，并用 session 记录器把它们重新捕获成本地的 changeset。
//
//     · 分支 C —— LocalWrite（本节点自己发起的写）：
//         比对会话 save()、引擎 write() 等都走这里——把一批 RowMutation 执行进本地库，
//         同时被 session 捕获、封存进 __sync_changelog，从而可被打包广播给对端
//         （供其向上游/全域继续传播）。
//   B 与 C 的落库+捕获机制完全一致，仅“元信息从何而来”不同（见 branchBC），故合用一个
//   私有方法 branchBC()；分支 A 单独由 branchA() 处理。
//
// 【“被捕获的写”是什么意思——本类名字的由来】
//   普通直写 SQL 会绕过同步、要么被门控拦下、要么造成漏同步（W_SYNC_UNTRACKED_CHANGE）。
//   本模板保证：写在落本地库的同时，被 SQLite session 扩展记录进 changeset 并封存进
//   changelog——即“既落库、又被捕获”。这正是“CapturedWriteTemplate（被捕获的写模板）”
//   的含义，也是它处于 apply 与捕获两条链路交汇点的原因。
//
// 【apply “三件套”的原子性（贯穿全文件的核心不变量）】
//   一次成功的写必须让以下三者在同一个写事务 (WriteTxn) 内一起提交、或一起回滚：
//     ① 业务数据本身（被 UPSERT / changeset apply 进目标表）；
//     ② __sync_changelog（封存 changeset，供日后广播）/ __sync_applied_vector（推进水位）；
//     ③ __sync_table_state（按行的 before/after 哈希维护各表校验和，供跨节点比对）。
//   任何一步失败都立即 rollback 整个事务。否则三者会“撕裂”——例如 table_state 落后于
//   实际数据，会让各节点算出的校验和悄悄发散（diverge），破坏一致性判断。
//
// 【协作者一览（构造函数注入的引用，均不持有所有权）】
//   · AppliedVectorStore  av_      —— 每个 origin 的“连续序列”水位（去重/补洞判定）。
//   · RowWinnerStore      rw_      —— 行级冲突仲裁的“胜者账本”（rank/seq 谁赢）。
//   · TableStateStore     ts_      —— 各表校验和（增量维护 before/after 行哈希）。
//   · ChangelogStore      clog_    —— __sync_changelog 读写（封存/转存 changeset）。
//   · SessionRecorder     rec_     —— 用 SQLite session 捕获本地写（分支 B/C 专用）。
//   · SchemaGuard         guard_   —— 校验来件的表结构版本/指纹是否与本地一致。
//   · ChangesetApplier    applier_ —— 分支 A 应用原始 changeset（含冲突回调仲裁）。
//   · UpsertExecutor（局部构造）   —— 分支 B/C 逐行 UPSERT 落库。
//
// 【线程】非线程安全：所有操作绑定到同一个写连接 wconn_ / 原生句柄 h_，必须在持有该
//   写连接的单一线程上串行使用（SQLite 连接本身不可跨线程并发）。
//
// 【错误码】机器可读码见 include/dbridge/Errors.h（E_SYNC_*）；本文件还会用到若干
//   “内部码”（如 "TXN_BEGIN" / "GAP_PENDING" / "SEAL"，非 Errors.h 词汇），由上层
//   processArtifact 等流程消费而非对外暴露，相应处均有行内说明。
// ============================================================================

namespace dbridge::sync {

// WriteKind —— 一次写的“来源/种类”，决定 execute() 路由到哪条分支。
//   InboundChangeset     收到对端原始 changeset 二进制       → 分支 A（branchA）
//   InboundSelectionPush 收到对端“选择性推送”的行级变更      → 分支 B（branchBC）
//   LocalWrite           本节点自己发起的写（默认值）         → 分支 C（branchBC）
enum class WriteKind { InboundChangeset, InboundSelectionPush, LocalWrite };

// WriteParams —— 一次写的全部输入参数（不同分支只用其中相关子集，见各字段分组）。
struct WriteParams {
    WriteKind kind = WriteKind::LocalWrite;  // 写的种类（默认本地写）；决定路由分支

    // ── 同步元信息（分支 A 与分支 B 入站时需要）─────────────────────────────
    QString origin;    // 变更的来源节点 id（这批改动最初由谁产生）
    qint64 epoch = 0;  // 流纪元（origin 的同步流代号；用于区分重置后的新流）
    qint64 seq = 0;    // 来源序号（该 origin 流内的单调序号；连续性靠它判定）
    qint64 schemaVer = 0;  // 来件携带的表结构版本号
    QString schemaFp;  // 来件携带的表结构指纹（与本地比对，防结构不一致误写）
    int originRank = 0;  // 来源节点的“等级/权重”（冲突仲裁时 rank 高者优先）

    // ── 仅 SelectionPush（选择性推送）相关 ──────────────────────────────────
    QString pushId;    // 本次推送的唯一 id（同一推送被切成多个 chunk）
    int chunkSeq = 0;  // 本 chunk 在该推送内的序号（用于分片幂等记账）
    QString checksum;  // 本 chunk 的校验和（重投同 chunk 时比对，防错投/损坏）

    // ── 仅分支 A（InboundChangeset）相关 ────────────────────────────────────
    QByteArray changesetBlob;  // 对端录好的原始 changeset 二进制（整块转存+应用）
    // H-01 fix: set true when the changeset comes from an authoritative source (center→edge).
    // H-01 修复：当 changeset 来自“权威来源”（中心→边缘下行）时置 true——边缘节点
    //   将无条件接受、冲突一律 REPLACE，不再做 rank/seq 仲裁（见 ChangesetApplier）。
    bool authoritative = false;
    // M-01 fix: conflict resolution policy for non-authoritative applies.
    // M-01 修复：非权威 apply 的冲突解决策略（SourceWins/TargetWins/Manual），
    //   透传给 conflictCb 决定撞行时谁胜。
    ConflictPolicy conflictPolicy = ConflictPolicy::SourceWins;

    // ── 仅分支 B/C（SelectionPush / LocalWrite）相关 ────────────────────────
    QList<RowMutation> mutations;  // 待执行的一批行级变更（逐条 UPSERT 落库并被捕获）

    // Tables to track with SessionRecorder (B/C only).
    // 分支 B/C 专用：交给 SessionRecorder 捕获的“同步表白名单”（canonicalSyncTables）。
    //   只有名列其中的表的改动才会被纳入 changeset / table_state，__sync_* 元表与
    //   非同步表一律排除，避免污染校验和与广播内容。
    QStringList syncTables;
};

// WriteResult —— 一次写的结果回执（无论成功失败都填 ok 与可能的错误码/文本）。
struct WriteResult {
    bool ok = false;  // 整体成功与否（含“幂等跳过”也算 true）
    qint64 localChangelogSeq = 0;  // 本次封存进 changelog 所得的本地 local_seq（0=无可广播变更）
    QList<TableMutation> tableMutations;  // 本次涉及的逐行表变更（已写入 table_state 的那批）
    QString errorCode;  // 失败时的机器可读码（Errors.h 码或内部码）
    QString errorMsg;   // 失败时的人类可读详情（用于日志/诊断）
    ApplyOutcome
        applyOutcome;  // Branch A only —— 仅分支 A 填：apply 的统计（applied/conflicts/ignored）
    QString
        tableStateStaleSince;  // M-04: non-empty when table_state update failed (non-fatal)
                               // M-04：当 table_state 更新失败但被判为“非致命”时非空，
                               //   记录其“自何时起陈旧”，供上层延后修复（本文件当前路径多为致命回滚）。
};

// Three-branch write template implementing G-03.
// All writes go through execute() which routes to branchA / branchBC.
// 实现设计编号 G-03 的“三分支写模板”。
// 所有写都经统一入口 execute() 进入，再按 kind 路由到 branchA / branchBC。
class CapturedWriteTemplate {
   public:
    // 构造：注入全部协作者（均为引用/裸句柄，本类不拥有其生命周期）与本节点身份。
    // 参数：
    //   wconn       —— 写连接（QSqlDatabase，须已 open）；所有 SQL 经它执行。
    //   h           —— 同一连接的底层 sqlite3* 原生句柄（session/changeset C API 需要）。
    //   av/rw/ts/clog/rec/guard/applier —— 见文件头“协作者一览”。
    //   nodeId      —— 本节点 id（分支 C 本地写时充当 origin）。
    //   streamEpoch —— 本节点当前流纪元（分支 C 写入 changelog 的 epoch）。
    //   schemaFp/schemaVer —— 本地表结构指纹/版本（分支 C 封存元信息时使用）。
    CapturedWriteTemplate(QSqlDatabase& wconn, sqlite3* h, AppliedVectorStore& av,
                          RowWinnerStore& rw, TableStateStore& ts, ChangelogStore& clog,
                          SessionRecorder& rec, SchemaGuard& guard, ChangesetApplier& applier,
                          const QString& nodeId, qint64 streamEpoch, const QString& schemaFp,
                          qint64 schemaVer);

    // 统一写入口：按 params.kind 分派到对应分支，返回写结果回执。
    // 副作用：在内部各分支里开启并提交/回滚一个完整的 WriteTxn（apply 三件套原子化）。
    WriteResult execute(const WriteParams& params);

    // M-01 fix: public static version so submitImportSync can extract incremental mutations
    // from the import changeset without a resetFromBaseline() full table scan.
    // syncTables is the canonical allow-list; tables not in the list are skipped.
    // M-01 修复：extractMutations 的公开静态版。让 submitImportSync 能直接从“导入产生的
    //   changeset”里抽取增量 TableMutation，而不必走代价高昂的 resetFromBaseline() 全表重扫。
    //   做什么：解析 changeset → 产出每行的 before/after/pk 哈希列表（供 table_state 增量更新）。
    //   参数  ：changeset=原始字节；db=用于 PRAGMA table_info 取列名的连接；
    //           syncTables=规范同步表白名单（不在白名单的表被跳过）。
    //   返回  ：逐行 TableMutation 列表（空 changeset → 空列表）。
    static QList<TableMutation> extractMutationsStatic(const QByteArray& changeset,
                                                       QSqlDatabase& db,
                                                       const QStringList& syncTables);

   private:
    WriteResult branchA(const WriteParams& p);  // InboundChangeset —— 应用对端原始 changeset
    WriteResult branchBC(const WriteParams& p);  // InboundSelectionPush or LocalWrite —— 逐行
                                                 // UPSERT + session 重捕获

    // L-02 fix: execMutation() removed (dead code with unquoted identifiers).
    // L-02 修复：旧的
    // execMutation()（标识符未加引号、已无人调用的死代码）已删除——此行仅作历史留痕，保留勿删。

    // Parse a changeset blob into a list of TableMutations for table_state accounting (I-07).
    // H-01 fix: syncTables is the same allow-list as filterCb; tables not in the list
    // (and __sync_* tables) are skipped so __sync_table_state is not polluted.
    // 把一段 changeset 二进制解析成 TableMutation 列表，供 __sync_table_state 记账（设计 I-07）。
    // 实例版（用成员 wconn_ 取列名），逻辑与上面的静态版完全一致，只是连接来源不同。
    // H-01 修复：syncTables 与 ChangesetApplier::filterCb 使用同一份白名单——不在名单内的表
    //   （以及所有 __sync_* 元表）都被跳过，绝不写入 __sync_table_state，防止污染校验和。
    QList<TableMutation> extractMutations(const QByteArray& changeset,
                                          const QStringList& syncTables);

    QSqlDatabase& wconn_;        // 写连接（引用，外部拥有）
    sqlite3* h_;                 // 同一连接的原生句柄（session/changeset C API）
    AppliedVectorStore& av_;     // 连续序列水位（去重/补洞）
    RowWinnerStore& rw_;         // 行级冲突仲裁胜者账本
    TableStateStore& ts_;        // 各表校验和（增量维护）
    ChangelogStore& clog_;       // __sync_changelog 读写
    SessionRecorder& rec_;       // session 变更捕获器（分支 B/C）
    SchemaGuard& guard_;         // 来件表结构校验
    ChangesetApplier& applier_;  // 分支 A 的 changeset 应用器
    QString nodeId_;             // 本节点 id（分支 C 的 origin）
    qint64 streamEpoch_;         // 本节点流纪元（分支 C 写 changelog 用）
    QString schemaFp_;           // 本地表结构指纹（分支 C 封存元信息用）
    qint64 schemaVer_;           // 本地表结构版本（分支 C 封存元信息用）
};

}  // namespace dbridge::sync
