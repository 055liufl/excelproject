# SQLite 同步工具实现评审报告（第十六轮）

评审日期：2026-06-26

## 总体评分

总分：76 / 100

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 功能合规性 | 82 | 第十五轮多数显式修复已落地；baseline、route-local 失败粒度、反向 lookup error 语义仍有偏差 |
| 同步协议正确性 | 63 | ACK 窗口修复明显改善，但转发流仍混用本地 epoch，baseline cut 不完整，quarantine 持久化失效 |
| Excel 导入/导出合规性 | 84 | columnOrder、批量导出校验、时间格式基本符合；FK preflight 与 reverse lookup 的错误粒度仍不完全符合规格 |
| 安全性 | 87 | SQL 标识符引用、绑定与内部表过滤总体可接受；未发现新的注入类 Critical |
| 边界处理 | 71 | ACK deadline、push done、缺口 pending 有改善；schema mismatch、baseline、长 push 屏障仍有高风险边界缺陷 |

本轮对指定规格文档、`openspec/specs/`、`openspec/changes/archive/` 与 `src/` 下同步、导入导出、profile、batch 关键实现进行了静态审查；未执行编译或自动化测试。

## 上一轮修复验证

| 编号 | 状态 | 验证结论 |
| --- | --- | --- |
| C-01 | 通过 | `enqueueDrain()` 直接调用 `broadcastTopeer(..., &broadcastedEntries)`，并按实际写出的 `(peer, origin, epoch, originSeq)` 构造 `pendingAckWindow_`。 |
| C-02 | 有偏差 | `minUnackedLocalSeq()` 已改为 LEFT JOIN + COALESCE，覆盖无 ACK 行 origin；但广播仍以本地 `streamEpoch_` 查询/更新部分转发 ACK 锚点，见 Critical C-02。 |
| C-03 | 有偏差 | baseline payload 已有 `BaselineOriginCut{origin, streamEpoch, appliedSeq}` 且来自 `__sync_applied_vector`；但未补充 baseline 源节点自己的本地 origin cut，见 Critical C-01。 |
| C-04 | 有偏差 | `EntryFull` 返回 `streamEpoch`，payload header 使用 entry epoch；但路由 ACK 查询仍用本地 epoch，且 entry 未返回 `push_id`，见 Critical C-02 / High H-01。 |
| C-05 | 通过 | `PayloadHeader.senderPeer` 已写入，接收方 changeset ACK 使用物理发送者。 |
| H-01 | 通过 | `CapturedWriteTemplate` 在 chunk applied 后统计 total/applied，达到总数时将 `__sync_push_progress.status` 置为 `done`。 |
| H-02 | 有偏差 | 写入阶段对 `lookup/fkInject` 已按 route 过滤；但 FK preflight 失败仍只记录 row error，未合并进 route 失败集合，见 High H-02。 |
| H-03 | 通过 | `BatchTransfer::runExport()` 调用 `ProfileValidator::validateForExport()`，批量导出不再绕过 export-mode 校验。 |
| H-04 | 有偏差 | `resolveAHeaders()` 已返回失败 A-header 集合并清空对应列；但当前 OpenSpec 的 `exportOnMissing:"error"` 要求受影响行不写出，见 High H-03。 |
| M-01 | 有偏差 | `SyncConfig::Builder` 增加了重复 peer、空 peer、Edge center、部分正数校验；仍缺软硬阈值关系和部分字段正数校验，见 Medium M-01。 |
| M-02 | 通过 | `SchemaEligibility` 的 composite PK 拒绝消息已明确为 MVP 限制。规格能力偏差另列 Medium M-02。 |
| M-03 | 通过 | `AckChannel::nextDeadlineMs()` 已纳入 worker wait interval，ACK flush 不再依赖 broadcast tick。 |
| M-04 | 有偏差 | `__sync_changelog.push_id` 列已增加，但广播屏障仍按 `origin` 粗过滤，未按 entry 的 `push_id` 精确过滤，见 High H-01。 |

## Critical 问题（必须修复）

### C-01：baseline 未携带源节点本地 origin cut，应用后会把源 origin 的 applied vector 重置为 0

- 位置：`src/sync/baseline/BaselineManager.cpp:149`、`src/sync/baseline/BaselineManager.cpp:155`、`src/sync/baseline/BaselineManager.cpp:359`
- 描述：`queryOriginCuts()` 只读取 `__sync_applied_vector`。本地写入不会推进本节点自己的 applied vector，因此 baseline 源节点的本地 `(origin=source, stream_epoch=sourceEpoch, applied_seq=localOriginSeq)` 通常不在 cuts 中。`applyBaseline()` 在 `primaryReset=false` 时把该 origin 重置到 `0`。baseline 数据已经包含源节点当前表切面，但接收端仍认为源节点 seq 只应用到 0；后续若旧 changelog 已压缩或只收到 seq N+1，会永久 gap / 反复 baseline；若旧 artifact 重放，也可能重复应用已包含在 baseline 中的变更。
- 规格依据：设计文档 §5.10 要求 baseline 是权威切面，应用后重置 applied-vector/table_state/row_winner；plan T5.1 要求 baseline 后锚点与权威切面对齐。
- 修复方案：baseline export 时在 `originCuts` 中补充源节点本地 cut。可从 `__sync_changelog WHERE origin=<local node> AND stream_epoch=<local epoch>` 取 `MAX(origin_seq)`，或由 worker 显式传入 `localOriginSeq_` 和 `streamEpoch_`。若 applied_vector 中已有同 tuple，取两者最大值。禁止用 `sourceMaxSeq` 的 `local_seq` 代替 `origin_seq`。

### C-02：转发 changeset 的 ACK 查询仍使用本地 epoch，远端 origin 会被无限重播

- 位置：`src/sync/SyncWorker.cpp:1079`、`src/sync/SyncWorker.cpp:1095`、`src/sync/SyncWorker.cpp:1176`、`src/sync/anchor/OutboundAckStore.cpp:119`
- 描述：`broadcastTopeer()` 读取 entry 后，`ackedSeq(peer, entry.origin, streamEpoch_)` 用的是当前节点本地 epoch，而不是 `entry.streamEpoch`。中心转发 B-origin artifact 给 C 后，C 的 ACK 会按 `(peer=C, origin=B, epoch=B_epoch)` 更新；下一轮中心仍查 `(peer=C, origin=B, epoch=center_epoch)` 得到 -1，于是 `RoutingTable::shouldRoute()` 继续允许发送。`minUnackedLocalSeq()` 也按单个本地 epoch 过滤 changelog，无法正确作为多 epoch 转发流的重发下界。
- 规格依据：设计文档 §6.1 中 `__sync_outbound_ack` 主键是 `(peer, origin, stream_epoch)`；§7.4 要求发送端锚点按收到的 typed ACK 前移，未 ACK 才重发。
- 修复方案：所有 per-entry ACK 判断必须使用 `entry.streamEpoch`。`readRangeAll()` 应返回 `push_id/schemaVer/schemaFingerprint` 等元数据；重发下界不能只按本地 epoch 的 sentinel 行计算，建议按 changelog entry LEFT JOIN `__sync_outbound_ack(peer, origin, stream_epoch)` 后筛选 `origin_seq > acked_seq`，再按 `local_seq ASC LIMIT` 发送。

### C-03：schema mismatch quarantine 存入空 payload，持久重放能力失效

- 位置：`src/sync/payload/PayloadCodec.cpp:161`、`include/dbridge/sync/SyncTypes.h:145`、`src/sync/SyncWorker.cpp:590`
- 描述：`DecodeResult::rawPayload` 注释要求保存完整 encoded artifact，`processChangesetArtifact()` 在 schema mismatch 时用 `dec.rawPayload` 写入 `__sync_quarantine`。但 `PayloadCodec::decode()` 从未执行 `out->rawPayload = data`。结果 quarantine 行保存空 BLOB；`drainReady()` 后 `codec_->decode(empty)` 必然失败。若原 inbox 文件被传输层清理或重启后不可用，唯一持久副本不可重放，schema-mismatch payload 会丢失。
- 规格依据：设计文档 §5.2/§5.4/§5.10 要求 schema 不匹配载荷隔离到 quarantine，并在 schema/baseline 适配后重放；QuarantineStore 注释也要求 `drainReady()` 返回可 replay payload。
- 修复方案：在 `PayloadCodec::decode()` 成功读取 magic/version 后立即保存完整输入：`out->rawPayload = data`，并在进入 decode 前清空 `*out` 避免复用污染。补充 schema mismatch → quarantine → drainReady → replay 的回归测试。

## High 问题

### H-01：selection push 半截广播屏障仍按 origin 粗过滤，未按 push_id 精确过滤

- 位置：`src/sync/SyncWorker.cpp:1099`、`src/sync/SyncWorker.cpp:1106`、`src/sync/capture/ChangelogStore.h:41`、`src/sync/capture/ChangelogStore.cpp:154`
- 描述：`__sync_changelog` 已有 `push_id`，`appendForward/insertRow` 也可写入；但 `EntryFull` 不返回 `push_id`，`broadcastTopeer()` 仍查询 `__sync_push_progress WHERE origin=? AND status!='done'`。这会在某个 B-origin push streaming 时阻塞 B 的所有其它普通 forwarded changeset，也无法证明只屏蔽“本 push 的直选变更”。
- 规格依据：设计文档 §5.5 要求中心在“本 push”完成前不得向下游广播本 push 的直选变更，而不是冻结该 origin 的所有流量。
- 修复方案：`EntryFull` 增加 `pushId`，`readRangeAll()` SELECT `push_id`。广播时仅当 `entry.pushId` 非空且对应 `push_progress.status!='done'` 时跳过；普通 changeset 和其它 push 不受影响。

### H-02：FK preflight 失败仍导致整行跳过，兄弟 route 不能继续写入

- 位置：`src/validation/ForeignKeyPreflight.cpp:14`、`src/validation/ForeignKeyPreflight.cpp:150`、`src/service/ImportService.cpp:623`、`src/service/ImportService.cpp:671`
- 描述：`FkInjector::inject()` 返回 route 失败集合，写入阶段据此过滤 route 及 descendants；但 `ForeignKeyPreflight::check()` 只向 `ErrorCollector` 添加 row-level `E_VALIDATE_FK`，没有把失败 route index 回填到 `RowContext::failedRouteIndices`。因此当某个 route 预检缺父行时，`failedExcelRows` 包含该 row 且 `ctx.failedRouteIndices` 可能为空，整行被跳过；不相关 sibling route 仍无法写入。
- 规格依据：`openspec/specs/fk-injection/spec.md` 与归档 design 要求失败 route 只级联抑制 descendants，不相关 sibling route 不受影响。
- 修复方案：让 `ForeignKeyPreflight::check()` 返回 `QHash<excelRow,QSet<routeIndex>>` 或直接接收可变 `contexts` 并合并失败 route。写入阶段统一使用 `directFailedRoutes + descendants`，只有非 route 绑定的结构/映射错误才整行跳过。

### H-03：`exportOnMissing:"error"` 的 reverse lookup 仍写出该行，只清空 A 列

- 位置：`src/service/ExportService.cpp:479`、`src/service/ExportService.cpp:505`、`src/service/ExportService.cpp:978`、`src/service/ExportService.cpp:998`
- 描述：当前 `resolveAHeaders()` 在 NOT_FOUND 或 NULL H 且 `exportOnMissing=="error"` 时记录 `failedAHeaders`，随后投影阶段只把这些 A 列写空，仍写出整行。OpenSpec 当前主规格要求 `"error"` 产生 `E_REVERSE_LOOKUP_NOT_FOUND` 后受影响行不写出；`"null"`/`"skip"` 才是空 A 单元格继续写。当前行为会把本应失败跳过的数据静默导出为缺业务键行。
- 规格依据：`openspec/specs/export-reverse-lookup/spec.md` 的 `exportOnMissing` 与 “Export remains row-resilient” 要求：`error` 模式 bad row skipped，`null/skip` 模式 empty A cells。
- 修复方案：区分三种模式：`error` 设置 row/route skip；`null` 写空且不报 row-level error；`skip` 写空且不计错误。若项目决定采用“只清空列”的弱语义，需要先同步更新 OpenSpec，再以新规格评审。

## Medium 问题

### M-01：`SyncConfig::Builder` 校验仍不完整

- 位置：`include/dbridge/sync/SyncConfig.h:327`、`include/dbridge/sync/SyncConfig.h:340`、`include/dbridge/sync/SyncConfig.h:377`
- 描述：Builder 已校验重复 peer、空 peer、Edge center 和部分正数；但仍未校验 `peerLagSoftSeq <= peerLagHardSeq`、`peerLagSoftBytes <= peerLagHardBytes`、`peerLagSoftMs <= peerLagHardMs`，也未校验 hard 阈值、`outboxMaxArtifactsPerPeer`、`baselineSizeWarnBytes`、`changelogRetention` 等正数。非法配置会让 dead-peer eviction、GC 或载荷预算行为不可预测。
- 规格依据：设计文档 §1/§4.4 要求 Builder 完整性校验。
- 修复方案：补全所有数值字段正数校验与软硬阈值关系校验；Center/Edge 拓扑规则也应明确：Center 不应要求 `centerNodeId`，Edge 的 center 应在 peer 列表或传输路由中可达。

### M-02：同步 eligibility 仍拒绝规格允许的复合主键 / WITHOUT ROWID 表

- 位置：`src/sync/schema/SchemaEligibility.cpp:78`
- 描述：实现对 composite PRIMARY KEY 直接拒绝，并建议改为单列 surrogate PK。错误消息已清楚，但能力仍窄于同步设计：规格要求的是明确 sync key / UPSERT target，而不是强制单列 PK。
- 规格依据：设计文档 §5.1/G-04 要求 ordinary rowid 或 WITHOUT ROWID 表可被 eligibility 判断，只要有明确冲突目标。
- 修复方案：若维持 MVP 限制，应在能力矩阵和同步公开文档中显式声明；长期需将 `RowMutation.pkColumns`、`RowWinnerStore`、`SelectionResolver`、`UpsertExecutor` 等路径按列元组建模。

### M-03：payload header 的 `senderPeer` 追加方式不是真正向后兼容

- 位置：`src/sync/payload/PayloadCodec.cpp:64`、`src/sync/payload/PayloadCodec.cpp:71`
- 描述：`readHeader()` 无条件尝试读取 `senderPeer`。旧 payload 在 header 后紧跟 compressed body；这里会把 body 的 `QByteArray` 序列化内容按 `QString` 读取，可能推进流位置并导致后续 body 读取失败。注释称 `ReadPastEnd` 可兼容旧 payload，但 `QDataStream` 对不同类型无字段标签，不能用这种方式可靠探测可选字段。
- 规格依据：设计文档 §5.11/§13.1 要求传输制品 schema 稳定；混版本或积压 payload 不应因头字段追加而损坏。
- 修复方案：提升 codec version，或在 header 中加入 flags/field-count；旧 version 按旧字段数量解析，新 version 再读取 `senderPeer`。

### M-04：`ImportService` 写入阶段无法区分“route-local row error”和“非 route row error”的混合情况

- 位置：`src/service/ImportService.cpp:623`、`src/service/ImportService.cpp:667`
- 描述：当前逻辑只判断 `failedExcelRows.contains(row) && ctx.failedRouteIndices.isEmpty()`。如果同一 Excel 行同时存在一个 route-local 错误和一个非 route 绑定的映射/验证错误，`ctx.failedRouteIndices` 非空会让整行继续进入 route 过滤，非 route 错误可能被忽略。
- 规格依据：导入设计要求结构/类型等基础错误在落盘前阻断对应无效数据；row-lookup/fk-injection 才是 route-local 抑制。
- 修复方案：ErrorCollector 或 RowContext 应携带 `routeIndex`/`scope`。写入阶段只在所有 row errors 都能归属到 failed route 集合时继续；否则整行跳过或按对应 route 精确跳过。

## 修复优先级汇总表

| 编号 | 文件 | 严重度 | 一句话描述 |
| --- | --- | --- | --- |
| C-01 | `src/sync/baseline/BaselineManager.cpp:149` | Critical | baseline cuts 缺源节点本地 origin cut，应用后 applied vector 可能回到 0 |
| C-02 | `src/sync/SyncWorker.cpp:1095` | Critical | 转发流 ACK 查询使用本地 epoch，远端 origin 会无限重播 |
| C-03 | `src/sync/payload/PayloadCodec.cpp:161` | Critical | `rawPayload` 未赋值，schema mismatch quarantine 保存空 payload |
| H-01 | `src/sync/SyncWorker.cpp:1106` | High | selection push 屏障按 origin 粗过滤，未按 entry.push_id 精确过滤 |
| H-02 | `src/validation/ForeignKeyPreflight.cpp:150` | High | FK preflight route 失败未回填 route 集合，仍整行跳过 |
| H-03 | `src/service/ExportService.cpp:505` | High | reverse lookup `error` 模式写出空 A 列行，偏离当前 OpenSpec |
| M-01 | `include/dbridge/sync/SyncConfig.h:327` | Medium | SyncConfig 仍缺软硬阈值和部分数值字段完整性校验 |
| M-02 | `src/sync/schema/SchemaEligibility.cpp:78` | Medium | eligibility 仍拒绝复合 PK / WITHOUT ROWID 能力范围 |
| M-03 | `src/sync/payload/PayloadCodec.cpp:71` | Medium | `senderPeer` 可选字段解析方式无法兼容旧 payload |
| M-04 | `src/service/ImportService.cpp:667` | Medium | ImportService 不能区分 route-local 与非 route row error 的混合行 |

本轮仍存在 Critical/High 阻断性问题，不建议通过。
