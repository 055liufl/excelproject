# SQLite 同步工具实现评审报告（第十四轮）

评审日期：2026-06-26

## 总体评分

总分：68 / 100

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 维度 A：功能合规性 | 71 | Excel/OpenSpec 主路径实现较完整；同步选择推送、baseline、配置校验仍有规格偏差 |
| 维度 B：同步协议正确性 | 55 | ACK 完整性、发送锚点、baseline applied vector、半截 push 下游传播存在 Critical 风险 |
| 维度 C：Excel 导入/导出合规性 | 78 | `columnOrder`、反向查找、时间格式大体实现；导出侧未执行 profile 校验，FK 级联粒度错误 |
| 维度 D：安全性 | 82 | 主路径普遍使用绑定参数和 `quoteIdent`；仍有内部表过滤顺序、配置开关未生效等问题 |
| 维度 E：边界处理 | 63 | gap pending、重复 chunk 幂等已有基础；quarantine 重放、multi-peer ACK、baseline 后续处理不足 |

本轮覆盖 9 个主规格文档、`openspec/changes/archive/` 下 27 个归档变更文件，并枚举核查 `src/` 下 122 个 `.cpp/.h` 文件；同步主路径、导入导出主路径、OpenSpec 扩展路径均按调用链检查。

## Critical 问题（必须修复）

### C-01：前台 `sync()` 收到任意一个 Changeset ACK 即完成，未等待全部 peer/全部窗口

- 位置：`src/sync/SyncEngine.cpp:164`、`src/sync/SyncWorker.cpp:906`
- 描述：`sync()` 只调用 `startAckWait()` 设置一个全局布尔等待位；`processAckArtifact()` 解出任意 `ChangesetAck` 后就 `ackWaiting_.exchange(false)` 并发出 `Completed`。实现没有记录本轮发给哪些 peer、每个 peer 需要 ACK 到哪个 `(origin, stream_epoch, applied_seq)`，也没有区分本轮 ACK 与旧 ACK。
- 规格依据：`specs/SQLite-同步工具-设计文档.md` §5.4/§5.11 要求 typed ACK；§4.3/FR-11/FR-17 要求 `Exporting` 直到足额 ACK 才 `Completed`；多 peer 场景要求 ACK 完整性。
- 影响：前台可能在部分 peer 未收到或未应用变更时报告成功，掩盖数据丢失和节点分叉。
- 修复方案：为每次 foreground operation 建立 `PendingAckWindow`，记录 expected peers 和每个 peer 的目标 `(origin, epoch, seq)`；只有所有 expected peer 的 `acked_seq >= target` 才完成。ACK 处理必须校验 `toPeer`、发送 peer、origin/epoch/seq 属于当前窗口；旧 ACK 只能推进持久锚点，不能完成当前操作。

### C-02：发送水位在 ACK 前推进，未 ACK 的变更可能永久不重发

- 位置：`src/sync/SyncWorker.cpp:1018`、`src/sync/SyncWorker.cpp:1086`、`src/sync/anchor/OutboundAckStore.cpp:72`
- 描述：`broadcastTopeer()` 先按 `lastSentLocalSeq` 读取 changelog，写出 payload 后立即 `updateLastSent()`。如果第三方搬运、对端 apply 或 ACK 丢失，下一轮从新的 `last_sent_seq` 之后读取，旧的未 ACK changelog 不再进入重发范围。
- 规格依据：设计文档 §6.1/§7.4 和 plan T1.8 明确 outbound ACK 是发送端锚点，裁剪/重发/截断应由 ACK 前移驱动；§10 序列图写明“收 ACK 才前移锚点”。
- 影响：只要 artifact 写出后在 ACK 前丢失，发送端就认为已发送过并跳过，造成永久丢更。
- 修复方案：拆分“本轮已尝试发送游标”和“durable ACK 游标”。广播读取范围应从每个 peer/origin 的 `acked_seq` 之后开始，或维护 pending outbox 表直到 ACK；`last_sent_seq` 只能作为限流/诊断字段，不能作为排除未 ACK 数据的读取下界。

### C-03：baseline apply 未把 applied vector 推进到源端权威截点，且混用 `local_seq` 与 `origin_seq`

- 位置：`src/sync/baseline/BaselineManager.cpp:75`、`src/sync/baseline/BaselineManager.cpp:261`、`src/sync/apply/AppliedVectorStore.cpp:69`
- 描述：baseline 导出使用 `MAX(local_seq)` 填 `sourceMaxSeq`；apply 时调用 `av.reset(origin, epoch, art.sourceMaxSeq)`，但 `AppliedVectorStore::reset()` 固定写入 `applied_seq = 0`，只把该值放入 `baseline_generation`。`applied_vector` 的语义却是 `(origin, stream_epoch) -> origin_seq`。
- 规格依据：设计 §5.10 要求 baseline 是权威切面，应用后同事务重置 `applied_vector/table_state/row_winner` 并推进到源端权威 cut；§6.1 要求序列命名空间不能混用。
- 影响：baseline 后旧 changeset 可能被重复应用，或新 changeset 被判定为 gap；在多 origin changelog 中 `local_seq` 不能代表任一 origin 的连续序列，可能导致卡死、重复写或错误基线恢复。
- 修复方案：baseline response 携带源端在切面时刻的 per-origin applied vector，例如 `{origin, epoch, max_origin_seq}` 列表。apply baseline 必须在同一事务中为每个 origin 设置 `applied_seq=max_origin_seq`，并清理/重建 `row_winner` 与 `table_state`。

### C-04：SelectionPush 入站被重铸为本地 origin，且可在全片 ACK 前被普通广播下游

- 位置：`src/sync/SyncWorker.cpp:727`、`src/sync/SyncWorker.cpp:734`、`src/sync/apply/CapturedWriteTemplate.cpp:336`、`src/sync/SyncWorker.cpp:1024`
- 描述：接收 selection push 后，代码把 `WriteParams.origin` 设为 `config_.nodeId()`，`CapturedWriteTemplate` 将其作为 changelog origin；随后普通 `broadcastTopeer()` 读取所有 changelog 进行广播。changelog 中没有 push_id/完成状态过滤，无法阻止“只落了部分 chunk”的变更被下游 peer 接收。
- 规格依据：设计 §5.5/§5.6 要求上行 UPSERT 推送在中心 changelog 保留原发送方 origin，且中心在 `push_progress.status=done`（全片 ACK）前不得向下游广播本 push 的直选变更。
- 影响：冲突仲裁 rank/origin 语义被破坏；半截选择推送可能外泄到 C/D 节点，造成下游可见不完整父子集合，属于协议级数据损坏风险。
- 修复方案：selection push 的 captured changelog 应保留远端 origin 或至少携带 `source_origin/source_push_id`；广播层必须过滤未完成 push 生成的 changelog 区间。可在 changelog 增加 `push_id/chunk_seq/push_status`，全片完成后一次性解锁广播。

## High 问题

### H-01：quarantine 重放顺序不是原始到达顺序，且先删后重放会丢失载荷

- 位置：`src/sync/schema/QuarantineStore.cpp:42`、`src/sync/schema/QuarantineStore.cpp:61`、`src/sync/SyncWorker.cpp:342`
- 描述：`drainReady()` 只按 `origin_seq ASC` 排序，忽略 `created_ms/id/origin/epoch`，跨 origin 会重排；同时在返回 payload 前删除 quarantine 行。调用方只在 decode 为 changeset 时尝试处理，非 schema 类失败不会恢复 quarantine。
- 规格依据：设计 §5.8/§6.1 要求 schema 不匹配隔离后按原到达顺序重放，失败不得丢载荷。
- 修复方案：quarantine 表以 `id` 或明确 `arrival_seq` 排序；重放与删除应在“成功应用/明确损坏”后完成，或提供 `markReplayed(id)`，任何非成功状态保留原行。

### H-02：选择推送的 `includeFkDependencies` / `pruneConsistentDependencies` API 未生效

- 位置：`include/dbridge/sync/SyncSelection.h:27`、`src/sync/selection/FkClosureBuilder.cpp:221`、`src/sync/SyncWorker.cpp:1506`
- 描述：`SyncSelection` 暴露 `includeFkDeps()` 和 `pruneConsistent()`，但 `FkClosureBuilder::build()` 没有接收 selection/options，固定调用 `buildClosure(..., true)`，总是展开 FK 且总是剪枝。
- 规格依据：设计 §4.4 要求 `SyncSelection::Builder` 字段完整实现，§5.5 明确选择集解析包含 FK 闭包和一致性剪枝开关。
- 修复方案：把 `SyncSelection` 或显式 options 传入 `FkClosureBuilder::build()`；`includeFkDependencies=false` 时仅发送直选记录，`pruneConsistentDependencies=false` 时禁用一致性缓存剪枝。

### H-03：导出路径未运行 `ProfileValidator`，`columnOrder` 等加载期失败条件可绕过

- 位置：`src/DataBridge.cpp:77`、`src/DataBridge.cpp:347`、`src/service/ExportService.cpp:590`、`src/profile/ProfileValidator.cpp:417`
- 描述：`loadProfileDoc()` 仅解析 JSON 并缓存；`ImportService` 在读取 Excel header 后才调用 `ProfileValidator`，但 `ExportService::run()` 不调用 validator。结果 `export.columnOrder` 的 unknown header、duplicate、raw SQL 互斥、反向查找替换后 header 集校验只在导入路径生效。
- 规格依据：`export-column-order` 要求 unknown/duplicate/raw SQL conflict 在 profile validation 阶段失败；`export-reverse-lookup` 要求 `columnOrder` 基于替换后的 A/H header 集校验。
- 修复方案：增加 export 专用 validator，在 `DataBridge::exportExcel()` 刷新 catalog 后、调用 `ExportService` 前执行；没有 Excel 输入时，应根据 profile routes 和 reverse lookup 规则构建可达 header 集。导入专用的 mixed discriminator 校验需按模式拆分，避免误伤 export-only profile。

### H-04：FK 注入/lookup 失败被扩大为整行跳过，违背“失败链路抑制，兄弟路由继续”

- 位置：`src/mapping/FkInjector.cpp:21`、`src/service/ImportService.cpp:557`、`src/service/ImportService.cpp:619`
- 描述：`FkInjector::inject()` 返回失败 payload 索引集合，但调用方忽略返回值。随后写入阶段把任意 row-level error 收集到 `failedExcelRows`，整 Excel 行全部跳过。这样一个子路由 FK 注入失败会阻止同一行中无关兄弟路由落库。
- 规格依据：`fk-injection` §D11 要求 parent 链路错误只级联抑制 descendants，无关 sibling routes 继续；row-lookup 也要求 route-local visibility。
- 修复方案：`RowContext` 中保存 failed route indices；写入前只过滤失败 route 及其 descendants。`abortOnError=false` 时，未受影响的兄弟 payload 应继续参与 batch uniqueness、FK preflight 和 upsert。

## Medium 问题

### M-01：`SyncConfig::Builder` 完整性校验不足

- 位置：`include/dbridge/sync/SyncConfig.h:281`
- 描述：`build()` 只校验 `nodeId/sqlitePath/outboxDir/inboxDir`，未校验 role/center/peer 列表、自 peer/重复 peer、ack/broadcast/selection/chunk/lag 阈值正数、schemaVersion 等。
- 规格依据：设计 §4.4 明确 `SyncConfig::Builder` 必须包含全部字段并做完整性校验。
- 修复方案：补全字段级 validation，并将非法配置返回明确错误码；对多 peer 拓扑至少校验 `nodeId` 不在 `peerNodes`、center 节点存在且不冲突。

### M-02：`verifySchemaFingerprint` 配置项未被使用

- 位置：`include/dbridge/sync/SyncConfig.h:248`、`src/sync/schema/SchemaGuard.cpp:18`
- 描述：配置暴露 `verifySchemaFingerprint(bool)`，但 `SchemaGuard::verifyPayload()` 总是比较 fingerprint，`SyncWorker` 没有按配置关闭或降级。
- 规格依据：设计 §4.4 把该字段列入 `SyncConfig::Builder` 的显式契约。
- 修复方案：初始化 `SchemaGuard` 时传入策略；关闭时仍校验 schema version，但 fingerprint mismatch 应按配置跳过或降级为 warning。

### M-03：同步 schema eligibility 窄于规格，拒绝复合 PK / WITHOUT ROWID 等合法模型

- 位置：`src/sync/schema/SchemaEligibility.cpp:76`
- 描述：实现显式拒绝 composite PRIMARY KEY，并只按单列 PK 支撑 selection/FK closure。规格允许 ordinary base rowid 或 WITHOUT ROWID，只要求有显式非空 PK/sync key 和完整 unique UPSERT target。
- 规格依据：设计 §5.1/G-04 同步表 eligibility 要求校验完整 unique target，而非强制单列 PK。
- 修复方案：短期在文档和错误码中声明 MVP 限制；长期把 `SelectionResolver/FkClosureBuilder/RowWinnerStore/UpsertExecutor` 的 PK 表示升级为列元组，并校验非 partial/expression unique index。

### M-04：`ChangesetApplier` 在 allow-list 为空时会先接受所有表，再过滤内部表

- 位置：`src/sync/apply/ChangesetApplier.cpp:144`
- 描述：`filterCb()` 对 `!c->syncTables` 直接 `return 1`，内部 `__sync_*` 表过滤写在后面。正常初始化会展开 sync tables，但空库或异常路径下恶意/损坏 changeset 可能触达内部表。
- 规格依据：设计 §5.1/§6.1 要求同步元表不属于业务载荷，内部状态只能由同步引擎维护。
- 修复方案：先拒绝 `__sync_` 前缀，再处理 allow-list；同时在 decode/apply 前对 changeset 涉及表集合做一次显式内部表过滤。

### M-05：Mixed export-only profile 与通用 validator 规则冲突

- 位置：`src/profile/ProfileLoader.cpp:671`、`src/profile/ProfileValidator.cpp:350`
- 描述：Loader 允许 Mixed profile 没有 discriminator，以支持 export-only；但 validator 无模式区分，固定要求 `discriminator.source` 非空。如果后续按 H-03 修复导出校验，export-only mixed profile 会被误拒。
- 规格依据：归档 `add-export-column-order` / `add-export-reverse-lookup` 允许 Mixed 导出侧只依赖 classes；导入路由才需要 discriminator。
- 修复方案：把 validator 拆成 import validator 与 export validator；只有 import 路径要求 discriminator 和 Excel header 中存在 discriminator source。

### M-06：`SchemaGuard`/quarantine 只在启动 drain，一次 schema 变化后的 ready payload 可能长期不重放

- 位置：`src/sync/SyncWorker.cpp:342`
- 描述：注释说明 quarantine 只在 worker init drain 一次，因为 worker schema version 固定。baseline 或运行期 schema version 更新后，ready quarantine 不会立即重新评估。
- 规格依据：设计 §5.8/§8.2 要求 schema 兼容后隔离 payload 可重放，baseline/迁移收口后应触发重判。
- 修复方案：在 baseline 成功、schemaVersion 更新或显式 rescan 时触发 `drainReady()`；同时按 H-01 修复先删后 replay 的丢载荷问题。

## 第一部分：功能合规性评审（同步引擎）

同步引擎主干已经实现了 session capture、changelog、applied vector、row winner、outbox/inbox/ACK、baseline、selection push 等模块；`SessionRecorder::sealInto()` 在事务内 append changelog，`CapturedWriteTemplate` 把 apply、vector、table_state、changelog 包在统一事务骨架中，这是符合设计方向的。

不合规集中在协议闭环：前台完成条件不是“足额 ACK”；发送锚点按 last-sent 而不是 ACK 驱动；baseline 没有携带/应用 per-origin cut；selection push 没有把“全片完成前不下游广播”和“保留原 origin”落到 changelog/广播层。以上问题会直接破坏 FR-4、FR-8、FR-11、FR-17。

## 第二部分：功能合规性评审（Excel 导入/导出）

`ProfileLoader` 已解析 row lookup、fkInject 数组、`columnOrder`、`exportRoundtrip/exportOnMissing`、时间格式 side-object 和 `epochSec`；`ExportService` 也实现了 A/H header 替换、batch prefetch、source 非 NULL 优先、`columnOrder` 重排与 classColumn 默认 prepend。

主要缺口是 validator 调用点：导出不走 `ProfileValidator`，导致 OpenSpec 中要求“profile loading/validation SHALL fail”的条件在导出侧可绕过。导入侧 FK 注入错误粒度也过粗，`abortOnError=false` 时仍会因为任意 row error 跳过整行，和 fk-injection/row-lookup 的 route-local 语义不一致。

## 第三部分：同步协议正确性详细评审

- ChangeLog 捕获：本地写和 selection push 经 `CapturedWriteTemplate` fresh capture；入站 changeset 保留 blob 转发。但 selection push origin 重铸与半截广播破坏协议。
- 向量连续性：`AppliedVectorStore::check()` 实现严格连续；baseline reset 未推进到权威 cut，是当前最大缺口。
- ACK：typed ACK codec/文件通道存在，但 foreground completion 和 outbound retransmit 均未按 ACK 窗口建模。
- 冲突仲裁：`RowWinnerStore` 使用 `(rank, origin_seq)` 最大元，基本符合；baseline 后 winner 重置需要与 applied vector 修复联动。
- Quarantine：存储和启动 drain 已有，但顺序、删除时机、运行期重放触发不满足规格。
- SelectionResolver：`addWhere` 在 Builder 中拒绝 raw SQL，符合 MVP 安全收口；但 FK 依赖选项未被实际传递。

## 第四部分：安全性审查

SQL 注入防御总体较好：`SqlBuilder::quoteIdent()` 和绑定参数覆盖了 upsert、join、FK preflight、selection resolver、schema/table_state 多数路径。`SyncSelection::Builder::addWhere()` 拒绝 raw SQL，避免开放项未完成前引入注入面。

仍需修复两类安全边界：一是 `ChangesetApplier::filterCb()` 应无条件拒绝 `__sync_*` 内部表；二是配置项 `verifySchemaFingerprint` 未生效，容易让调用方误以为已降低 fingerprint 校验策略。`PRAGMA foreign_keys` 在 baseline apply 中大体成对恢复，本轮未发现新的 FK OFF 泄漏路径，但 baseline vector 语义仍需修复。

## 第五部分：边界处理审查

重复 chunk：`CapturedWriteTemplate::branchBC()` 在 `__sync_push_chunk_progress` 中检查 `(push_id, chunk_seq)`，同 checksum no-op、不同 checksum 报 corrupt，符合规格。

乱序 chunk：`SyncWorker::processSelectionPushArtifact()` 在 `chunkSeq > 0` 时要求前序 chunk 已 applied，能避免子 chunk 提前应用。但因为 changelog/广播层没有 push 完成屏障，已应用的前几个 chunk 仍可能被下游看到。

schema mismatch：SchemaGuard 能拒绝 mismatch 并 quarantine；但 quarantine drain 先删后重放且只启动时 drain，运行期兼容后重放不完整。

多 peer ACK：当前实现没有 expected ACK set，也没有 per-peer foreground completion 判断，是多 peer 场景最高优先级缺口。

## 修复优先级汇总表

| 编号 | 文件 | 严重度 | 一句话描述 |
| --- | --- | --- | --- |
| C-01 | `src/sync/SyncWorker.cpp:906` | Critical | 任意 Changeset ACK 即完成前台 sync，未等待所有 peer 足额 ACK |
| C-02 | `src/sync/SyncWorker.cpp:1086` | Critical | 发送水位在 ACK 前推进，未 ACK payload 可能永久不重发 |
| C-03 | `src/sync/apply/AppliedVectorStore.cpp:69` | Critical | baseline reset 未推进 applied_seq，且混用 local_seq/origin_seq |
| C-04 | `src/sync/SyncWorker.cpp:734` | Critical | selection push 被重铸 origin，并可能在全片 ACK 前下游广播 |
| H-01 | `src/sync/schema/QuarantineStore.cpp:42` | High | quarantine 不按到达顺序重放且先删后 replay |
| H-02 | `src/sync/selection/FkClosureBuilder.cpp:221` | High | selection FK 依赖/剪枝开关未生效 |
| H-03 | `src/service/ExportService.cpp:590` | High | 导出路径不运行 ProfileValidator，OpenSpec 校验可绕过 |
| H-04 | `src/service/ImportService.cpp:557` | High | FK 注入失败集合被忽略，任意错误扩大为整行跳过 |
| M-01 | `include/dbridge/sync/SyncConfig.h:281` | Medium | SyncConfig Builder 校验不完整 |
| M-02 | `src/sync/schema/SchemaGuard.cpp:18` | Medium | verifySchemaFingerprint 配置项未生效 |
| M-03 | `src/sync/schema/SchemaEligibility.cpp:76` | Medium | schema eligibility 过窄，拒绝规格允许的复合 PK/无 rowid 模型 |
| M-04 | `src/sync/apply/ChangesetApplier.cpp:144` | Medium | allow-list 为空时内部 `__sync_*` 表过滤被绕过 |
| M-05 | `src/profile/ProfileValidator.cpp:350` | Medium | Mixed export-only profile 与通用 validator discriminator 规则冲突 |
| M-06 | `src/sync/SyncWorker.cpp:342` | Medium | quarantine 只在启动时 drain，baseline/schema 兼容后重放触发不足 |
