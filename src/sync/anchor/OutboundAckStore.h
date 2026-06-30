#pragma once
#include <QSqlDatabase>
#include <QString>

// ============================================================================
// OutboundAckStore.h — 「对外发送方向」ACK 水位/锚点存储（__sync_outbound_ack 表）
// ============================================================================
//
// 【这个文件是什么】
//   本节点对 __sync_outbound_ack 这张同步元数据表的「数据访问层（DAO）」封装。
//   表名里的 outbound（对外、出方向）是理解一切的钥匙：它记录的是本节点
//   「往各个对端发送」这条方向上的进度账本——
//     · 我发给某对端的变更，对端已经回 ACK 确认应用到了哪个序号（acked_seq）？
//     · 我已经把本地 changelog 中哪个位置之前的内容广播给该对端了（last_sent_seq）？
//   它不关心「别人发给我、我应用到哪」（那是入方向，归 __sync_applied_vector 管）。
//
// 【为什么需要这本账（在同步管线中的位置与价值）】
//   增量同步的核心难点是「可靠地、不重不漏地把本地变更推给所有对端」。要做到这点，
//   发送方必须持久化两个稳定参照点（锚点 / watermark）：
//     1) 已发送水位（last_sent_seq）：下次广播从哪儿接着发，避免每次都从头重发；
//     2) 已确认水位（acked_seq）：对端确实收妥并落库到了哪，这是「真正安全」的水位。
//   这两个水位支撑了三件关键事：
//     a) changelog 裁剪：所有对端都确认过的最小序号之前的变更日志可被安全删除
//        （见 minAckedSeq —— 全体对端取 MIN，谁都确认了才敢删，否则会漏发）；
//     b) 重发判定：未被对端 ACK 的变更必须在下次广播时仍可被重新读出并发送
//        （见 minUnackedLocalSeq —— 把广播读取下界压到「最早一条未确认」处）；
//     c) 完成判定：前台 sync() 是否真正成功，取决于所有 PendingAckEntry 期望的
//        targetSeq 是否都被对端 ACK 到位（acked_seq 推进到目标即视为该对端达成）。
//
// 【数据流中的协作者（谁来调这些方法）】
//   · SyncWorker（src/sync/SyncWorker.*，核心后台线程）是主要调用方：
//       - 广播前：调 lastSentLocalSeq / minUnackedLocalSeq 计算「这次该读 changelog
//         哪一段发给该对端」的读取边界；
//       - 广播后：调 updateLastSent 把已发送水位往前推；
//       - 收到对端 ChangesetAck 时：调 updateAcked 把该对端的 acked_seq 往前推；
//       - 周期性裁剪 changelog 前：调 minAckedSeq 求「全体对端公认安全」的下界。
//   · 基线流程（baseline/*）：边缘节点入网或对齐基线期间，对其调
//     setPendingBaseline(true) 把该对端临时排除出 minAckedSeq 的裁剪计算，
//     避免「基线尚未铺好、acked_seq 还很低」把别人的 changelog 误删掉。
//   · 表结构（DDL）见 SyncDDL.h 中的 __sync_outbound_ack 定义；本类只读写、不建表。
//
// 【__sync_outbound_ack 表结构速览（权威定义见 src/sync/SyncDDL.h）】
//   主键 PRIMARY KEY(peer, origin, stream_epoch) —— 「对哪个对端 / 哪个来源 / 哪个纪元」
//   这三元组唯一定位一行水位记录。各列含义：
//     peer            TEXT  对端节点 id（本行水位是「对它」的发送账）。
//     origin          TEXT  变更的来源节点 id；普通行 = 真实 origin，另有一个特殊
//                           「哨兵」取值 '__broadcast__' 见下方说明。
//     stream_epoch    INTEGER 流纪元；基线重置后递增，隔离新旧两段序号流。
//     acked_seq       INTEGER 该对端已 ACK 确认应用到的「来源序号(origin_seq)」上界，
//                             DEFAULT -1（尚无任何确认）。只增不减（MAX 推进）。
//     last_sent_seq   INTEGER 已广播给该对端的「本地序号(local_seq)」上界，
//                             DEFAULT -1。只增不减。仅哨兵行使用（见下）。
//     last_ack_ms     INTEGER 最近一次 ACK/发送的时间戳（毫秒），用于诊断对端是否失联。
//     pending_baseline INTEGER 该对端是否处于「基线进行中」，=1 时被 minAckedSeq 排除。
//     last_push_id / last_chunk_seq 选择性推送的断点续传游标（本类暂未直接读写）。
//
// 【两类行：真实 origin 行 vs '__broadcast__' 哨兵行（J-01 fix 的核心设计）】
//   同一行水位记录承载两套语义，靠 origin 列区分，互不干扰：
//     · origin == 真实来源 id 的行：记 acked_seq —— 「对端确认到该来源的哪个 seq」，
//       按来源序号(origin_seq)计量。每个被发送过的 origin 各占一行。
//     · origin == '__broadcast__' 的哨兵行：仅用于记 last_sent_seq —— 「对该对端的
//       广播已发到本地 changelog 的哪个 local_seq」，按本地序号计量。
//   为什么要拆成两套：已发送水位是「本地 changelog 视角（local_seq，全 origin 混排的
//   广播游标）」，而已确认水位是「来源视角（每个 origin 各自的 origin_seq）」，两者
//   计量单位与维度不同，硬塞进同一列会互相覆盖。J-01 fix 用专门的哨兵行把「广播发送
//   游标」与「逐 origin 的确认水位」彻底解耦。
//
// 【线程模型】
//   本类是无状态的纯方法集合（成员里没有任何字段），所有状态都落在 db 里。每个方法
//   都接收 QSqlDatabase& 现用现查。调用约定上由 SyncWorker 在其工作线程内串行调用，
//   配合外层事务保证原子性；本类自身不加锁、不开事务。
//
// 注释风格与 Errors.h / Types.h / RowPayload.h 一致：`// ──` 分节、中文、信息密集。
// 既有英文注释一律保留并翻译扩展，各 fix 标记（J-01 / C-02 / M-02 等）务必原样保留。
// ============================================================================

namespace dbridge::sync {

// ── OutboundAckStore —— __sync_outbound_ack 表的数据访问对象（DAO）────────────
//
// 【做什么】封装对「对外发送方向」ACK 水位表的全部读写，向 SyncWorker 提供语义化方法
//   （推进/查询 已确认水位、已发送水位，求裁剪下界、求重发下界，切换基线挂起标志）。
// 【为什么是一个独立的类】把零散的 SQL（UPSERT / MIN / LEFT JOIN）收拢到一处，让上层
//   只面对「水位」这一业务概念，而不必关心哨兵行、MAX 推进、NULL 兜底等 SQL 细节。
// 【状态/线程】无成员字段 → 无实例状态，天然可重入；状态全在传入的 db 中。详见文件头。
//
// Per-peer per-origin ACK water-mark stored in __sync_outbound_ack.
// （逐对端、逐来源 的 ACK 水位，存储于 __sync_outbound_ack 表中。）
class OutboundAckStore {
   public:
    // ── init —— 启动期自检：确认 __sync_outbound_ack 表存在且可用 ──────────────
    // 做什么：执行一条恒为空集的探针查询（WHERE 0），若表不存在/不可读则 SQL 报错。
    // 为什么用 "WHERE 0"：它让数据库做语义解析、确认表与列可访问，却不真正扫描/返回
    //   任何行，是一种零代价的「表健在性」探活手法（建表本身由 SyncDDL 负责，不在此处）。
    // 参数：db 当前同步库连接；err 可选出参，失败时填入数据库错误文本（可传 nullptr）。
    // 返回：true 表存在且可查；false 表缺失或不可读（此时 *err 已被填写）。
    // 副作用：无（只读探针，不改任何数据）。
    // 错误模式：仅在表确实不存在/驱动异常时失败 → 调用方应据此判定同步元数据未就绪。
    bool init(QSqlDatabase& db, QString* err);

    // ── updateAcked —— 推进某对端对某来源的「已确认水位」acked_seq ─────────────
    // 做什么：对 (peer, origin, epoch) 这一行做 UPSERT；不存在则插入，存在则把
    //   acked_seq 取「新旧两者较大值」更新（MAX 语义），并刷新 last_ack_ms 时间戳。
    // 为什么取 MAX（只增不减）：ACK 工件可能乱序到达或重复（传输层不保证有序/去重）。
    //   水位是「已确认到此为止」的累积量，绝不能因为一个迟到的旧 ACK 而回退，否则会把
    //   早已确认的变更误判为「仍未确认」，引发不必要的重发甚至 changelog 误删后的漏发。
    // 参数：peer 对端；origin 被确认变更的来源；epoch 流纪元；ackedSeq_ 本次 ACK 携带的
    //   appliedSeq（对端已应用到的来源序号）；err 可选错误出参。
    // 返回：true 成功（无论是插入还是 MAX 更新，乃至「旧值更大、实质未变」都算成功）。
    // 副作用：写一行/更新一行 __sync_outbound_ack；当被前台 sync() 等待时，本次推进可能
    //   令某个 PendingAckEntry 的 targetSeq 达成，从而推动整体状态走向 Completed。
    // 线程：由 SyncWorker 在处理收到的 ChangesetAck 时调用，须在其工作线程内。
    // 复杂度：O(1)（一次主键命中的 UPSERT）。
    //
    // Upsert acked_seq if the incoming value is higher.
    // （若传入的值更高，则 UPSERT 更新 acked_seq。）
    bool updateAcked(QSqlDatabase& db, const QString& peer, const QString& origin, qint64 epoch,
                     qint64 ackedSeq, QString* err);

    // ── ackedSeq —— 读某对端对某来源当前的「已确认水位」──────────────────────
    // 做什么：精确查询 (peer, origin, epoch) 一行的 acked_seq。
    // 用途：判断某条 (origin, seq) 变更是否已被特定对端确认（如完成判定、重发剪枝）。
    // 参数同上三元组。
    // 返回：该行的 acked_seq；若该三元组尚无记录（从未对其发过该来源的变更），返回 -1。
    // 副作用：无（纯读）。错误模式：SQL 失败或无行 → 一律返回 -1（调用方按「未确认」处理）。
    // 线程：SyncWorker 工作线程。复杂度：O(1)（主键命中）。
    //
    // Return current acked_seq for a specific (peer, origin, epoch). -1 if not found.
    // （返回指定 (peer, origin, epoch) 的当前 acked_seq；未找到则 -1。）
    qint64 ackedSeq(QSqlDatabase& db, const QString& peer, const QString& origin, qint64 epoch);

    // ── minAckedSeq —— 求「全体对端」对某来源都已确认的最小水位（changelog 裁剪下界）─
    // 做什么：对给定 (origin, epoch)，跨所有对端取 acked_seq 的 MIN，但排除处于基线
    //   挂起（pending_baseline=1）的对端。
    // 为什么取 MIN 且必须排除基线对端：changelog 只有在「所有仍需接收的对端都已确认」
    //   之后才能安全删除——任何一个对端没确认到，就不能删它还没拿到的那段，否则它将
    //   永久漏掉这些变更。取 MIN 即「木桶最短板」。而正在做基线的对端其 acked_seq 暂时
    //   还很低（基线铺完才会跟上），若把它算进来会把 MIN 压得极低、几乎删不了日志，甚至
    //   在某些路径下反而误删——故 pending_baseline=1 的对端被排除在裁剪计算之外。
    // 参数：origin 来源；epoch 流纪元（注意此方法不按 peer，而是横扫所有 peer）。
    // 返回：该来源被全体（非挂起）对端公认安全的最小已确认序号；若无任何相关行（或 MIN
    //   计算结果为 NULL）返回 -1（调用方按「没有可安全裁剪的下界」处理，即不裁剪）。
    // 副作用：无（纯读）。线程：SyncWorker。复杂度：O(命中行数)，受 (origin,epoch) 过滤。
    //
    // Return the minimum acked_seq across all peers for a given (origin, epoch).
    // Used as the changelog truncation watermark.  Returns -1 if no rows exist.
    // （返回给定 (origin, epoch) 下、跨所有对端的最小 acked_seq；用作 changelog
    //  裁剪水位；若无任何行则返回 -1。）
    qint64 minAckedSeq(QSqlDatabase& db, const QString& origin, qint64 epoch);

    // ── setPendingBaseline —— 切换某对端的「基线进行中」标志 ─────────────────
    // 做什么：把该 peer 名下「所有行」的 pending_baseline 一并置为 pending（1/0）。
    // 为什么按 peer 全量更新（不带 origin/epoch）：基线是「对该对端整体」的一次性对齐
    //   动作，期间它对所有来源的确认都不应参与裁剪计算，故一次性把它的全部水位行挂起；
    //   基线完成后再整体清零，使其重新计入 minAckedSeq。
    // 参数：peer 对端；pending true=进入基线挂起 / false=解除挂起；err 可选错误出参。
    // 返回：true 成功（即便没有命中任何行，UPDATE 影响 0 行也算成功）。
    // 副作用：更新该 peer 的若干行 pending_baseline，进而改变 minAckedSeq 的取值集合。
    // 线程：基线流程调用（同样在 SyncWorker 工作线程语境）。复杂度：O(该 peer 行数)。
    //
    // Toggle the pending_baseline flag for a peer.
    // （切换某个对端的 pending_baseline（基线挂起）标志。）
    bool setPendingBaseline(QSqlDatabase& db, const QString& peer, bool pending, QString* err);

    // ── lastSentLocalSeq —— 读对某对端的「广播已发送水位」（哨兵行，local_seq）────
    // 做什么：从 origin=='__broadcast__' 的哨兵行读 last_sent_seq（取 MAX 并 COALESCE
    //   兜底为 -1），即「已广播给该对端、覆盖到本地 changelog 的哪个 local_seq」。
    // 为什么用哨兵行而非复用 acked_seq：已发送水位以本地 changelog 的 local_seq 计量
    //   （一个混排了所有 origin 的广播游标），与逐 origin 的 acked_seq（按 origin_seq
    //   计量）维度不同；用专用哨兵行存储二者互不污染（J-01 fix 的设计要点，详见文件头）。
    // 参数：peer 对端；epoch 流纪元。
    // 返回：该对端的广播已发送 local_seq 上界；尚无哨兵行 / 查询失败 / 值为 NULL → 返回
    //   -1（表示「还没给它发过」，下次广播应从头考虑）。
    // 副作用：无（纯读）。线程：SyncWorker（广播前计算读取边界）。复杂度：O(1)。
    //
    // Return the last local_seq successfully sent to peer in the given epoch.
    // Uses a dedicated sentinel row (origin == "__broadcast__") to track the
    // broadcast send-watermark independently of per-origin acked_seq (J-01 fix).
    // Returns -1 if no record exists yet.
    // （返回给定 epoch 内、已成功发送给该对端的最后一个 local_seq。使用专门的哨兵行
    //  (origin == "__broadcast__") 来独立于逐 origin 的 acked_seq 跟踪广播发送水位
    //  —— J-01 修复。若尚无记录则返回 -1。）
    qint64 lastSentLocalSeq(QSqlDatabase& db, const QString& peer, qint64 epoch);

    // ── minUnackedLocalSeq —— 求「该对端最早一条尚未确认」对应的广播读取下界 ─────
    // 做什么：跨所有 origin，找出 __sync_changelog 中尚未被该对端 ACK 的最早一条变更，
    //   返回「其 local_seq 减 1」作为广播读取的下界（exclusive，即从它之后开始读）。
    // 为什么需要它（C-02 fix）：仅凭 last_sent_seq 推进读取游标会有漏洞——某条变更虽已
    //   「发送过」，但对端可能并未确认（丢包、应用失败）。若读取下界只跟着 last_sent_seq
    //   一路前移，这些未确认的变更将永远不会被重新读出、永远补发不上。把读取下界压到
    //   「最早一条未确认」处，确保任何未被 ACK 的变更在下次广播时仍然可被重新读取并重发。
    // 参数：peer 对端；epoch 流纪元（M-02 fix 后该参数不再进入 WHERE，仅为 API 兼容保留，
    //   实现内以 Q_UNUSED 标注；纪元匹配改由 LEFT JOIN 的三元键完成，详见 .cpp 注释）。
    // 返回：广播读取下界（exclusive）；若没有任何未确认变更则返回 -1（从头开始读，即
    //   下界为 -1，>-1 的都可读）。
    // 副作用：无（纯读）。线程：SyncWorker（广播前与 lastSentLocalSeq 配合定读取窗口）。
    // 复杂度：O(changelog 行数 × JOIN)，受 origin_seq > acked_seq 过滤后通常很小。
    //
    // C-02 fix: return the minimum local_seq in __sync_changelog that has NOT yet been
    // confirmed by this peer across all origins.  Used as the broadcast read lower-bound
    // so un-ACKed changesets are always eligible for re-send.  Returns -1 if nothing has
    // been sent (start from the beginning).
    // （C-02 修复：跨所有 origin，返回 __sync_changelog 中尚未被该对端确认的最小
    //  local_seq。用作广播读取下界，使未被 ACK 的变更集始终有资格被重发。若尚无任何
    //  发送则返回 -1（从头开始）。）
    qint64 minUnackedLocalSeq(QSqlDatabase& db, const QString& peer, qint64 epoch);

    // ── updateLastSent —— 推进对某对端的「广播已发送水位」（哨兵行，only-forward）─
    // 做什么：对哨兵行 (peer, '__broadcast__', epoch) 做 UPSERT；不存在则创建，存在则把
    //   last_sent_seq 取 MAX 更新（只前进、不回退），并刷新 last_ack_ms 时间戳。
    // 为什么只前进（MAX）：已发送水位是单调累积的进度，重复/乱序的发送回报不应使它倒退；
    //   配合 minUnackedLocalSeq 共同决定下次广播窗口（已发送≤但未确认的仍会被重读）。
    // 参数：peer 对端；epoch 流纪元；lastSentLocalSeq_ 本次广播达到的 local_seq 上界；
    //   err 可选错误出参。
    // 返回：true 成功（含「新值不大于旧值、实质未推进」也算成功）。
    // 副作用：插入或更新该对端的 '__broadcast__' 哨兵行；插入时 acked_seq 占位为 -1
    //   （哨兵行不参与确认语义，其 acked_seq 无意义，仅为满足 NOT NULL 列）。
    // 线程：SyncWorker（一次广播成功投递后调用以推进游标）。复杂度：O(1)。
    //
    // Advance the broadcast send-watermark for peer.  Only moves forward
    // (MAX semantics).  Creates the sentinel row if absent (J-01 fix).
    // （推进该对端的广播发送水位。只前进（MAX 语义）。哨兵行不存在则创建（J-01 修复）。）
    bool updateLastSent(QSqlDatabase& db, const QString& peer, qint64 epoch,
                        qint64 lastSentLocalSeq, QString* err);
};

}  // namespace dbridge::sync
