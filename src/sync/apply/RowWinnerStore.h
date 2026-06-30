#pragma once
#include <QByteArray>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

#include <climits>

// ============================================================================
// RowWinnerStore.h — 行级「胜者表」：冲突仲裁的权威裁判台
// ============================================================================
//
// 【这个类是什么】
//   多节点并发写同一行时会产生冲突，必须有一个「确定性」的裁决规则来选出唯一胜者，
//   否则不同节点最终会收敛到不同的值（违反最终一致性）。RowWinnerStore 就是这个
//   裁判台：它把每一行「当前胜者」是谁持久化到本地元数据表 __sync_row_winner 里，
//   键为 (table_name, pk_hash)，值为胜者的来源标识(origin)、等级(rank)、序号(seq)
//   以及胜者行的完整内容(winningContent)。
//
// 【仲裁规则（max-element / G-01）—— 全局确定，与到达顺序无关】
//   给定「挑战者(challenger)」与「在位者(incumbent)」，挑战者获胜当且仅当：
//     1. 在位者不存在（rank == INT_MIN 哨兵）；             —— 空位直接占据
//     2. challenger.rank   >  incumbent.rank；              —— 高 rank 胜（如中心 > 边缘）
//     3. rank 相同且 challenger.originSeq > incumbent.originSeq；  —— 同级取较大 seq（较新）
//     4. rank、seq 都相同且 challenger.origin > incumbent.origin。 —— 仍打平则比 origin 字符串
//   规则 4（H-01 fix）是纯粹为「确定性」存在的兜底：origin 字符串字典序大者胜。
//   它本身无业务含义，但保证「任意到达顺序下，所有节点算出的胜者完全一致」。
//   完整比较逻辑见 .cpp 里的 beats()，是本类唯一的「真理来源」。
//
// 【为什么要存 winningContent（C-01）】
//   一个低 rank 节点发来的 DELETE 可能误删了本地的高 rank 胜者行。SQLite 的
//   changeset apply 无法事先按行过滤，所以删除会先真实发生。ChangesetApplier 在
//   apply 之后需要把被误删的胜者行「重新插回去」——而插回的数据就来自这里缓存的
//   winningContent（JSON 编码的整行内容）。没有它就无从恢复，apply 只能失败回滚。
//
// 【与其它组件的关系】
//   · ChangesetApplier  —— 冲突回调里查 get() 做仲裁；apply 后用 putOrRefill() 更新胜者、
//                          用 winningContent 恢复被误删的高 rank 行。
//   · TableStateStore   —— pkHash() 复用它的 rowHash() 规范化编码，确保两处算出的
//                          哈希落在同一键空间（M-01 fix）。
//   · __sync_row_winner —— 持久化表，DDL 见 schema 子系统；本类只做读写，不建表。
//
// 【线程】本类无成员状态（纯静态规则 + 每次显式传入 db），但 QSqlDatabase 连接本身
//   不可跨线程并发使用；所有调用须在持有写连接的同一线程内、且通常在一个写事务里完成。
// ============================================================================

namespace dbridge::sync {

// RowWinner —— 描述某一行 (table, pk_hash) 当前的「胜者」及其裁决依据。
// 既用作 get() 的返回值（读出在位者），也用作 put()/putOrRefill() 的入参（写入挑战者）。
struct RowWinner {
    QString origin;  // 胜者来源节点标识（originId）；同级打平时作字典序兜底比较
    int rank = INT_MIN;  // 胜者等级；INT_MIN 是「无胜者」哨兵（表中无此行记录）
    qint64 originSeq = 0;  // 胜者在其来源流中的序号；同 rank 时越大越新、越优先
    QByteArray contentHash;  // 胜者行内容的指纹（SHA-256 前 16 字节），用于快速比对是否同值
    // C-01：胜者行的完整内容，JSON 编码（列名→类型标签值）。
    // 唯一用途：低 rank DELETE 误删高 rank 胜者后，据此把整行重新插回（见类头说明）。
    QString winningContent;
};

// RowWinnerStore —— 维护 __sync_row_winner 表，按 (rank, seq) 取最大元素的方式仲裁冲突(G-01)。
class RowWinnerStore {
   public:
    // 初始化：仅探测 __sync_row_winner 表是否存在/可读（用 `WHERE 0` 不取任何行只验结构）。
    // 失败（表缺失等）→ 返回 false 并把驱动错误文本写入 *err。不负责建表。
    bool init(QSqlDatabase& db, QString* err);

    // 读取当前胜者。查无记录时返回一个 rank == INT_MIN 的空 RowWinner（哨兵），
    // 调用方据此判断「此行尚无任何胜者」。副作用：执行一次 SELECT。
    RowWinner get(QSqlDatabase& db, const QString& table, const QString& pkHash);

    // 写入新胜者，但仅当挑战者按 beats() 规则确实战胜在位者时才真正写；否则直接返回
    // true（无操作）。 即：inRank > cur.rank，或（rank 相同且 inSeq >
    // cur.originSeq），或更细的同级兜底。 用 INSERT ... ON CONFLICT DO UPDATE
    // 实现「不存在则插入、存在则覆盖」。
    bool put(QSqlDatabase& db, const QString& table, const QString& pkHash, const RowWinner& winner,
             QString* err);

    // H-01 fix: 类似 put()，但额外允许「rank/seq 完全相同且在位者 winningContent 为空」时也覆盖。
    // 用途：conflictCb 在冲突回调里曾抢先写过一条胜者记录，但当时拿不到完整行内容
    //       （winningContent 为空）；apply 之后本方法带着完整内容补写，把那条残缺记录「填满」。
    bool putOrRefill(QSqlDatabase& db, const QString& table, const QString& pkHash,
                     const RowWinner& winner, QString* err);

    // 删除全部胜者记录（基线(baseline)重置后调用——旧的胜负全部作废，从头重新累积）。
    bool resetAll(QSqlDatabase& db, QString* err);

    // C-06 fix: 删除单行胜者记录（当某行的胜者已被一个 DELETE 合法擦除时调用）。
    bool clear(QSqlDatabase& db, const QString& table, const QString& pkHash, QString* err);

    // 由主键各列的值生成 pk_hash：规范化键字符串 → SHA-256 → 取前 16 字节 → 十六进制串。
    // 复用 TableStateStore::rowHash() 的规范化编码（M-01 fix），保证全库哈希同空间。
    static QString pkHash(const QVariantMap& pkValues);

   private:
    // 仲裁核心：挑战者是否战胜在位者。是本类全部「谁赢」判断的唯一真理来源（见类头规则 1–4）。
    static bool beats(const RowWinner& challenger, const RowWinner& incumbent);
};

}  // namespace dbridge::sync
