#pragma once
#include <QSqlDatabase>
#include <QString>

// ============================================================================
// AppliedVectorStore.h — 「已应用向量」(applied-vector) 的读写器
// ============================================================================
//
// 【这个类是什么】
//   增量同步里，每个写入源 (origin) 产生的变更都被编号成一条严格递增的整数序列：
//   seq = 1, 2, 3, …（同一 origin 在同一 stream_epoch 内连续、不跳号）。
//   本节点把「自己已经成功应用到的最高序号」记成一个「水位」(watermark)。
//   把每个 origin 的水位合在一起，就是这个节点的「已应用向量」(applied-vector)——
//   即「我已经追到每个上游各自的第几号变更了」。本类就是这张向量表
//   (__sync_table __sync_applied_vector) 的唯一读写门面。
//
// 【它在 apply 管线里解决什么问题】
//   ① 去重 (idempotency)：同一条变更可能被重复投递（重传、广播扇出、节点重连补发）。
//      若 seq <= 水位，说明早已应用过，直接当成「成功的空操作」跳过，避免重复落库。
//   ② 判定 gap（空洞）：变更必须「连续应用」。若收到 seq > 水位+1，说明中间漏收了
//      （比如先到了 5 号，但本地只追到 3 号，缺 4 号）。此时不能贸然应用 5 号——
//      否则会基于错误的前置状态产生分歧。必须先等缺失的中间变更补齐。
//   ③ 推进水位 (advance)：一条变更成功落库后，把水位 +1，记录「我追到这里了」。
//      推进必须与 apply 在「同一个写事务」内完成，否则水位与实际落库会不一致。
//
// 【关键概念：origin / stream_epoch / seq】
//   · origin       —— 变更的「源头节点」标识（谁最早产生了这次写入）。
//   · stream_epoch —— 序列的「世代号」。一旦某 origin 重置基线 (baseline)，它的
//                     序列会从头开始；用 epoch 区分「新旧两条同号但不同世代的序列」，
//                     防止旧世代的 5 号被误当成新世代的 5 号。水位是 per (origin, epoch) 的。
//   · seq          —— 该 origin 在该 epoch 内的单调递增序号（从 1 起）。
//
// 【表结构 __sync_applied_vector（由 SyncDDL 建表，本类只读写其内容）】
//   主键    : (origin, stream_epoch)         —— 每个源·每个世代一行水位
//   applied_seq         : 已应用到的最高 seq（即「水位」），从 0 起（0 = 尚未应用任何变更）
//   baseline_generation : 基线代号，每次基线重置 +1（用于诊断/排查，apply 逻辑不依赖它判定）
//   updated_ms          : 最近一次更新的毫秒时间戳（运维/诊断用）
//
// 【线程/事务约束】所有方法都需传入打开的 QSqlDatabase；advance() 必须与 apply 处于
//   同一 WriteTxn 内（见 CapturedWriteTemplate::branchA 第 4 步）。本类自身无状态，
//   不缓存，可安全地在持有写连接的那一个线程上反复调用。
// ============================================================================

namespace dbridge::sync {

// SeqCheckResult —— check() 的三态判定结果（对应设计编号 G-05「连续序列约束」）。
// 三者互斥，调用方据此分支：Apply→照常应用；NoOp→静默成功跳过；Gap→保留待补齐。
enum class SeqCheckResult {
    Apply,  // seq == 水位+1：正好是下一条，可以应用
    NoOp,   // seq <= 水位：早已应用过（幂等的空操作，安全跳过）
    Gap     // seq > 水位+1：中间缺了前置变更（出现空洞，暂不能应用）
};

// 维护 __sync_applied_vector，对每个 origin 强制「连续序列」语义。
// 本类是「无状态门面」：每次调用都现查现写，状态全部落在数据库表里。
class AppliedVectorStore {
   public:
    // 初始化/自检：确认 __sync_applied_vector 表存在且可访问（不建表，建表是 SyncDDL 的事）。
    // 做什么：执行一条 "WHERE 0" 的探针查询（不返回任何行，只验证表与列可用）。
    // 返回  ：true=表就绪；false=表缺失/不可访问，*err 填入数据库错误文本。
    // 副作用：无（只读探针）。
    bool init(QSqlDatabase& db, QString* err);

    // 检查 (origin, epoch) 下的 seq 能否被应用——这是 gap/去重判定的核心入口。
    // 做什么：读出当前水位 applied_seq，按「seq vs 水位」三态返回 Apply/NoOp/Gap。
    // 参数  ：origin/epoch 定位水位行；seq 是待检查的来件序号。
    // 返回  ：SeqCheckResult 三态之一。仅当返回 Gap 时才会写 *err（人类可读的空洞描述）。
    // 副作用：无（只读，不改表）。
    // 边界  ：水位行尚不存在（首次见到该源）时，仅 seq==1 可应用，>1 视为 Gap，<=0 视为 NoOp。
    SeqCheckResult check(QSqlDatabase& db, const QString& origin, qint64 epoch, qint64 seq,
                         QString* err);

    // 推进水位到 seq（把「已应用到的最高序号」更新为 seq）。
    // 做什么：UPSERT 一行，仅当 seq 比当前水位更大时才真正抬高水位（防回退）。
    // 关键约束：必须与对应的 apply 处于同一个 WriteTxn 内——这样「落库」与「水位推进」
    //   要么一起提交、要么一起回滚，绝不会出现「应用了但水位没动」或反之。
    // 返回  ：true=成功；false=SQL 执行失败，*err 填错误文本。
    bool advance(QSqlDatabase& db, const QString& origin, qint64 epoch, qint64 seq, QString* err);

    // 基线重置：把水位清零 (applied_seq = 0) 并抬高 baseline_generation。
    // 何时用：本节点接收了某 origin 的「全量基线」，旧的增量序列作废、需从 0 重新计数。
    // 返回  ：true=成功；false=SQL 失败，*err 填错误文本。
    bool reset(QSqlDatabase& db, const QString& origin, qint64 epoch, qint64 baselineGeneration,
               QString* err);

    // C-03 fix: 重置到「某个具体的 origin_seq」（基线导出时的权威截断点）。
    //   与 reset() 永远写 applied_seq=0 不同，本方法写入「基线快照里该源真实的最大 seq」。
    //   为什么需要：基线不是「空白起点」而是「截至 seq=N 的全量快照」；若仍把水位置 0，
    //   后续增量从 N+1 来时会被误判成 Gap（因为水位=0 期望下一条是 1）。写入真实的 N，
    //   才能让 gap 判定从正确的起点开始（下一条期望 N+1）。
    // 返回  ：true=成功；false=SQL 失败，*err 填错误文本。
    bool resetTo(QSqlDatabase& db, const QString& origin, qint64 epoch, qint64 originSeq,
                 qint64 baselineGeneration, QString* err);

    // 返回 (origin, epoch) 当前水位；若该行尚不存在返回 -1（注意区别于水位为 0
    // 的「已存在但未应用」）。 副作用：无（只读）。常用于诊断、ACK 回执填值、广播屏障判断等。
    qint64 current(QSqlDatabase& db, const QString& origin, qint64 epoch);

   private:
    // 私有底层读：取出 (origin, epoch) 行的 (applied_seq, baseline_generation)。
    // 返回  ：true=行存在并已写出参；false=行不存在，此时 *appliedSeq 置 -1、*baselineGen 置 0。
    // 说明  ：check()/current() 都建立在它之上；把「行不存在」与「水位=-1」统一表达，
    //         让上层用 -1 作为「从未应用过」的哨兵值。
    bool readRow(QSqlDatabase& db, const QString& origin, qint64 epoch, qint64* appliedSeq,
                 qint64* baselineGen);
};

}  // namespace dbridge::sync
