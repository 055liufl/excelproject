# SQLite 同步工具实现评审报告（第十五轮）

评审日期：2026-06-26

## 总体评分

总分：72 / 100

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 功能合规性 | 77 | Excel 主路径和多数 OpenSpec 扩展已补齐；同步 foreground ACK、baseline、selection push 仍有协议偏差 |
| 同步协议正确性 | 58 | ACK 窗口、重发锚点、epoch 保真、forward ACK 路由仍会破坏多 peer/多 origin 收敛 |
| Excel 导入/导出合规性 | 84 | `columnOrder`、反向 lookup、时间格式基本可用；批量导出和 route-local 失败粒度仍有遗漏 |
| 安全性 | 86 | SQL 绑定和内部表过滤较上一轮明显改善；配置完整性和同步表 eligibility 仍偏 MVP |
| 边界处理 | 68 | quarantine、FK closure、fingerprint 开关已修；大批量广播、baseline epoch、多片 push 完成边界仍不稳 |

本轮核查了 9 个主规格文档、`openspec/changes/archive/` 下 27 个归档文件，以及 `src/` 下全部 122 个 `.cpp/.h` 文件；同步公共头文件中与实现直接绑定的 `include/dbridge/sync/*.h` 也纳入核对。

## 上一轮修复验证

### 修复验证结果

| 编号 | 状态 | 验证结论 |
| --- | --- | --- |
| C-01 | 有偏差 | `pendingAckWindow_` 已存在，`processAckArtifact()` 会遍历窗口；但窗口在广播后用 `localOriginSeq_` 反推，只覆盖本地 origin，不能表达本轮实际发出的每个 `(peer, origin, epoch, seq)` |
| C-02 | 有偏差 | 广播不再直接用 `last_sent_seq` 作为下界，但 `minUnackedLocalSeq()` 仍用单个 `local_seq` 下界和 ACK 表 JOIN，可能跳过没有 ACK 行的未确认 origin |
| C-03 | 有偏差 | `originMaxSeq` 和 `resetTo()` 已实现；但 baseline 只携带 `origin -> max_seq`，没有 `stream_epoch`，且来源是 changelog 最大值，不是完整 applied vector |
| C-04 | 有偏差 | selection push 入站 `origin` 保留远端 origin；但 changelog 没有 push_id 元数据，广播过滤按 origin 粗粒度执行，且中心侧 push_progress 不会在全片应用后置为 done |
| H-01 | 通过 | `drainReady()` 按 `id ASC` 返回，删除已解耦到 `markReplayed()` |
| H-02 | 通过 | `FkClosureBuilder::build()` 接收并使用 `includeFkDeps/pruneConsistent`，调用方已传入 `SyncSelection` flags |
| H-03 | 基本通过 | `DataBridge::exportExcel()` 已调用 `validateForExport()`；但 `BatchTransfer::runExport()` 仍绕过该校验，见 H-04 |
| H-04 | 未生效 | `failedRouteIndices` 已收集；写入阶段仍把任意 row-level error 归入 `failedExcelRows`，整行跳过 |
| M-01 | 有偏差 | `SyncConfig::Builder` 增加了部分正数/自 peer 校验，但仍缺重复 peer、center/role、软硬阈值关系等完整性校验 |
| M-02 | 通过 | `SchemaGuard` 构造时使用 `verifySchemaFingerprint()`，关闭时跳过 fingerprint 比较 |
| M-03 | 通过 | 新错误码已在 `Errors.h` 中落地，主要触发点有接入 |
| M-04 | 通过 | `ChangesetApplier::filterCb()` 先拒绝 `__sync_*`，再处理 allow-list |
| M-05 | 通过 | Mixed export-only profile 不再强制 discriminator |
| M-06 | 基本通过 | baseline apply 后会触发 `drainQuarantine()`；运行期 schema 热升级没有独立 API，本轮不再作为阻断项 |

## Critical 问题（必须修复）

### C-01：Foreground ACK 窗口不是“本轮实际发出载荷”的窗口，会误完成、误超时或卡住前台 gate

- 位置：`src/sync/SyncWorker.cpp:1497`、`src/sync/SyncWorker.cpp:1504`、`src/sync/SyncWorker.cpp:1517`、`src/sync/SyncWorker.cpp:1530`
- 描述：`enqueueDrain()` 先 `scanInbox()`/`broadcast()`，之后才构造 `pendingAckWindow_`；窗口只记录 `origin=config_.nodeId()` 且目标为全局 `localOriginSeq_`。如果本轮只广播转发的远端 origin，`wrote=true` 但窗口为空，代码直接清 `ackWaiting_` 且不发 `Completed`，`SyncEngine` 会停在 `Exporting` 并持有 `ForegroundGate`。如果本地 changelog 超过 `broadcastThreshold`，窗口目标又可能大于本轮实际写出的最大 seq，导致必然 ACK timeout。
- 规格依据：设计文档 §7.1 要求 `Exporting(percent=-1)` 直到足额 ACK；plan T1.12 要求前台 `Exporting=等ACK`，足额 ACK 才 `Completed`。
- 修复方案：广播函数返回本轮实际写出的 `QList<PendingAckEntry>`，每个 entry 必须来自实际 artifact header 的 `(targetPeer, origin, stream_epoch, origin_seq)`；窗口应在写出 artifact 的同一逻辑中累积，而不是事后用 `localOriginSeq_`/`last_sent_seq` 推断。窗口为空但 `wrote=true` 应视为内部错误，不应静默清等待位。

### C-02：重发下界仍可能跳过没有 ACK 行的未确认 origin

- 位置：`src/sync/anchor/OutboundAckStore.cpp:112`、`src/sync/anchor/OutboundAckStore.cpp:119`、`src/sync/SyncWorker.cpp:1048`、`src/sync/capture/ChangelogStore.cpp:76`
- 描述：`minUnackedLocalSeq()` 只 JOIN 已存在 `__sync_outbound_ack` 行的 origin。若某 origin 的 artifact 已写出但 ACK 丢失，因此还没有 ACK 行，而另一个已建 ACK 行的 origin 存在更大的未 ACK `local_seq`，单一 `afterLocalSeq` 会越过前者，`readRangeAll(local_seq > afterLocalSeq)` 永久不再读到该未确认 origin。
- 规格依据：设计 §6.1/§7.4 与 plan T1.8 要求发送端锚点按 ACK 前移，未 ACK 数据必须可重发；`last_sent_seq` 不能作为排除条件。
- 修复方案：不要用单个 local_seq 作为全局排除下界。可选方案：发送前为 `(peer, origin, epoch)` 建 ACK 行并以 `acked_seq=-1` 参与查询；或按 origin 分组读取 `origin_seq > acked_seq` 后合并排序；或维护 pending outbox 表直到 ACK。

### C-03：baseline applied vector 缺少 `stream_epoch`，且导出来源不是权威 applied vector

- 位置：`include/dbridge/sync/SyncTypes.h:121`、`src/sync/baseline/BaselineManager.cpp:90`、`src/sync/baseline/BaselineManager.cpp:95`、`src/sync/baseline/BaselineManager.cpp:281`
- 描述：`BaselineResponsePayload::originMaxSeq` 是 `QHash<QString,qint64>`，只按 origin 分组。`queryOriginMaxSeq()` 从 `__sync_changelog GROUP BY origin` 取最大 `origin_seq`，忽略 `stream_epoch`，也忽略已经通过 baseline/compaction 推进但不在 changelog 中的 applied vector。`applyBaseline()` 又把所有 origin 重置到单个 `resp.streamEpoch` 下。
- 规格依据：设计 §6.1 中 `__sync_applied_vector` 主键是 `(origin, stream_epoch)`，§5.10 要求 baseline 是权威切面，应用后重置 applied vector/table_state/row_winner。
- 修复方案：baseline payload 应携带数组 `{origin, stream_epoch, applied_seq}`，来源以 `__sync_applied_vector` 为准，并补充切面时刻 changelog 中尚未反映到 vector 的本地 cut。apply 时逐 tuple 调用 `resetTo(origin, epoch, seq, generation)`。

### C-04：转发 changeset 时改写了原始 `stream_epoch`

- 位置：`src/sync/capture/ChangelogStore.h:38`、`src/sync/capture/ChangelogStore.cpp:76`、`src/sync/SyncWorker.cpp:1101`、`src/sync/SyncWorker.cpp:1104`
- 描述：`__sync_changelog` 保存了 `stream_epoch`，但 `EntryFull`/`readRangeAll()` 不返回该字段；`broadcastTopeer()` 重新编码 payload 时统一写 `hdr.streamEpoch = streamEpoch_`。中心转发 B 节点 changeset 给 C 时，载荷变成 `(origin=B, stream_epoch=centerEpoch, origin_seq=BSeq)`，破坏 applied_vector 的命名空间。
- 规格依据：设计 §6.1 明确 changelog 唯一键和 applied_vector 均按 `(origin, stream_epoch, origin_seq)`；§6 epoch 规则要求应用前比对载荷 epoch。
- 修复方案：`ChangelogStore::EntryFull` 返回 `streamEpoch/schemaVer/schemaFingerprint`；广播 header 必须使用 entry 中保存的原始 epoch 和 schema 元数据。ACK 查询和更新也必须使用该 entry epoch。

### C-05：转发 changeset 的 ACK 被发给 origin，而不是发给本次 artifact 的发送者

- 位置：`src/sync/SyncWorker.cpp:536`、`src/sync/SyncWorker.cpp:544`、`src/sync/transport/AckChannel.cpp:33`
- 描述：接收方应用 changeset 后设置 `ack.toPeer = dec.header.origin`。直接 A→B 时 origin 恰好等于发送者；但中心转发 B-origin artifact 给 C 时，C 会把 ACK 发给 B，而中心的 `__sync_outbound_ack(peer=C, origin=B, epoch=...)` 永远不会前移，导致中心无限重发/无法完成以远端 origin 为目标的 ACK 窗口。
- 规格依据：设计 §5.11/§7.4 要求 ACK 前移发送端 per-peer 锚点；ACK 是运输层对本次发送者的确认，不是业务 origin 的私信。
- 修复方案：payload header 或 artifact naming 必须携带 `fromPeer/senderPeer`，或由 InboxWatcher/transport 层把物理来源传入 `processArtifact()`。ACK 的 `toPeer` 应为发送者，ACK body 仍携带业务 `(origin, stream_epoch, applied_seq)`。

## High 问题

### H-01：selection push 的“完成后解锁广播”没有在中心侧落地

- 位置：`src/sync/SyncWorker.cpp:633`、`src/sync/apply/CapturedWriteTemplate.cpp:344`、`src/sync/SyncWorker.cpp:993`、`src/sync/SyncWorker.cpp:1068`
- 描述：中心收到 selection push chunk 时把 `__sync_push_progress.status` 写成 `streaming`，chunk 应用后只更新 `__sync_push_chunk_progress`。`status='done'` 的代码只在处理 `PushChunkAck` 时执行，也就是原始 push 发起方收到 ACK 时执行；中心侧没有在全部 chunk applied 后置 done。于是中心广播层的 `status != 'done'` 过滤会长期阻止该 origin 的 changelog 下游广播。
- 规格依据：设计 §5.5/§7.3 要求中心在全片完成前不向 C/D 广播本 push，完成后应解锁；plan T2.13 要求半截不外泄且全片 ACK 后 Completed。
- 修复方案：中心侧每次 chunk applied 后按 `push_id` 统计 `total_chunks` 与 applied chunk 数；达到总数时在同一事务或紧随事务把 `push_progress.status='done'`。广播过滤必须按 changelog 的 push_id 精确判断，不能按 origin 粗过滤。

### H-02：`failedRouteIndices` 收集后仍被 `failedExcelRows` 整行跳过

- 位置：`src/service/ImportService.cpp:555`、`src/service/ImportService.cpp:623`、`src/service/ImportService.cpp:663`
- 描述：`FkInjector::inject()` 已返回失败 route 集合，`ctx.failedRouteIndices` 也已保存；但写入前仍把所有 row-level errors 加入 `failedExcelRows`，随后 `if (failedExcelRows.contains(ctx.excelRow)) continue;`。任一 lookup/fkInject/preflight route 出错都会跳过整个 Excel 行，兄弟 route 无法继续。
- 规格依据：`openspec/specs/fk-injection/spec.md` 要求失败 route 只级联抑制 descendants，兄弟 route 不受影响；`row-lookup` 要求 lookup 输出 route-local。
- 修复方案：错误收集需要带 route index 或 route table；写入阶段不要用 row 维度总开关。对每个 `RowContext` 构造 `directFailedRoutes + descendants` 后只跳过对应 payload。非 route 绑定的行错误才可整行跳过。

### H-03：批量导出绕过 export-mode profile validation

- 位置：`src/DataBridge.cpp:349`、`src/batch/BatchTransfer.cpp:104`、`src/batch/BatchTransfer.cpp:272`
- 描述：`DataBridge::exportExcel()` 已调用 `ProfileValidator::validateForExport()`，但 `BatchTransfer::startExport()` snapshot profile/catalog 后，worker 线程直接 `ExportService::run()`。这条公开导出路径仍可绕过 `columnOrder` unknown/duplicate、raw SQL 互斥、反向 lookup post-substitution header 校验。
- 规格依据：`export-column-order` 要求这些条件在 profile validation 阶段失败；`export-reverse-lookup` 要求 columnOrder 按替换后 header 集校验。
- 修复方案：把 export validation 下沉到共享 helper，`DataBridge::exportExcel()`、`DataBridge::runExportOnDb()`、`BatchTransfer::runExport()` 在调用 `ExportService` 前统一执行。

### H-04：反向 lookup 的 row-level 错误跳过整行，而不是仅影响声明 route

- 位置：`src/service/ExportService.cpp:440`、`src/service/ExportService.cpp:487`、`src/service/ExportService.cpp:936`、`src/service/ExportService.cpp:941`
- 描述：`resolveAHeaders()` 只返回一个 `rowSkip` 布尔值；任一 lookup miss/ambiguous 就让整条导出行不写。规格要求 `"error"` 模式跳过声明 route 在该 Excel 行的输出，其他 route 贡献不受影响。
- 规格依据：`export-reverse-lookup` §`exportOnMissing` 与 Row resilience 明确单个 `E_REVERSE_LOOKUP_*` 不得中止整 sheet，且其他 route 不受影响。
- 修复方案：反向 lookup resolver 返回失败 route 集合和 A-header 空值集合；投影时只清空/跳过该 route 的输出列，保留其他 route 的列。对于当前无法表达 route 局部投影的扁平行模型，至少需在规格中声明 MVP 限制并提供测试覆盖。

## Medium 问题

### M-01：`SyncConfig::Builder` 校验仍不完整

- 位置：`include/dbridge/sync/SyncConfig.h:281`
- 描述：当前只校验少量必填项和部分正数。仍未拒绝重复 peer、空 peer id、center 与 role 不一致、Edge 未配置 center、soft 阈值大于 hard 阈值、`outboxMaxBytesPerPeer/baselineSizeWarnBytes/changelogRetention/gapTimeoutMs` 非法值等。
- 规格依据：设计 §4.4 要求 `SyncConfig::Builder` 包含全部字段并做完整性校验。
- 修复方案：补全拓扑、阈值、目录、rank 映射的字段级校验；返回明确错误文本和错误码，测试覆盖非法组合。

### M-02：同步 schema eligibility 仍窄于规格

- 位置：`src/sync/schema/SchemaEligibility.cpp:78`
- 描述：实现仍显式拒绝 composite PRIMARY KEY，只是换成了更明确的 `E_SYNC_COMPOSITE_PK_NOT_SUPPORTED`。规格允许 ordinary rowid 或 WITHOUT ROWID，只要求有明确 sync key / UPSERT target。
- 规格依据：设计 §5.1/G-04 要求校验同步表 eligibility，而非强制单列 PK。
- 修复方案：若这是 MVP 限制，应在公开文档和能力表中声明；长期应把 SelectionResolver、FkClosureBuilder、RowWinnerStore、UpsertExecutor 的 PK 表示升级为列元组。

### M-03：ACK flush 不保证独立满足 `ackMaxDelayMs`

- 位置：`src/sync/transport/AckChannel.cpp:15`、`src/sync/transport/AckChannel.cpp:65`、`src/sync/SyncWorker.cpp:393`
- 描述：`scheduleChangesetAck()` 只在距离上次 flush 超过 `ackMaxDelayMs` 时写出；否则依赖主循环下一次 broadcast flush。若 `broadcastIntervalMs > ackMaxDelayMs` 且无其他事件，ACK 可能晚于配置上限。
- 规格依据：设计 §5.11/FR-4/F-14 要求 ACK 在 `ackMaxDelayMs` 内必发。
- 修复方案：主循环等待时间还应考虑 pending ACK 的 deadline，或 `AckChannel` 暴露 `nextFlushDeadlineMs()`，让 worker 在 ACK 截止前唤醒并 flush。

### M-04：selection push/changelog 缺少 push_id 元数据，无法精确实现半截屏障

- 位置：`src/sync/SyncDDL.h:13`、`src/sync/capture/ChangelogStore.cpp:139`、`src/sync/SyncWorker.cpp:1072`
- 描述：`__sync_changelog` 没有 `push_id/chunk_seq/push_status` 或等价来源字段，广播层只能通过 `origin` 查 `push_progress`。这既会阻塞无关变更，也无法证明失败/未完成 push 的已落片不会泄漏。
- 规格依据：设计 §5.5 要求中心在本 push 完成前不得向下游广播“本 push 的直选变更”。
- 修复方案：Branch B seal changelog 时写入 `push_id/chunk_seq`；广播层按 entry.push_id 精确判断 done。对 local write 和普通 forwarded changeset，该字段为空，不参与 push 屏障。

## 修复优先级汇总表

| 编号 | 文件 | 严重度 | 一句话描述 |
| --- | --- | --- | --- |
| C-01 | `src/sync/SyncWorker.cpp:1504` | Critical | Foreground ACK 窗口只按本地 origin 反推，不能等待本轮实际发送载荷 |
| C-02 | `src/sync/anchor/OutboundAckStore.cpp:112` | Critical | 单一 local_seq 重发下界会跳过没有 ACK 行的未确认 origin |
| C-03 | `src/sync/baseline/BaselineManager.cpp:90` | Critical | baseline vector 缺 stream_epoch，且不以 applied_vector 为权威来源 |
| C-04 | `src/sync/SyncWorker.cpp:1104` | Critical | 广播转发 changeset 时改写原始 stream_epoch |
| C-05 | `src/sync/SyncWorker.cpp:544` | Critical | 转发 changeset 的 ACK 发给业务 origin，发送端锚点不会前移 |
| H-01 | `src/sync/SyncWorker.cpp:1068` | High | selection push 在中心侧不会完成解锁，且广播过滤不是 per-push |
| H-02 | `src/service/ImportService.cpp:623` | High | `failedRouteIndices` 被整行 `failedExcelRows` 覆盖，兄弟 route 仍被跳过 |
| H-03 | `src/batch/BatchTransfer.cpp:272` | High | 批量导出绕过 `validateForExport()` |
| H-04 | `src/service/ExportService.cpp:941` | High | 反向 lookup row-level 错误跳过整行而非声明 route |
| M-01 | `include/dbridge/sync/SyncConfig.h:281` | Medium | `SyncConfig::Builder` 仍缺完整拓扑和阈值校验 |
| M-02 | `src/sync/schema/SchemaEligibility.cpp:78` | Medium | 同步 eligibility 仍拒绝规格允许的复合 PK/WITHOUT ROWID 模型 |
| M-03 | `src/sync/transport/AckChannel.cpp:65` | Medium | ACK flush 依赖 broadcast tick，可能超过 `ackMaxDelayMs` |
| M-04 | `src/sync/SyncDDL.h:13` | Medium | changelog 缺 push_id，无法精确实现 selection push 半截屏障 |
