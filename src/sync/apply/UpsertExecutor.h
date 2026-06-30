#pragma once
#include "dbridge/Types.h"
#include "dbridge/sync/SyncTypes.h"

#include <QHash>
#include <QSqlDatabase>
#include <QSqlQuery>

// ============================================================================
// UpsertExecutor.h — 把一批「行级变更」(RowMutation) 以 UPSERT 方式写入本地库
// ============================================================================
//
// 【这个类是什么】
//   apply 子系统里负责「实际把行落库」的小执行器。它接收一批 RowMutation（每条描述
//   「往某张表写一行，主键是哪些列，是 UPDATE 覆盖还是 IGNORE 跳过」），逐条构造并
//   执行 SQLite 的 UPSERT 语句。它不关心 changeset、不关心冲突仲裁——那些在更高层
//   （ChangesetApplier / CapturedWriteTemplate）处理；本类只管「给定列与值，写进去」。
//
// 【什么是 UPSERT、为什么需要它】
//   UPSERT = UPDATE + INSERT 的合体：「若行不存在则插入，若已存在（撞了冲突键）则更新」。
//   同步场景里，同一行可能先被插入、后被多次更新，且这些变更乱序/重复到达，所以不能用
//   裸 INSERT（会撞唯一约束报错）。UPSERT 用「冲突键」(conflict key，通常是主键) 判定
//   到底该插还是该更，天然幂等、可重放。
//
// 【两种 UpsertMode（见 SyncTypes.h）】
//   · DoUpdate  → "INSERT … ON CONFLICT(pk) DO UPDATE SET 非主键列=excluded.该列"
//                 （撞键时用新值覆盖旧值，即「来源胜」语义的落地）。
//   · DoNothing → "INSERT OR IGNORE …"（撞键时什么都不做，保留本地旧值）。
//
// 【错误模型——区分「致命」与「逐行」两类】
//   · 致命错误（apply 返回 false）：prepare 失败（多半是表不存在）、库未打开。一旦致命，
//     调用方应回滚整个事务。
//   · 逐行错误（apply 仍返回 true，收集进 errors 列表）：某一行 exec 失败（约束冲突、
//     外键违反等）。本类不抛异常、不中断，继续处理后续行；是否因此回滚由上层决定
//     （事实上 CapturedWriteTemplate 见到任何逐行错误就整块回滚——见其 C-09 fix）。
//
// 【预编译语句缓存 cache_】
//   同一条 SQL 反复执行时，复用已 prepare 的 QSqlQuery 能省去重复编译开销。缓存以
//   「完整 SQL 字符串」为键（M-04 fix，详见成员注释）。
//
// 【线程】非线程安全：cache_ 是裸 QHash，且 QSqlQuery 绑定到特定连接；只能在持有该
//   写连接的单一线程上使用。
// ============================================================================

namespace dbridge::sync {

class UpsertExecutor {
   public:
    // 在「已经打开的事务内」应用一批 RowMutation。
    // 做什么：逐条 RowMutation 构造 UPSERT SQL（带缓存）、绑定值、执行。
    // 参数  ：db=写连接（须已 open，且调用方已 BEGIN）；rows=待写入的行；
    //         errors=逐行失败的收集器（可为 nullptr 表示不收集）；err=致命错误文本出参。
    // 返回  ：仅在「致命/不可恢复」错误时返回 false（如 prepare 失败、库未打开）；
    //         逐行失败不影响返回值（true），而是追加到 *errors。
    // 副作用：向 db 写入数据；可能向 cache_ 插入新的预编译语句。
    // 事务  ：本方法不自行 BEGIN/COMMIT，必须被包在调用方的写事务里。
    bool apply(QSqlDatabase& db, const QList<RowMutation>& rows, QList<dbridge::RowError>* errors,
               QString* err);

    // 清空预编译语句缓存。
    // 何时用：连接被关闭/重建、或表结构发生迁移后，缓存里的旧语句已失效，必须丢弃。
    void clearPreparedCache();

   private:
    // M-04 fix: 缓存键现在是「完整 SQL 字符串」（而非旧版的 "table:mode"）。
    //   原因：同一张表、同一 mode 下，列集合 / 主键集合不同会生成不同的 SQL；若仍用
    //   "table:mode" 作键，不同列集会错误地共用同一条预编译语句（绑定数量/语义不符）。
    //   用整条 SQL 当键，则「SQL 不同 → 键必不同」，从根上杜绝串用。
    QHash<QString, QSqlQuery> cache_;

    // 按 (表名, 列名表, 主键列表, 模式) 构造一条 UPSERT SQL 文本（不执行，仅拼串）。
    // 标识符一律经 SqlBuilder::quoteIdent 加引号转义，防注入并兼容含特殊字符的列名。
    // 退化规则：DoUpdate 下若「所有列都是主键列」（没有可更新的非主键列），会退化为
    //   INSERT OR IGNORE（等价 DoNothing），因为 SET 子句会是空的、无意义。详见 .cpp。
    QString buildUpsertSql(const QString& table, const QStringList& cols, const QStringList& pkCols,
                           UpsertMode mode);
};

}  // namespace dbridge::sync
