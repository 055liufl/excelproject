#pragma once
#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

// ============================================================================
// SyncTypes.h — 增量同步子系统的核心“值类型”词汇表
// ============================================================================
//
// 【这个文件是什么】
//   dbridge 同步子系统（include/dbridge/sync/*）对外/对内共享的一组纯数据 struct
//   与 enum。它们只装数据、无行为，是各模块之间传递“同步事实”的标准信封：
//     · 引擎状态/进度/日志/错误（SyncState / SyncProgress / SyncLogEntry / SyncError）
//     · 一次同步的最终战报（SyncResult / PeerSyncState）
//     · 在节点间传输的“变更工件（artifact）”的负载结构（PayloadHeader / *Body /
//       *Payload / DecodeResult）
//     · 收到变更后回执用的 ACK（ChangesetAck / PushChunkAck / PendingAckEntry）
//     · 调用方主动写库时描述“要改哪一行”的 RowMutation
//
// 【先建立一张同步全景图（贯穿全文件的术语）】
//   同步采用“工件文件交换”模型，每一次本地写入都会沿如下管线流向其它节点：
//     本地写库（被 SQLite session 捕获）
//       → 生成 changeset（变更集二进制）
//       → 打包成 artifact 文件，含 PayloadHeader 头 + 负载体
//       → 写入本节点 outbox（发件箱目录），由传输层投递到对端 inbox（收件箱目录）
//       → 对端读取 → 解码（得到 DecodeResult）→ apply（应用到本地库）
//       → 遇同一行被多方改动则做 conflict 仲裁（rank 优先级 + seq 序号）
//       → 应用成功后回 ACK，发送方据此推进“已确认水位”（watermark）
//
// 【几个反复出现的关键标识，务必先吃透】
//   · origin（来源节点 id）：这条变更“最初”是在哪个节点产生的。注意它不等于
//     “是谁把文件递给我的”——中心节点会转发别人的变更，此时转发者另记在
//     PayloadHeader::senderPeer。
//   · originSeq（来源序号）：同一 origin 产生的变更的单调递增编号。(origin, seq)
//     合起来唯一标识一条变更，也是检测“序号空洞 = 漏收”（E_SYNC_GAP）的依据。
//   · streamEpoch（流纪元）：当某节点做了基线重置等“断流”操作后会换一个新纪元，
//     使新旧两段序号流互不混淆；几乎所有水位/状态都按 (origin, streamEpoch) 分桶。
//   · rank（仲裁优先级）：见 SyncConfig::originPriority；冲突时先比 rank，rank 相同
//     再比 seq，保证多节点对“同一行最终值”达成确定性一致。
//   · schemaFingerprint / schemaVer（表结构指纹/版本）：随负载同行，接收端据此校验
//     两端表结构是否一致（不一致 → E_SYNC_SCHEMA_MISMATCH）。
//
// 错误码字符串集中定义于 Errors.h；本文件只定义承载它们的结构（如 SyncError）。
// 注释风格与 Types.h / RowPayload.h 一致：POD 信封、逐字段中文详注。
// ============================================================================

namespace dbridge::sync {

// 节点角色。同步拓扑为“星型”：一个 Center（中心）+ 若干 Edge（边缘）。
//   · Center：枢纽，负责汇聚各边缘的变更并转发给其它边缘（充当中转）。
//   · Edge：叶子节点，只与中心交换；其 SyncConfig 必须指定 centerNodeId。
enum class NodeRole { Center, Edge };

// 同步引擎的“前台操作状态机”。一次手动 sync()/syncSelected() 会在这些状态间推进；
// 三个终态（Completed/Stopped/Failed）到达后前台门控（ForegroundGate）即被释放。
//   Idle         空闲，未在执行任何前台操作。
//   Capturing    正在捕获/冻结本地变更（选择性推送的预备阶段）。
//   Exporting    正在把本地变更打包成 artifact 写入 outbox（drain 发件箱）。
//   Importing    正在从 inbox 读取对端变更准备应用（导入方向）。
//   Broadcasting 正在向各对端广播负载。
//   Completed    正常完成（且若需要 ACK，已收齐所有对端的 ACK）。
//   Stopped      被 stop() 协作式中止（非错误）。
//   Failed       因错误而中止（传输失败 / ACK 超时 / 应用约束失败等）。
enum class SyncState {
    Idle,
    Capturing,
    Exporting,
    Importing,
    Broadcasting,
    Completed,
    Stopped,
    Failed
};

// 冲突解决策略：当同一行被本地与远端同时改动、且 rank+seq 仲裁交由策略决定时采用。
//   SourceWins  来源（发来变更的一方）胜：用远端值覆盖本地。
//   TargetWins  目标（本地）胜：保留本地值，丢弃远端改动。
//   Manual      人工介入：不自动覆盖，记为冲突待用户裁决。
enum class ConflictPolicy { SourceWins, TargetWins, Manual };

// 严重级别。用于 SyncLogEntry/SyncError 分级。Error 与 Fatal 会令前台操作进入 Failed；
// Info/Warning 不阻断流程，仅作诊断。
enum class Severity { Info, Warning, Error, Fatal };

// SyncProgress —— 同步进度快照（可被 ISyncEngine::progress() 随时拉取）。
// 由 SyncWorker 后台线程更新、引擎加锁拷贝后返回，故各字段是“某一刻的截面值”。
struct SyncProgress {
    SyncState state = SyncState::Idle;  // 当前前台状态机所处状态
    int percent = -1;                   // 进度百分比 [0,100]；-1 表示“未知/不适用”
    qint64 bytesPacked = 0;   // 已打包（写入 outbox）的字节数（出方向累计）
    qint64 bytesApplied = 0;  // 已应用（从 inbox 落库）的字节数（入方向累计）
    int changesApplied = 0;   // 已成功应用的行变更条数
    int conflicts = 0;        // 本次累计发生的冲突次数
};

// PeerSyncState —— 单个对端节点的同步进度画像（汇总进 SyncResult::peers）。
// 用于诊断“某个对端是否跟得上、是否已被驱逐”。
struct PeerSyncState {
    QString nodeId;            // 对端节点 id
    qint64 lastAckedSeq = -1;  // 对端已 ACK 确认到的最大序号（-1 = 尚无确认）
    qint64 lastSentSeq = -1;   // 本地已发往该对端的最大序号（-1 = 尚未发送）
    // 是否“滞后”：lastSentSeq - lastAckedSeq 等指标超过软阈值
    // （见 SyncConfig::peerLagSoft*），仅告警、不阻断。
    bool lagging = false;
    // 是否被“驱逐”：滞后超过硬阈值（peerLagHard*）后停止为其堆积 outbox，
    // 避免发件箱无限膨胀拖垮本节点。
    bool evicted = false;
};

// SyncLogEntry —— 一条同步日志（环形缓冲，引擎内限长保存，见 SyncEngine::appendLog）。
struct SyncLogEntry {
    qint64 epochMs = 0;                  // 产生时刻（Unix 毫秒时间戳）
    Severity severity = Severity::Info;  // 级别
    QString phase;                       // 所处阶段标签（如 "init"/"sync"/"apply"）
    QString message;                     // 人类可读描述
};

// SyncError —— 一条同步错误/警告的统一载体（同步路径上 Errors.h 错误码的“信封”）。
// 与 ETL 路径的 RowError 平行：那边按行/列定位，这边按阶段/节点定位。
struct SyncError {
    QString code;  // 错误码字符串，取值见 Errors.h（E_SYNC_* / W_SYNC_*）
    Severity severity = Severity::Error;  // 级别；Error/Fatal 会令前台操作判定为 Failed
    QString phase;                        // 出错阶段（"init"/"sync"/"apply"/"transport"…）
    QString nodeId;                       // 关联节点 id（通常是本地 nodeId）
    QString message;                      // 详细描述
};

// SyncResult —— 一次（已完成的）同步操作的最终战报，由 result() 返回。
struct SyncResult {
    bool ok = false;  // 整体是否成功（finalState==Completed 时为 true）
    SyncState finalState = SyncState::Idle;  // 收束时的终态（Completed/Stopped/Failed）
    int payloadsSent = 0;                    // 本次发出的 artifact 个数
    int payloadsApplied = 0;                 // 本次应用的 artifact 个数
    int changesApplied = 0;                  // 成功落库的行变更条数
    int conflicts = 0;                       // 发生的冲突次数
    QList<PeerSyncState> peers;              // 各对端的进度画像
    QList<SyncError> errors;                 // 本次过程中累积的错误
};

// PayloadHeader —— 每个 artifact（变更工件文件）都携带的“信封头”。
// 接收端先读头做路由/校验/幂等判定，再据 kind 决定如何解码负载体。
struct PayloadHeader {
    QString origin;  // 变更的“原始产生节点”id（≠ 转发者，见 senderPeer）
    qint64 originSeq = 0;  // 该 origin 的单调递增序号；(origin,seq) 唯一标识一条变更
    qint64 parentSeq = 0;  // 上一条变更的序号（构成链，用于检测空洞/排序）
    QString schemaFingerprint;  // 表结构指纹；与本地不一致 → E_SYNC_SCHEMA_MISMATCH
    qint64 schemaVer = 0;       // 表结构版本号（配套指纹做粗粒度版本闸）
    qint64 streamEpoch = 0;     // 流纪元；基线重置后递增，隔离新旧序号流
    QString routeTag;           // 路由标签（多路/分流场景的可选标记）
    QString pushId;             // 选择性推送的批次 id；仅 SelectionPush 非空
    int chunkSeq = 0;     // 分片序号（大负载切分为多片传输时，本片的序号）
    int totalChunks = 0;  // 该 pushId 批次的总分片数
    // C-05 fix：本工件的“物理发送者”——把文件写进 outbox 的那个节点。
    // 当中心节点转发某个远端 origin 的变更时，senderPeer 会与 origin 不同。
    // 接收端把 senderPeer 作为 ACK.toPeer 回执，使中心的 outbound_ack 水位得以推进。
    QString senderPeer;
};

// 负载种类：决定 DecodeResult 中哪个负载体字段有效。
//   Changeset        普通增量变更集（最常见，对应 DecodeResult::changeset）。
//   SelectionPush    选择性推送（带冻结清单 + 行数据，见 SelectionPushBody）。
//   BaselineRequest  请求对端导出完整基线（新节点入网/补齐用）。
//   BaselineResponse 携带基线全量数据的应答。
enum class PayloadKind { Changeset, SelectionPush, BaselineRequest, BaselineResponse };

// FrozenEntry —— 选择性推送时，被“冻结”的一行的清单项。
// 冻结 = 在发送前对选中行拍快照并记录指纹，以便检测发送窗口内该行是否又被改动
// （漂移 → W_SYNC_PUSH_ROW_DRIFTED）。
struct FrozenEntry {
    QString table;       // 行所属表
    QString primaryKey;  // 行主键（业务可读形式）
    QString pkHash;      // 主键哈希（定长、便于做索引/比对的键）
    QString recordKind;  // "selected"（用户显式选中）| "dependency"（外键闭包带入）
    int topoIndex = 0;   // 拓扑序下标：保证父表先于子表应用，满足外键依赖
    QByteArray fingerprint;  // 行内容指纹（用于漂移检测/一致性比对）
};

// SelectionPushBody —— 选择性推送负载体（PayloadKind::SelectionPush 时有效）。
struct SelectionPushBody {
    QList<FrozenEntry> frozenEntries;  // 冻结清单（与 rows 同序、一一对应）
    // 实际行数据；rows[i] 即 frozenEntries[i] 描述的那一行的列值映射。
    QList<QVariantMap> rows;
    QString pushId;       // 本次推送批次 id（与 PayloadHeader::pushId 一致）
    int chunkSeq = 0;     // 本片序号
    int totalChunks = 0;  // 总片数
};

// BaselineRequestPayload —— “请求基线”负载体（PayloadKind::BaselineRequest 时有效）。
// 典型场景：边缘节点检测到序号空洞或刚入网，向中心请求某些表的全量基线以对齐。
struct BaselineRequestPayload {
    QString origin;               // 请求方节点 id
    qint64 streamEpoch = 0;       // 请求方当前流纪元
    QStringList requestedTables;  // 需要补齐的表清单（空可表示全部同步表）
    qint64 fromSeq = 0;  // 请求方已有的起点序号（应答只需补这之后的内容）
    QString pendingArtifactName;  // 关联的待处理工件名（用于把应答与请求配对）
};

// C-03 fix：基线应答里随附的“每个 origin 的 applied-vector 切点”。
// 携带 stream_epoch，使接收端能以正确的纪元调用
// av.resetTo(origin, epoch, seq, generation)，而非用本地 streamEpoch_ 去猜。
struct BaselineOriginCut {
    QString origin;          // 该切点对应的来源节点
    qint64 streamEpoch = 0;  // 切点所在的流纪元
    qint64 appliedSeq = 0;   // 基线已涵盖到该 origin 的此序号为止
};

// BaselineResponsePayload —— “基线应答”负载体（PayloadKind::BaselineResponse 时有效）。
// 携带导出端的全量数据快照 + 各 origin 的水位切点，接收端据此重置本地状态再继续增量。
struct BaselineResponsePayload {
    QString origin;               // 应答方（导出基线的一方）节点 id
    QString requestOrigin;        // 原始请求方节点 id（把应答路由回请求者）
    qint64 streamEpoch = 0;       // 基线对应的流纪元
    QStringList tables;           // 基线涵盖的表清单
    qint64 fromSeq = 0;           // 基线对应的起点序号
    QString pendingArtifactName;  // 与请求配对的工件名
    QByteArray baselineData;      // 基线全量数据（序列化后的二进制）
    qint64 sourceMaxSeq = 0;      // 导出端此次基线截止到的最大序号
    // C-03 fix：基线导出时刻的“逐 origin applied-vector 快照”（含 epoch）。
    // 取代旧版缺少 stream_epoch 的 QHash<QString,qint64>。
    QVector<BaselineOriginCut> originCuts;
};

// DecodeResult —— 解码一个收到的 artifact 后的“全字段结果”。
// 解码器先填 header、定下 kind，再按 kind 填对应的负载体（其余负载体字段保持空）。
struct DecodeResult {
    PayloadHeader header;                       // 工件头（始终有效）
    PayloadKind kind = PayloadKind::Changeset;  // 负载种类，决定下方哪个字段有效
    QByteArray changeset;                       // kind==Changeset 时的变更集二进制
    // C-5 fix：保留完整的原始编码负载，使隔离区（quarantine）能用 codec->decode() 原样重放。
    QByteArray rawPayload;
    SelectionPushBody selection;               // kind==SelectionPush 时有效
    BaselineRequestPayload baselineRequest;    // kind==BaselineRequest 时有效
    BaselineResponsePayload baselineResponse;  // kind==BaselineResponse 时有效
};

// ── 带类型的 ACK（回执）工件 ────────────────────────────────────────────────
// 接收端成功应用变更后回发 ACK；发送端据此推进“对端已确认水位”，并据以判定一次
// 前台 sync() 是否真正完成（见 PendingAckEntry）。

// ChangesetAck —— 对普通增量变更集的确认。
struct ChangesetAck {
    QString origin;          // 被确认变更的来源节点 id
    qint64 streamEpoch = 0;  // 被确认变更所在流纪元
    qint64 appliedSeq = 0;   // 接收端已成功应用到该 origin 的此序号为止
    QString toPeer;  // 本 ACK 投递给的目标对端（J-01 fix：定向回执，避免误推他人水位）
};

// PushChunkAck —— 对选择性推送“单个分片”的确认。
struct PushChunkAck {
    QString pushId;       // 被确认的推送批次 id
    int chunkSeq = 0;     // 被确认的分片序号
    int totalChunks = 0;  // 该批次总分片数（便于发送端判断是否收齐）
    QString checksum;     // 该分片校验和（接收端回带，供发送端核对一致）
    bool ok = false;      // 该分片是否应用成功（false 表示需重发/排障）
    QString toPeer;       // H-04 fix：本 ACK 路由到的对端（= 该推送的原始 origin）
};

// PendingAckEntry —— 一次前台 sync() 期间，“尚待收齐”的某个对端 + 序号目标。
// 引擎会登记“必须收到哪些 (peer, origin, epoch, targetSeq) 的 ACK”，全部收齐才算
// Completed；超时则 E_SYNC_ACK_TIMEOUT → Failed。
struct PendingAckEntry {
    QString peer;          // 期待其回 ACK 的对端
    QString origin;        // 所等待变更的来源
    qint64 epoch = 0;      // 流纪元
    qint64 targetSeq = 0;  // 期望被确认到的目标序号
};

// 写入语义模式（用于 RowMutation）：
//   DoUpdate   行已存在则更新（标准 UPSERT：插入或更新）。
//   DoNothing  行已存在则跳过（INSERT OR IGNORE 语义，不覆盖既有值）。
enum class UpsertMode { DoUpdate, DoNothing };

// RowMutation —— 调用方通过 ISyncEngine::write() 主动写库时，“描述一行改动”的指令。
// 关键点：这些改动会经由 session 录制器执行，从而被 SQLite changeset 捕获并在下次
// sync() 时打包广播给对端（区别于绕过同步的直写 SQL，后者会被门控或漏同步）。
struct RowMutation {
    QString table;        // 目标表名
    QStringList columns;  // 待写列名（与 values 一一对应、同序）
    QVariantList values;  // 各列的值（与 columns 同序、同长）
    QStringList pkColumns;  // 主键列名（用于定位“是哪一行”，决定插入还是更新）
    UpsertMode mode = UpsertMode::DoUpdate;  // 冲突时更新还是忽略
    // 来源元数据（可选）：携带 origin/seq 等出处信息，供同步/仲裁层使用。
    QVariantMap originMeta;
};

}  // namespace dbridge::sync
