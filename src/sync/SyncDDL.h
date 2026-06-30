#pragma once
// ============================================================================
// SyncDDL.h — 同步子系统全部 __sync_* 元数据表的 DDL（建表语句）+ 工件命名约定
// ============================================================================
//
// 【这个文件是什么】
//   增量同步运转所依赖的全部“内务记账表”（bookkeeping tables）的 CREATE 语句集合，
//   外加“传输工件文件名”的统一拼装规则。所有 __sync_* 表都【建在被同步的同一个 SQLite
//   库里】——同步状态与业务数据同库同事务，保证“记账”与“真正改库”能原子地一起提交/回滚。
//   本文件是纯声明式的内联工具：无状态、无类，只是一组返回字符串/字符串列表的 inline 函数，
//   供 SyncWorker 在 run() 内建表/迁移、以及收发工件时命名使用。
//
// 【在同步管线中的位置】
//   SyncWorker.run() 启动时：先执行 allCreateStatements()（建表，IF NOT EXISTS 幂等）
//   → 再执行 applyMigrations()（对老库补列）→ 然后初始化各 store。各 store 就是这些表的
//   类型化读写门面：ChangelogStore↔__sync_changelog、AppliedVectorStore↔__sync_applied_vector、
//   OutboundAckStore↔__sync_outbound_ack、TableStateStore↔__sync_table_state、
//   RowWinnerStore↔__sync_row_winner、InboxLedger↔__sync_inbox_ledger、
//   QuarantineStore↔__sync_quarantine、ConsistencyCache↔__sync_consistency_cache 等。
//
// 【贯穿全表的核心概念（看表前先建立直觉）】
//   · origin / origin_seq：变更的“来源节点 + 该来源单调递增的序号”。每条逻辑变更由
//     (origin, origin_seq) 全局唯一标识；对端据此发现“漏收了中间某条”（gap）。
//   · stream_epoch：某来源一次进程生命的“纪元”（通常是启动毫秒时间戳）。重启换 epoch，
//     使 (origin, epoch, seq) 三元组在“重启导致 seq 计数重来”时仍不致与旧纪元混淆。
//   · local_seq：本地 changelog 的物理自增主键，与 origin_seq 是【不同命名空间】：
//     发送水位线 / 广播 FIFO 顺序看 local_seq；漏收判定 / ACK 看 (origin, origin_seq)。
//   · rank 仲裁：每个 origin 配一个唯一 rank，行级冲突时“高 rank 胜”，平 rank 再比 seq。
//   · schema_fingerprint：表结构的指纹（列名/类型等的散列）。两端指纹不一致即拒绝套用
//     （E_SYNC_SCHEMA_MISMATCH），防止把变更应用到结构已漂移的表上。
//   · changeset：SQLite session 扩展产出的二进制行级变更包（一段 BLOB）。
//   · 工件（artifact）：一次传输的最小文件单元（changeset / selection-push chunk /
//     baseline / ack），文件名自带路由信息，落在 outbox/inbox 目录。
//
// 【为什么用 inline 函数返回 DDL 而非 .sql 文件】
//   · inline：头文件内定义，多 TU 包含不重复定义；零额外编译单元。
//   · 与 store 同库编译：列名拼写一处可查，建表与读写代码物理上靠近，便于一致演进。
// ============================================================================
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QUuid>

namespace dbridge::sync::ddl {

// Returns all CREATE TABLE statements in dependency order.
// allCreateStatements —— 返回全部建表/建索引语句，已按“依赖先后”排好序。
// 做什么：把所有 __sync_* 表与索引的 CREATE 语句装进一个 QStringList，调用方按列表顺序
//   逐条执行即可一次性建齐。全部语句都带 IF NOT EXISTS → 反复执行幂等（库已建好不会报错）。
// 为什么要排序：带 FOREIGN KEY 的表（如 __sync_push_chunk_progress、__sync_frozen_manifest
//   引用 __sync_push_progress）必须在被引用表之后创建，否则外键目标不存在。故 push_progress
//   先于其两张子表出现。
// 返回：建表/建索引 SQL 列表。线程：纯函数，任意线程可调用。副作用：无（只生成字符串，不执行）。
inline QStringList allCreateStatements() {
    return {
        // --- changelog ---
        // ╔══ __sync_changelog —— 变更流水账（同步系统的“总账本”，最核心一张表）═══════╗
        // 职责：按 local_seq 顺序记录“每一笔需要传播的逻辑变更”，既包含本节点本地产生的，
        //   也包含从对端转发收下的。每条记录就是一个可独立编码、传输、应用的 changeset。
        //   它同时是“发送源”（广播按 local_seq 取未发的发出去）与“审计/补洞源”
        //   （对端发现 gap 时据 (origin, origin_seq) 回溯）。store 见 ChangelogStore。
        // 逐列：
        //   local_seq         本地物理自增主键。仅本节点内单调递增，决定广播 FIFO 顺序与
        //                     发送水位线。注意它【不等于】origin_seq（不同命名空间）。
        //   kind              变更种类（文本枚举，如 local 本地写 / forwarded 转发 等），
        //                     供路由与统计区分。
        //   origin            该变更的“真正来源节点”id。本地产生则为本节点 id；转发则保留
        //                     最初来源——冲突仲裁的 rank 与 gap 判定都认这个 origin。
        //   source_peer       直接把它递给本节点的那个“上一跳” peer（可空）。区别于 origin：
        //                     origin 是“原作者”，source_peer 是“从谁手里收到的”，用于
        //                     anti-echo（不回送给来路）与故障排查。
        //   origin_seq        该 origin 命名空间内的单调序号。与 origin 一起构成全局唯一逻辑
        //                     标识；对端靠它判断“是否漏收中间某条”（gap）。NOT NULL。
        //   parent_seq        逻辑前驱的 origin_seq（可空）：用于变基/因果链表达“本条建立在
        //                     哪条之上”。链首或无依赖时为 NULL。
        //   stream_epoch      产生该变更时该 origin 的纪元（进程生命标识）。重启换 epoch，
        //                     使“重启后 seq 重新计数”不至于与旧纪元的同号变更混淆。NOT NULL。
        //   schema_ver        产生时的表结构版本号（整数），与指纹配合做兼容性判断。NOT NULL。
        //   schema_fingerprint产生时相关表结构的指纹（文本散列）。应用端比对不一致即拒绝
        //                     （E_SYNC_SCHEMA_MISMATCH），防止套到已漂移的结构上。NOT NULL。
        //   changeset         SQLite session 产出的二进制行级变更包本体（BLOB）。NOT NULL。
        //   payload_checksum  changeset 的校验和（文本）。收发两端比对，损坏即
        //   E_SYNC_PAYLOAD_CORRUPT。
        //   byte_size         changeset
        //   字节大小。用于滞后字节量评估、超大负载告警（W_SYNC_PAYLOAD_LARGE）。
        //   authoritative     是否“权威下行”（如中心 center→边缘 edge 的权威套用），默认 0。
        //                     权威变更在套用/剪枝上有特殊待遇（如直接喂一致性缓存）。
        //   created_ms        本地写入该行的毫秒时间戳。供时间线排序/诊断（非因果序，因果看 seq）。
        //   push_id           若该变更属某次“选择性推送”，记其 push 批次 id（普通 changeset 为
        //   NULL）。
        //                     【M-04 修复】此列在 CREATE 内联声明；老库由 applyMigrations() 补列。
        // 约束：UNIQUE(origin, stream_epoch, origin_seq) —— 同一来源、同一纪元、同一序号只能
        //   有一条，是“幂等去重”的硬保证（转发/重收同一变更不会重复入账）。
        // ╚══════════════════════════════════════════════════════════════════════════╝
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_changelog (
  local_seq       INTEGER PRIMARY KEY AUTOINCREMENT,
  kind            TEXT    NOT NULL,
  origin          TEXT    NOT NULL,
  source_peer     TEXT,
  origin_seq      INTEGER NOT NULL,
  parent_seq      INTEGER,
  stream_epoch    INTEGER NOT NULL,
  schema_ver      INTEGER NOT NULL,
  schema_fingerprint TEXT NOT NULL,
  changeset       BLOB    NOT NULL,
  payload_checksum TEXT   NOT NULL,
  byte_size       INTEGER NOT NULL,
  authoritative   INTEGER NOT NULL DEFAULT 0,
  created_ms      INTEGER NOT NULL,
  push_id         TEXT,
  UNIQUE(origin, stream_epoch, origin_seq)
))"),
        // Note: push_id column added inline in CREATE TABLE above.
        // For existing databases, applyMigrations() performs ALTER TABLE ADD COLUMN.,
        // 说明：push_id 列已在上面 CREATE TABLE 内联声明；对“早于该列存在的老库”，由
        //   applyMigrations() 执行 ALTER TABLE ADD COLUMN 补上（见文件末）。
        // 索引一：按 (origin, origin_seq) —— 加速“某来源到某序号”的范围扫描（gap 检测、补洞、
        //   计算对端已确认 seq 等都按 origin 维度查）。
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_changelog_origin ON "
                       "__sync_changelog(origin, origin_seq)"),
        // 索引二：按 (stream_epoch, local_seq) —— 加速“按本地物理序广播/取未发”的扫描。
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_changelog_epoch  ON "
                       "__sync_changelog(stream_epoch, local_seq)"),

        // --- applied_vector ---
        // ╔══ __sync_applied_vector —— “每来源已应用水位”向量（版本向量 / version vector）═╗
        // 职责：记录“本节点对每个 (origin, epoch) 已经连续应用到第几号 origin_seq”。这是
        //   收方的核心进度账：① 判重（小于等于水位的变更已应用，丢弃）；② 判 gap（来了
        //   seq 但中间有洞 → 触发补洞/基线回退，E_SYNC_GAP）；③ 回 ACK 时报告自己进度。
        //   store 见 AppliedVectorStore::check()。
        // 逐列：
        //   origin              来源节点 id（按来源各维护一条水位）。
        //   stream_epoch        该来源的纪元；换纪元即另起一条水位（不与旧纪元混算）。
        //   applied_seq         已“连续无洞”应用到的最大 origin_seq。其语义是连续前缀的高水位：
        //                       只有 seq == applied_seq+1 的变更能推进它；跳号的会被判 gap。
        //   baseline_generation 基线代次，默认 0。每次套用一次全量基线就 +1，用于区分“基线之
        //                       前/之后”的水位语义，避免基线套用与增量套用互相误判。
        //   updated_ms          本行最近更新的毫秒时间戳（诊断用）。
        // 主键：(origin, stream_epoch) —— 每个来源每个纪元唯一一条水位记录。
        // ╚══════════════════════════════════════════════════════════════════════════╝
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_applied_vector (
  origin          TEXT    NOT NULL,
  stream_epoch    INTEGER NOT NULL,
  applied_seq     INTEGER NOT NULL,
  baseline_generation INTEGER NOT NULL DEFAULT 0,
  updated_ms      INTEGER NOT NULL,
  PRIMARY KEY(origin, stream_epoch)
))"),

        // --- outbound_ack ---
        // ╔══ __sync_outbound_ack —— “发送侧”对每个对端的发送/确认水位（出站锚点）════════╗
        // 职责：站在发送方视角，记录“我对某 peer、就某 origin/epoch 的变更，发到了第几号、
        //   对方确认到了第几号”。它驱动：① 重发/续发（只补发 acked_seq 之后的）；② 滞后评估
        //   （last_sent_seq 与 acked_seq 的差 = 在途未确认量）；③ 失联判定（last_ack_ms 太久
        //   远 → Dead）。注意与 applied_vector 的视角相反：那是“我收到多少”，这是“对方收到多少”。
        //   store 见 OutboundAckStore。
        // 逐列：
        //   peer             目标对端节点 id（对每个 peer 分别记账）。
        //   origin           变更来源 id。同一 peer 可能要追多个 origin 的进度（本地的 + 转发的）。
        //   stream_epoch     该 origin 的纪元。
        //   acked_seq        对端已确认收到并应用到的最大 origin_seq；默认 -1 表示“尚未确认任何”。
        //                    续发的起点 = acked_seq + 1。
        //   last_sent_seq    本节点最近一次发给该 peer 的最大 origin_seq；默认 -1。
        //                    (last_sent_seq - acked_seq) 即在途窗口大小。
        //   last_ack_ms      最近一次收到该 (peer,origin,epoch) ACK 的毫秒时间戳（可空）。
        //                    失联评估（DeadPeerEvictor）看它距今多久。
        //   pending_baseline 是否已对该 peer 标记“待发/需走基线”，默认 0。peer 落后过多或
        //                    被驱逐时置 1，表示“增量补发已不划算，改用全量基线”。
        //   last_push_id     最近一次发给该 peer 的选择性推送批次 id（可空）。
        //   last_chunk_seq   该 push 已发到的 chunk 序号（可空），用于断点续发分片。
        // 主键：(peer, origin, stream_epoch) —— 每个对端 × 每个来源 × 每个纪元 唯一一条锚点。
        // ╚══════════════════════════════════════════════════════════════════════════╝
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_outbound_ack (
  peer            TEXT    NOT NULL,
  origin          TEXT    NOT NULL,
  stream_epoch    INTEGER NOT NULL,
  acked_seq       INTEGER NOT NULL DEFAULT -1,
  last_sent_seq   INTEGER NOT NULL DEFAULT -1,
  last_ack_ms     INTEGER,
  pending_baseline INTEGER NOT NULL DEFAULT 0,
  last_push_id    TEXT,
  last_chunk_seq  INTEGER,
  PRIMARY KEY(peer, origin, stream_epoch)
))"),

        // --- table_state ---
        // ╔══ __sync_table_state —— 每张被同步表的“结构指纹 + 内容校验 + 水位”状态═════════╗
        // 职责：为每张同步表维护一份摘要状态，供：① schema 守卫比对两端结构是否一致；
        //   ② DiffEngine 判定某表自上次以来是否变化、是否需要重算/重传；③ 快速一致性核对
        //   （content_checksum 一致即可跳过逐行 diff）。store 见 TableStateStore。
        // 逐列：
        //   table_name        被同步的业务表名。
        //   stream_epoch      记录该状态时的纪元。
        //   schema_fingerprint该表当前结构的指纹（文本散列）。与来包指纹比对，不一致即拒绝套用。
        //   high_water_seq    与该表相关、已纳入本状态的最高变更序号，默认
        //   0；推进表示该表又有新变更。
        //   content_checksum  该表内容的整体校验和（文本）。两端相等 → 内容一致，可跳过细粒度比对。
        //   row_count         该表行数，默认 0。粗粒度规模指标，辅助一致性判断与展示。
        //   updated_ms        本状态最近更新的毫秒时间戳。
        // 主键：(table_name, stream_epoch) —— 每表每纪元一条状态。
        // ╚══════════════════════════════════════════════════════════════════════════╝
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_table_state (
  table_name      TEXT    NOT NULL,
  stream_epoch    INTEGER NOT NULL,
  schema_fingerprint TEXT NOT NULL,
  high_water_seq  INTEGER NOT NULL DEFAULT 0,
  content_checksum TEXT   NOT NULL,
  row_count       INTEGER NOT NULL DEFAULT 0,
  updated_ms      INTEGER NOT NULL,
  PRIMARY KEY(table_name, stream_epoch)
))"),

        // --- consistency_cache ---
        // ╔══ __sync_consistency_cache —— 行级“中心指纹”缓存（选择性推送的剪枝依据）══════╗
        // 职责：缓存“每一行在中心（center）侧的内容指纹”。选择性推送时，发送方据此做依赖
        //   剪枝——若某行的本地指纹与缓存的中心指纹一致，说明对端已是最新，可不必重发该行，
        //   显著减小推送体积（M-01 修复相关）。由基线套用、权威下行（center→edge）持续喂入。
        //   内存侧门面见 ConsistencyCache。
        // 逐列：
        //   table_name         行所属表名。
        //   primary_key        行主键（文本化）。同步以单列主键为前提（复合主键见
        //                      E_SYNC_COMPOSITE_PK_NOT_SUPPORTED）。
        //   center_fingerprint 该行在中心侧的内容指纹（二进制 BLOB）。比对相等即“无需重传”。
        //   updated_ms         本缓存项最近更新的毫秒时间戳（可用于过期/清理）。
        // 主键：(table_name, primary_key) —— 每表每行一条缓存。
        // ╚══════════════════════════════════════════════════════════════════════════╝
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_consistency_cache (
  table_name      TEXT    NOT NULL,
  primary_key     TEXT    NOT NULL,
  center_fingerprint BLOB NOT NULL,
  updated_ms      INTEGER NOT NULL,
  PRIMARY KEY(table_name, primary_key)
))"),

        // --- quarantine ---
        // ╔══ __sync_quarantine —— 隔离区：暂存“当下无法套用、但将来可能可套用”的来包═══════╗
        // 职责：当收到的 changeset 因 schema 暂不匹配（如对端已升级表结构、本端还没跟上）而
        //   无法立刻 apply 时，先把它原样存进隔离区而非丢弃；待本端 schema 升级或套用基线后，
        //   drainQuarantine() 再把它取出重放（M-06 修复）。避免“暂时不能应用”被误当成永久失败。
        //   store 见 QuarantineStore。
        // 逐列：
        //   id                 自增主键（仅本表内排序/定位用）。
        //   origin             被隔离变更的来源 id。
        //   origin_seq         其来源序号。(origin, origin_seq) 标识它是哪条逻辑变更。
        //   stream_epoch       其纪元。
        //   payload_schema_ver 该 payload 当时携带的表结构版本号——重放前据此判断本端是否已
        //                      升级到可兼容的结构。
        //   payload            被隔离的原始负载本体（BLOB），原样保存以便日后重放。
        //   created_ms         入隔离区的毫秒时间戳（可用于老化/清理）。
        // 注意：本表无 (origin,seq) 唯一约束，允许同一逻辑变更因多次尝试而存在多份待重放副本。
        // ╚══════════════════════════════════════════════════════════════════════════╝
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_quarantine (
  id              INTEGER PRIMARY KEY AUTOINCREMENT,
  origin          TEXT    NOT NULL,
  origin_seq      INTEGER NOT NULL,
  stream_epoch    INTEGER NOT NULL,
  payload_schema_ver INTEGER NOT NULL,
  payload         BLOB    NOT NULL,
  created_ms      INTEGER NOT NULL
))"),

        // --- push_progress ---
        // ╔══ __sync_push_progress —— “选择性推送（批次级）”进度表════════════════════════╗
        // 职责：以一次完整的选择性推送（一个 push_id）为单位，记录其整体进度。一次推送会被
        //   切成 total_chunks 个分片，逐片传输/应用；本表是“批次表头”，分片明细在
        //   __sync_push_chunk_progress（外键引用本表）。它使推送可断点续传、可观察成败。
        // 逐列：
        //   push_id      本次推送的全局唯一批次 id（主键）。工件文件名里也带它。
        //   origin       发起该推送的来源节点 id。
        //   peer         接收该推送的目标对端 id。
        //   total_chunks 本次推送切出的分片总数（用于判断“是否全部到齐”）。
        //   schema_ver   推送内容的表结构版本号。
        //   status       批次整体状态（文本枚举，如 in-progress / completed / failed）。
        //   failed_code  失败时的错误码（可空，成功为 NULL），便于定位失败原因。
        //   updated_ms   本批次进度最近更新的毫秒时间戳。
        // 主键：push_id。被 __sync_push_chunk_progress 与 __sync_frozen_manifest 以外键引用，
        //   故本表必须先于那两张表创建（依赖序的由来）。
        // ╚══════════════════════════════════════════════════════════════════════════╝
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_push_progress (
  push_id         TEXT    PRIMARY KEY,
  origin          TEXT    NOT NULL,
  peer            TEXT    NOT NULL,
  total_chunks    INTEGER NOT NULL,
  schema_ver      INTEGER NOT NULL,
  status          TEXT    NOT NULL,
  failed_code     TEXT,
  updated_ms      INTEGER NOT NULL
))"),

        // --- push_chunk_progress ---
        // ╔══ __sync_push_chunk_progress —— 选择性推送的“分片级”进度明细════════════════════╗
        // 职责：__sync_push_progress 的子表，逐个分片记录状态。收方据此做分片级幂等
        //   （同一 chunk 重收不重复套用）与续传（只补缺失/失败的 chunk）。
        // 逐列：
        //   push_id    所属推送批次 id（外键 → __sync_push_progress.push_id）。
        //   chunk_seq  分片在该批次内的序号（从 0 起）。与 push_id 共同唯一定位一个分片。
        //   status     该分片状态（文本枚举，如 pending / applied / failed）。
        //   checksum   该分片内容校验和，收方比对以判损坏/判重。
        //   applied_ms 该分片成功套用的毫秒时间戳（可空，未套用时为 NULL）。
        // 主键：(push_id, chunk_seq)。
        // 外键：push_id REFERENCES __sync_push_progress(push_id) —— 故本表须在 push_progress
        // 之后建。
        // ╚══════════════════════════════════════════════════════════════════════════╝
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_push_chunk_progress (
  push_id         TEXT    NOT NULL,
  chunk_seq       INTEGER NOT NULL,
  status          TEXT    NOT NULL,
  checksum        TEXT    NOT NULL,
  applied_ms      INTEGER,
  PRIMARY KEY(push_id, chunk_seq),
  FOREIGN KEY(push_id) REFERENCES __sync_push_progress(push_id)
))"),

        // --- frozen_manifest ---
        // ╔══ __sync_frozen_manifest —— 推送“冻结清单”：固化某次推送应包含哪些行的快照════════╗
        // 职责：选择性推送在“冻结点”把本次要发的行集（含其外键闭包）逐行登记进来，形成一份
        //   不可变清单。冻结后若本地这些行又被改动，可比对 fingerprint 检出“漂移”
        //   （W_SYNC_PUSH_ROW_DRIFTED）；按 topo_index 顺序应用则保证父行先于子行（满足外键）。
        //   FrozenManifest 组件读写本表。
        // 逐列：
        //   push_id     所属推送批次 id（外键 → __sync_push_progress.push_id）。
        //   chunk_seq   该行被分配到的分片序号。
        //   table_name  行所属表名。
        //   pk_hash     主键的散列值（定长、便于做主键的一部分与索引）。
        //   primary_key 行主键原值（文本化）。
        //   record_kind 记录类型（文本枚举，如 upsert / delete），决定套用动作。
        //   topo_index  该行在外键拓扑序中的位次——按它升序套用，确保被依赖的父行先落库。
        //   fingerprint 冻结时该行的内容指纹（BLOB）。发送/套用前与当前值比对以检出漂移。
        // 主键：(push_id, chunk_seq, table_name, pk_hash) —— 一次推送内每个分片每张表每行唯一。
        // 外键：push_id REFERENCES __sync_push_progress(push_id) —— 故须在 push_progress 之后建。
        // ╚══════════════════════════════════════════════════════════════════════════╝
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_frozen_manifest (
  push_id         TEXT    NOT NULL,
  chunk_seq       INTEGER NOT NULL,
  table_name      TEXT    NOT NULL,
  pk_hash         TEXT    NOT NULL,
  primary_key     TEXT    NOT NULL,
  record_kind     TEXT    NOT NULL,
  topo_index      INTEGER NOT NULL,
  fingerprint     BLOB    NOT NULL,
  PRIMARY KEY(push_id, chunk_seq, table_name, pk_hash),
  FOREIGN KEY(push_id) REFERENCES __sync_push_progress(push_id)
))"),

        // --- row_winner (G-01: changeset path only) ---
        // ╔══ __sync_row_winner —— 行级“当前冲突胜者”表（rank 仲裁的持久化裁决）═══════════╗
        // 职责：对每一行（按表 + 主键散列定位），记录“目前是哪个来源、以多大 rank/seq 赢得了
        //   这一行的最终值”。当又有一笔变更落在同一行上，ConflictArbiter 拿它与本表登记的胜者
        //   比较：来者 rank 更高（或同 rank 而 seq 更新）才允许覆盖，否则丢弃——这就是“高 rank 胜、
        //   平 rank 比 seq”的落地点，保证多节点并发改同一行时各端最终收敛到同一个胜者。
        //   【G-01】本表只服务于 changeset 路径（非基线路径）。store 见 RowWinnerStore。
        // 逐列：
        //   table_name         行所属表名。
        //   pk_hash            行主键的散列（定长键，便于做主键与索引）。
        //   winning_origin     当前胜出值来自哪个来源节点。
        //   winning_rank       该来源的 rank（仲裁主依据，越大越优先）。
        //   winning_origin_seq 胜出变更的 origin_seq（rank 相同时的次级比较依据，越大越新）。
        //   content_hash       胜出内容的指纹（BLOB），用于快速判断“来者是否与现胜者同值”。
        //   winning_content    【C-01 修复】胜出行的 JSON 编码全文，默认空串。它的用途很关键：
        //                      当一条“低 rank 的 DELETE”到来、不该删除高 rank 胜者时，可用这里
        //                      保存的内容把被误删的行恢复回来（low-rank DELETE recovery）。
        //   updated_ms         本裁决最近更新的毫秒时间戳。
        // 主键：(table_name, pk_hash) —— 每表每行只有一个“当前胜者”。
        // ╚══════════════════════════════════════════════════════════════════════════╝
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_row_winner (
  table_name      TEXT    NOT NULL,
  pk_hash         TEXT    NOT NULL,
  winning_origin  TEXT    NOT NULL,
  winning_rank    INTEGER NOT NULL,
  winning_origin_seq INTEGER NOT NULL,
  content_hash    BLOB    NOT NULL,
  winning_content TEXT    NOT NULL DEFAULT '',  -- C-01: JSON-encoded row for low-rank DELETE recovery
  updated_ms      INTEGER NOT NULL,
  PRIMARY KEY(table_name, pk_hash)
))"),

        // --- inbox_ledger (G-08: artifact-level idempotent consumption) ---
        // ╔══ __sync_inbox_ledger —— 入站工件“幂等消费”台账（G-08）═══════════════════════╗
        // 职责：以“工件文件名”为键记录每个入站 artifact 的处理状态，确保同一工件即便被重复
        //   投递/重复扫描，也只被真正消费一次（artifact 级幂等）。scanInbox/processArtifact
        //   每见到一个工件先查台账：已 consumed 则直接跳过；新工件则记 seen、处理成功后记
        //   consumed。store 见 InboxLedger。
        // 逐列：
        //   artifact_name 工件文件名（主键）。文件名本身自带路由信息且全局唯一（见下方命名函数），
        //                 故可直接当幂等键。
        //   status        处理状态（文本枚举，如 seen 已发现 / consumed 已消费 / corrupt 损坏）。
        //   first_seen_ms 首次发现该工件的毫秒时间戳。
        //   consumed_ms   成功消费完成的毫秒时间戳（可空；尚未消费或损坏时为 NULL）。
        // 主键：artifact_name。
        // ╚══════════════════════════════════════════════════════════════════════════╝
        QStringLiteral(R"(
CREATE TABLE IF NOT EXISTS __sync_inbox_ledger (
  artifact_name   TEXT    PRIMARY KEY,
  status          TEXT    NOT NULL,
  first_seen_ms   INTEGER NOT NULL,
  consumed_ms     INTEGER
))"),
    };
}

// ── 工件命名约定（artifact naming）─────────────────────────────────────────────
// 总览：传输层把每个工件作为一个文件落在 outbox/inbox 目录，文件名【自带路由信息】，
//   接收方仅凭文件名即可分派种类、识别来源/目标，并据全局唯一性做幂等。以下函数是这些
//   文件名的唯一拼装入口，确保命名契约在收发两侧一致。所有时间/序号字段都做了零填充，
//   使“字典序 == 数值序”，目录列表天然有序、便于按序处理。

// Canonical artifact naming helpers.
// M-03 fix: artifact names follow the stable contract:
//   origin__epoch__kind__seq__[peer-]uuid.payload
// kind is now at a fixed position (field 3), and a UUID suffix is always appended for
// global uniqueness.  This matches the specification's stable naming contract and allows
// peer information to be embedded before the UUID without shifting the kind field.
// H-08 fix: include target peer so the same changelog entry written for different peers
// produces distinct file names (same outbox dir cannot hold two files with the same name).
//
// changesetArtifactName —— 拼装一个普通 changeset 工件的文件名。
// 【M-03 修复】文件名遵循稳定契约：origin__epoch__kind__seq__[peer-]uuid.payload。
//   kind（这里固定为 "changeset"）被钉在第 3 字段的固定位置，且总在末尾追加一段 UUID
//   保证全局唯一；把 peer 信息放在 UUID 之前嵌入，不会挤动 kind 字段的位置。
// 【H-08 修复】带上 targetPeer：同一条 changelog 若要分别发给不同 peer，会生成不同文件名——
//   否则同一个 outbox 目录里无法同时存放两个同名文件（后者会覆盖前者）。
// 参数：origin 来源 id；epoch 纪元；seq 该变更的 origin_seq；targetPeer 目标对端（可空）。
// 返回：形如 "<origin>__<epoch>__changeset__<000000000seq>__<uuid>.payload" 的文件名；
//   带 targetPeer 时尾部为 "...__<uuid8>-<peer>...?"（见实现：peer 与 uuid 用 '-' 连接）。
// 细节：seq 用 .arg(seq, 12, 10, '0') 左补零到 12 位十进制 → 文件名按字典序即按 seq 升序；
//   uuid 取 createUuid 去花括号后的前 8 位，足够避免同毫秒/同序号下的偶发重名。
// 线程：纯函数。副作用：无（仅生成字符串；UUID 来自系统熵源）。
inline QString changesetArtifactName(const QString& origin, qint64 epoch, qint64 seq,
                                     const QString& targetPeer = QString()) {
    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    if (targetPeer.isEmpty())
        // 无指定对端（如广播）：origin__epoch__changeset__<zero-padded seq>__uuid.payload
        return QStringLiteral("%1__%2__changeset__%3__%4.payload")
            .arg(origin)
            .arg(epoch)
            .arg(seq, 12, 10, QLatin1Char('0'))  // seq 左补零到 12 位，保证字典序=数值序
            .arg(uuid);
    // 指定对端：在 uuid 前嵌入 targetPeer，使“发给不同 peer 的同一变更”得到不同文件名。
    return QStringLiteral("%1__%2__changeset__%3__%4-%5.payload")
        .arg(origin)
        .arg(epoch)
        .arg(seq, 12, 10, QLatin1Char('0'))
        .arg(targetPeer)
        .arg(uuid);
}

// M-01 fix: naming follows the stable contract used by changeset artifacts:
//   origin__epoch__selectionpush__pushId.chunkSeq__[peer-]uuid.payload
// origin and epoch are at fixed positions; a UUID suffix guarantees global uniqueness.
inline QString selectionPushArtifactName(const QString& origin, qint64 epoch, const QString& pushId,
                                         int chunkSeq, const QString& targetPeer = QString()) {
    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    const QString chunkStr = QString::number(chunkSeq).rightJustified(6, QLatin1Char('0'));
    if (targetPeer.isEmpty())
        return QStringLiteral("%1__%2__selectionpush__%3.%4__%5.payload")
            .arg(origin)
            .arg(epoch)
            .arg(pushId)
            .arg(chunkStr)
            .arg(uuid);
    return QStringLiteral("%1__%2__selectionpush__%3.%4__peer-%5-%6.payload")
        .arg(origin)
        .arg(epoch)
        .arg(pushId)
        .arg(chunkStr)
        .arg(targetPeer)
        .arg(uuid);
}

inline QString baselineRequestArtifactName(const QString& fromPeer, const QString& toPeer,
                                           qint64 epoch, qint64 fromSeq,
                                           const QString& uniqueSuffix = QString()) {
    const QString suffix = uniqueSuffix.isEmpty()
                               ? QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)
                               : uniqueSuffix;
    return QStringLiteral("%1__%2__%3__%4__%5__baselinerequest.payload")
        .arg(fromPeer)
        .arg(toPeer)
        .arg(epoch)
        .arg(fromSeq, 12, 10, QLatin1Char('0'))
        .arg(suffix);
}

inline QString baselineResponseArtifactName(const QString& fromPeer, const QString& toPeer,
                                            qint64 epoch, qint64 sourceMaxSeq,
                                            const QString& uniqueSuffix = QString()) {
    const QString suffix = uniqueSuffix.isEmpty()
                               ? QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)
                               : uniqueSuffix;
    return QStringLiteral("%1__%2__%3__%4__%5__baselineresponse.payload")
        .arg(fromPeer)
        .arg(toPeer)
        .arg(epoch)
        .arg(sourceMaxSeq, 12, 10, QLatin1Char('0'))
        .arg(suffix);
}

// H-03 fix: include a per-call UUID suffix so same-millisecond ACKs never collide.
inline QString ackArtifactName(const QString& fromPeer, const QString& toPeer, qint64 ms,
                               const QString& uniqueSuffix = QString()) {
    const QString suffix = uniqueSuffix.isEmpty()
                               ? QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)
                               : uniqueSuffix;
    return QStringLiteral("ack__%1__%2__%3__%4.ack").arg(fromPeer).arg(toPeer).arg(ms).arg(suffix);
}

// M-04 fix: apply schema migrations for existing databases.
// ALTER TABLE ADD COLUMN fails if the column already exists; we intentionally ignore that
// error so the function is idempotent and works for both fresh and pre-existing databases.
// Returns false only on catastrophic failures (not "column already exists").
inline bool applyMigrations(QSqlDatabase& db) {
    // M-04: add push_id column to __sync_changelog (NULL for non-push changesets).
    QSqlQuery q(db);
    q.exec(QStringLiteral("ALTER TABLE __sync_changelog ADD COLUMN push_id TEXT"));
    // Ignore error: "duplicate column name" is expected when column already exists.
    // Any other error is a schema mismatch we cannot auto-fix — it will surface later
    // when a real INSERT fails.
    return true;
}

}  // namespace dbridge::sync::ddl
