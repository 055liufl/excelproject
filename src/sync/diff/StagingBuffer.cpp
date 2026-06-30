// ============================================================================
// StagingBuffer.cpp — 暂存区实现（内存累积 + 两条落库路径）
// ============================================================================
//
// 实现要点：
//   · staged_ 是一个 (table, pk) → 目标行 的线性表；stage/unstage/getRow 都按线性查找
//     定位（条目少，O(n) 足够）。stage 命中已存在键时是「覆盖」，体现「一行只有一个最终
//     目标快照」的设计。
//   · 落库有两个出口：save()（直连 wconn 自管事务，回退路径）与 toMutations()（只产出
//     RowMutation 交给上层走捕获式写，首选路径）。两者构造 RowMutation 的方式几乎一样，
//     差别仅在「主键列从哪来」：save 用调用方给的同一份 pkCols；toMutations 用「每表各自
//     的主键列」（更精确，因为一次暂存可能跨多张表）。
// ============================================================================

#include "StagingBuffer.h"

#include "../WriteTxn.h"

namespace dbridge::sync {

// 暂存/覆盖一行：先线性查找是否已存在同 (table, pk) 的暂存项；
// 命中则原地覆盖其 row（保持位置不变 → 落库顺序稳定），否则追加到末尾。
void StagingBuffer::stage(const QString& table, const QString& pk, const QVariantMap& row) {
    for (StagedRow& sr : staged_) {
        if (sr.table == table && sr.pk == pk) {
            sr.row = row;  // 覆盖语义：同一行只保留最新的目标快照
            return;
        }
    }
    staged_.append(StagedRow{table, pk, row});
}

// 取消暂存：线性查找并移除首个匹配项；未找到则什么都不做（撤销一个未暂存的行是无害的）。
void StagingBuffer::unstage(const QString& table, const QString& pk) {
    for (int i = 0; i < staged_.size(); ++i) {
        if (staged_[i].table == table && staged_[i].pk == pk) {
            staged_.removeAt(i);
            return;
        }
    }
}

// 回退落库路径：把全部暂存行打成一批 RowMutation，在一个 WriteTxn 里 UPSERT 落库。
// 任一阶段失败即回滚返回 false；注意此路径不经 SQLite session 捕获，改动不会广播。
bool StagingBuffer::save(QSqlDatabase& wconn, UpsertExecutor& upsert, const QStringList& pkCols,
                         QString* err) {
    if (staged_.isEmpty())
        return true;  // 没有暂存 → 视作成功（无事可做）

    // Group all staged rows into a single batch of RowMutations.
    // 把所有暂存行汇成一批 RowMutation（每行一条），一次性提交给 UpsertExecutor。
    QList<RowMutation> mutations;
    mutations.reserve(staged_.size());

    for (const StagedRow& sr : staged_) {
        RowMutation rm;
        rm.table = sr.table;
        rm.columns = sr.row.keys();  // 列名（QVariantMap 的 key），与 values 顺序一一对应
        rm.values = sr.row.values();  // 列值，顺序与 keys() 一致（Qt 保证同一 map 两者对齐）
        rm.pkColumns = pkCols;  // 回退路径：所有表共用调用方给的同一份主键列名
        rm.mode = UpsertMode::DoUpdate;  // UPSERT 更新语义：有则更、无则插（采纳对端的落地）
        mutations.append(rm);
    }

    // 包在一个写事务里：要么整批成功提交，要么出错全回滚（保证「全有或全无」）。
    WriteTxn txn(wconn);
    QString beginErr;
    if (!txn.begin(&beginErr)) {
        if (err)
            *err = beginErr;
        return false;  // 连事务都开不起来：直接失败，无需回滚（尚未 BEGIN）
    }

    QString applyErr;
    if (!upsert.apply(wconn, mutations, nullptr, &applyErr)) {
        txn.rollback();  // 写入过程中致命错误 → 回滚已写部分
        if (err)
            *err = applyErr;
        return false;
    }

    QString commitErr;
    if (!txn.commit(&commitErr)) {
        if (err)
            *err = commitErr;  // 提交失败（WriteTxn 析构会兜底回滚）
        return false;
    }

    return true;
}

// 首选落库路径的「前半段」：只把暂存翻译成 RowMutation 列表，不碰数据库。
// 与 save() 构造逻辑相同，唯一区别：每条 mutation 的主键列取「本表专属」的那份，
// 表不在映射中时退回 pkColsFallback（联合主键场景下比 save 的「全表共用一份」更精确）。
QList<RowMutation> StagingBuffer::toMutations(const QHash<QString, QStringList>& pkColsPerTable,
                                              const QStringList& pkColsFallback) const {
    QList<RowMutation> mutations;
    mutations.reserve(staged_.size());
    for (const StagedRow& sr : staged_) {
        RowMutation rm;
        rm.table = sr.table;
        rm.columns = sr.row.keys();
        rm.values = sr.row.values();
        // 取本表专属主键列；该表未登记则用兜底列名（QHash::value 的第二参即缺省值）。
        rm.pkColumns = pkColsPerTable.value(sr.table, pkColsFallback);
        rm.mode = UpsertMode::DoUpdate;
        mutations.append(rm);
    }
    return mutations;
}

// 取出某 (table, pk) 当前已暂存的目标行；未暂存返回空 map。stageCell 的累加基础。
QVariantMap StagingBuffer::getRow(const QString& table, const QString& pk) const {
    for (const StagedRow& sr : staged_) {
        if (sr.table == table && sr.pk == pk)
            return sr.row;
    }
    return {};
}

// 清空全部暂存（放弃所有未落库决策）。save 成功后或 discard 时调用。
void StagingBuffer::discard() {
    staged_.clear();
}

// 暂存区是否为空。
bool StagingBuffer::isEmpty() const {
    return staged_.isEmpty();
}

}  // namespace dbridge::sync
