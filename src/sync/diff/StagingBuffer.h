#pragma once
#include "dbridge/sync/SyncTypes.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

#include "../apply/UpsertExecutor.h"

// ============================================================================
// StagingBuffer.h — 比对合并决策的「内存暂存区」（采纳的对端行，待一次性落库）
// ============================================================================
//
// 【这个类是什么 / 在 diff 链路中的位置】
//   diff 三件套里的「暂存」。ComparisonSession 在交互式比对中，每当用户决定「这一行/
//   这一个单元格采用对端值」，就把那一行的最终目标内容存进本类（内存里，不立即写库）。
//   这样可以反复调整、撤销、叠加，直到用户点「保存」时，才由 ComparisonSession::save()
//   把暂存的全部决策一次性写回本地库——「先在内存里攒好，再原子地落地」。
//
// 【数据组织：以 (表, 主键) 为唯一键，存「整行的目标快照」】
//   暂存的颗粒是「行」：每个 (table, pk) 至多对应一条 StagedRow，其 row 字段是「这一行
//   save 后应当变成的样子」（列名→值的完整 QVariantMap）。
//     · acceptRemote/stageRow → 直接把对端整行作为目标存入；
//     · stageCell             → 在「已暂存的目标行」基础上只改某一列（累加，见 C-4 fix）。
//   再次 stage 同一 (table, pk) 是「覆盖」语义（替换目标行），不是追加。
//
// 【两条落库路径（重要区别）】
//   ① toMutations()：把暂存翻译成 RowMutation 列表，交给上层（ComparisonSession::save
//      → context_->workerCaptureWriteFn）走 CapturedWriteTemplate——这条路会被 SQLite
//      session 捕获、写入 __sync_changelog、并广播给对端，等同一次正常本地写（C-05 fix，
//      首选路径）。本类此时只「生成变更描述」，不亲自碰数据库。
//   ② save()：直接用传入的 wconn + UpsertExecutor 在一个 WriteTxn 里把暂存行 UPSERT 落库。
//      这是回退/旧路径（用于没有接 capture 函数的上下文，如测试桩）；这条路不经 session
//      捕获，故改动不会自动广播。
//
// 【为什么用 UPSERT（DoUpdate）】
//   采纳对端的行可能在本地「已存在（需更新）」也可能「不存在（需插入，对端 Added 行）」，
//   UPSERT「有则更、无则插」恰好两种都覆盖，且对主键冲突幂等。
//
// 【协作者】StagingBuffer（本类）↔ ComparisonSession（调用方）↔ UpsertExecutor/
//   WriteTxn（落库）↔ RowMutation（变更描述，定义见 SyncTypes.h）。
//
// 【线程】非线程安全。它是 ComparisonSession 的私有成员，只在会话调用线程上访问；
//   真正的写库经 workerWriteFn/workerCaptureWriteFn 被 marshalling 到 SyncWorker 单写线程。
// ============================================================================

namespace dbridge::sync {

// In-memory staging area for comparison session edits.
// save() flushes to DB via UpsertExecutor inside a WriteTxn.
// 比对会话编辑内容的内存暂存区；save() 在一个 WriteTxn 内经 UpsertExecutor 落库。
class StagingBuffer {
   public:
    // 暂存（或覆盖）某一行的目标内容。
    //   做什么：若 (table, pk) 已存在则用新的 row 覆盖其目标快照；否则追加一条新暂存项。
    //   参数：table 表名；pk 主键（字符串形式，与差异比对里的 key 一致）；
    //         row 该行「save 后应成为的样子」（列名→值的完整映射）。
    //   副作用：修改内部 staged_ 列表。复杂度：O(n) 线性查找已存在项。
    void stage(const QString& table, const QString& pk, const QVariantMap& row);

    // 取消暂存：从暂存区移除 (table, pk)（撤销之前的采纳决定）。未找到则静默无操作。
    //   复杂度：O(n)。
    void unstage(const QString& table, const QString& pk);

    // C-4 fix: return the currently staged row for (table, pk), or empty map if not staged.
    // C-4 修复：返回 (table, pk) 当前已暂存的目标行；若未暂存则返回空 map。
    //   为什么需要它：stageCell 要在「已暂存版本」上叠加单列修改，必须先把当前暂存行取出来，
    //   否则多次 stageCell 会各自从 local/remote 重新起算、互相覆盖（详见 ComparisonSession）。
    //   复杂度：O(n)。
    QVariantMap getRow(const QString& table, const QString& pk) const;

    // Flush all staged rows to wconn via UpsertExecutor.
    // pkCols: primary key column names (used to build RowMutation).
    // 回退路径：把全部暂存行经 UpsertExecutor 直接 UPSERT 落库（包裹在一个 WriteTxn 中）。
    //   参数：wconn 写连接（须已 open）；upsert 执行器；
    //         pkCols 主键列名（构造 RowMutation 用，决定按哪些列判冲突=哪一行）；err 错误出参。
    //   返回：成功 true；BEGIN/apply/COMMIT 任一失败则回滚并返回 false（暂存内容保留不清）。
    //   事务：本方法自行 BEGIN…COMMIT；失败会 rollback。
    //   注意：此路径不经 SQLite session 捕获，改动不会广播给对端；首选用 toMutations 那条路。
    bool save(QSqlDatabase& wconn, UpsertExecutor& upsert, const QStringList& pkCols, QString* err);

    // C-05 fix: build RowMutation list for CapturedWriteTemplate (bypasses direct DB write).
    // pkColsPerTable maps table name → PK column names; tables missing from the map fall back
    // to pkColsFallback.
    // C-05 修复：把暂存翻译成 RowMutation 列表，供上层走 CapturedWriteTemplate（不直接写库，
    // 改由 worker 的捕获式写落地 → 被 session 捕获 → 写 changelog → 广播给对端）。
    //   参数：pkColsPerTable 每表的主键列名映射（让每条 RowMutation 用本表正确的主键）；
    //         pkColsFallback 当某表不在上述映射中时使用的兜底主键列名。
    //   返回：与暂存行一一对应的 RowMutation 列表（mode 一律 DoUpdate=UPSERT 更新语义）。
    //   纯函数：只读取暂存、不修改本对象、不碰数据库。复杂度：O(n)。
    QList<RowMutation> toMutations(const QHash<QString, QStringList>& pkColsPerTable,
                                   const QStringList& pkColsFallback = QStringList()) const;

    // 清空暂存区（放弃全部尚未落库的决策）。save 成功后、或 discard 时调用。复杂度：O(n)。
    void discard();
    // 暂存区是否为空（没有任何待落库决策）。供 save() 走「空保存快速路径」判断。复杂度：O(1)。
    bool isEmpty() const;

   private:
    // 单条暂存项：一行的「目标快照」及其定位键。
    struct StagedRow {
        QString table;    // 表名
        QString pk;       // 主键（字符串形式）
        QVariantMap row;  // 该行 save 后应成为的完整内容（列名→值）
    };
    // 暂存集合，按插入/采纳顺序排列。用线性表而非哈希：暂存条目通常不多，且需保序落库。
    QList<StagedRow> staged_;
};

}  // namespace dbridge::sync
