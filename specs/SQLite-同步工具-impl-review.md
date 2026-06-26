# SQLite 同步工具实现评审报告（第十一轮）

评审日期：2026-06-26

## 总体评分

总分：72 / 100

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 合规性 | 70 | Excel/OpenSpec 扩展实现较完整；同步协议 M1/M2/M5 有多处闭环缺口 |
| 正确性 | 66 | ACK、baseline、quarantine、prepared 绑定复用存在确认级逻辑风险 |
| 安全性 | 78 | 主路径普遍使用 `quoteIdent`/绑定参数；raw `export.sql` 属设计授权风险 |
| 架构质量 | 76 | 模块分层清晰，但 SyncWorker 仍承担协议、传输、baseline、ACK 多职责 |
| 边界处理 | 69 | gap pending、重复 chunk、schema eligibility 有处理；多 peer ACK、baseline 失败路径不足 |

本轮已按要求读取 15 个设计/OpenSpec/归档变更文档，并枚举读取 `src/` 下 122 个 `.cpp/.h` 源文件。以下结论均以当前源码为准。

## 第一部分：功能合规性评审

### 1.1 同步引擎核心（SyncEngine / SyncWorker）

| 功能 | 状态 | 代码证据与评审 |
| --- | --- | --- |
| ChangeLog 捕获 | 已实现但有边界风险 | `SessionRecorder::begin` 使用 `sqlite3session_create/attach` 捕获同步表，见 `src/sync/capture/SessionRecorder.cpp:19`、`src/sync/capture/SessionRecorder.cpp:30`；本地 import 通过 worker 包裹 session，见 `src/sync/SyncWorker.cpp:1297`、`src/sync/SyncWorker.cpp:1321`。偏差：`ImportService` 直接写路径仍保留，只有 `DataBridge::importExcel` 在 sync active 时拦截，见 `src/DataBridge.cpp:181`。 |
| 向量时钟/严格连续 | 部分实现 | `AppliedVectorStore::check` 实现 `seq==applied+1` 才应用，见 `src/sync/apply/AppliedVectorStore.cpp:28`、`src/sync/apply/CapturedWriteTemplate.cpp:76`。偏差：baseline 后 `AppliedVectorStore::reset` 把 `applied_seq` 置 0，而不是重置到 baseline 覆盖的远端高水位，见 `src/sync/apply/AppliedVectorStore.cpp:76`、`src/sync/baseline/BaselineManager.cpp:255`。 |
| 差量计算 | 部分实现 | `DiffEngine::tableDiffs` 使用 `schema_fingerprint + row_count + content_checksum`，符合 G-06，见 `src/sync/diff/DiffEngine.cpp:108`。行级 diff 仍依赖传入的 `remoteRows` 全量列表和 offset 窗口，见 `src/sync/diff/DiffEngine.cpp:134`，不完全满足“只物化受影响行 + keyset 分页”。 |
| 冲突仲裁 | 部分实现 | `ConflictArbiter::beats` 使用 rank/seq，见 `src/sync/conflict/ConflictArbiter.cpp:13`；`ChangesetApplier` 在 conflict callback 中读写 `RowWinnerStore`，见 `src/sync/apply/ChangesetApplier.cpp:158`。偏差：同 rank 同 seq 在 callback 用 `>`，后置 winner 更新用 `>=`，语义不一致，见 `src/sync/apply/ChangesetApplier.cpp:197`、`src/sync/apply/ChangesetApplier.cpp:447`。 |
| 出站/入站通道 | 已实现但 ACK 闭环有严重偏差 | outbox 原子写 `tmp -> rename -> .ready`，见 `src/sync/transport/OutboxWriter.cpp:29`、`src/sync/transport/OutboxWriter.cpp:72`；inbox 以 `.ready + ledger` 扫描，见 `src/sync/transport/InboxWatcher.cpp:28`。偏差：前台 `sync()` 的 ACK 等待在 drain 后才开启，存在丢 ACK 竞态，见 `src/sync/SyncEngine.cpp:166`、`src/sync/SyncEngine.cpp:181`。 |
| 基线管理 | 部分实现但存在 Critical | `BaselineManager` 可序列化/应用基线，见 `src/sync/baseline/BaselineManager.cpp:18`、`src/sync/baseline/BaselineManager.cpp:211`。偏差：baseline apply 失败路径会泄漏 `PRAGMA foreign_keys=OFF`，且 applied vector 未推进到基线锚点，见 `src/sync/baseline/BaselineManager.cpp:222`、`src/sync/baseline/BaselineManager.cpp:253`。 |

### 1.2 差量与冲突处理（DiffEngine / ConflictArbiter / RebaseEngine）

- 已实现：表级判等不使用 `high_water_seq`，符合设计 G-06，见 `src/sync/diff/DiffEngine.cpp:108`。
- 偏差：`DiffEngine::rowDiffs` 使用调用方提供的 `remoteRows` 并在内存中切片，见 `src/sync/diff/DiffEngine.cpp:134`、`src/sync/diff/DiffEngine.cpp:143`，与设计中的远端 keyset 分页/只物化受影响行不完全一致。
- 已实现：`RebaseEngine` 调用 `sqlite3rebaser_*`，见 `src/sync/conflict/RebaseEngine.cpp:32`；广播时失败发 `E_SYNC_REBASE_FAILED` 并跳过发送，见 `src/sync/SyncWorker.cpp:1044`、`src/sync/SyncWorker.cpp:1050`。
- 偏差：`broadcastTopeer` 在写出 payload 后推进 `last_sent_seq`，见 `src/sync/SyncWorker.cpp:1084`；但前台完成只看任意 ACK，而不是所有 peer 的足额 ACK，见 `src/sync/SyncWorker.cpp:904`。

### 1.3 变更捕获（ChangelogStore / SessionRecorder）

- 已实现：`__sync_changelog` DDL 包含 `kind/origin/origin_seq/stream_epoch/schema_fingerprint/changeset`，见 `src/sync/SyncDDL.h:13`。
- 已实现：`ChangelogStore::appendForward` 保留原始 origin/originSeq，见 `src/sync/capture/ChangelogStore.cpp:30`，符合“origin 不重铸”。
- 偏差：schema mismatch 的 quarantine 存入的是 `dec.changeset`，见 `src/sync/SyncWorker.cpp:558`；启动重放却按完整 `PayloadCodec` 解码，见 `src/sync/SyncWorker.cpp:353`、`src/sync/schema/QuarantineStore.cpp:48`。该隔离载荷不可重放。

### 1.4 选择与传输（SelectionResolver / ChunkStreamer / OutboxWriter / InboxWatcher）

- 已实现：`SelectionResolver` 拒绝 raw `addWhere`，避免任意 WHERE 注入，见 `src/sync/selection/SelectionResolver.cpp:80`。
- 已实现：FK 闭包 BFS、拓扑排序、超规模报错，见 `src/sync/selection/FkClosureBuilder.cpp:85`、`src/sync/selection/FkClosureBuilder.cpp:138`、`src/sync/selection/FkClosureBuilder.cpp:226`。
- 已实现：分片按估算字节预算切块，单行超限报 `E_SYNC_SELECTION_TOO_LARGE`，见 `src/sync/selection/ChunkStreamer.cpp:64`。
- 偏差：receiver 侧 `processSelectionPushArtifact` 对已有 `__sync_push_progress` 无条件写回 `status='streaming'`，见 `src/sync/SyncWorker.cpp:633`，重复 chunk 可把已完成状态退回 streaming。

### 1.5 模式守卫（SchemaGuard / SchemaEligibility）

- 已实现：初始化时在 session attach 前执行 eligibility，见 `src/sync/SyncWorker.cpp:210`、`src/sync/SyncWorker.cpp:222`。
- 已实现：拒绝视图、虚表、shadow table、无 PK、nullable PK、复合 PK，见 `src/sync/schema/SchemaEligibility.cpp:56`、`src/sync/schema/SchemaEligibility.cpp:60`、`src/sync/schema/SchemaEligibility.cpp:68`、`src/sync/schema/SchemaEligibility.cpp:76`。
- 已实现：schema fingerprint 覆盖列、唯一索引、FK，见 `src/sync/schema/SchemaGuard.cpp:36`、`src/sync/schema/SchemaGuard.cpp:72`、`src/sync/schema/SchemaGuard.cpp:86`。
- 偏差：`SchemaGuard::verifyPayload` 仅比较全局版本/指纹，缺少 payload 涉及表集合的更细粒度隔离策略，见 `src/sync/schema/SchemaGuard.cpp:18`。

### 1.6 应用层（ChangesetApplier / UpsertExecutor / SelectionPushApplier）

- 已实现：changeset 使用 `sqlite3changeset_apply_v2`，见 `src/sync/apply/ChangesetApplier.cpp:111`。
- 已实现：Branch A 同事务执行 applied-vector、apply、table_state、appendForward、commit，见 `src/sync/apply/CapturedWriteTemplate.cpp:67`、`src/sync/apply/CapturedWriteTemplate.cpp:121`、`src/sync/apply/CapturedWriteTemplate.cpp:142`、`src/sync/apply/CapturedWriteTemplate.cpp:150`。
- 已实现：Branch B/C fresh session + `UpsertExecutor` + changelog seal，见 `src/sync/apply/CapturedWriteTemplate.cpp:223`、`src/sync/apply/CapturedWriteTemplate.cpp:300`、`src/sync/apply/CapturedWriteTemplate.cpp:336`。
- 偏差：`SelectionPushApplier` 只是对模板的薄包装，实际逻辑在 `SyncWorker::processSelectionPushArtifact`，见 `src/sync/apply/SelectionPushApplier.cpp:9`、`src/sync/SyncWorker.cpp:619`，职责边界与设计文件不完全一致。

### 1.7 基线管理（BaselineManager）

- 已实现：基线导出读取表数据并压缩，见 `src/sync/baseline/BaselineManager.cpp:18`、`src/sync/baseline/BaselineManager.cpp:86`。
- 已实现：应用基线清表、插入、重置 table_state、清 row_winner，见 `src/sync/baseline/BaselineManager.cpp:128`、`src/sync/baseline/BaselineManager.cpp:165`、`src/sync/baseline/BaselineManager.cpp:263`、`src/sync/baseline/BaselineManager.cpp:270`。
- 缺失/偏差：`serializeTables` 的 `sourceMaxSeq` 来自 `MAX(local_seq)`，见 `src/sync/baseline/BaselineManager.cpp:75`；而 `applied_vector` 是 `(origin, epoch, origin_seq)` 语义。用 local_seq 做基线锚点会混淆序列空间。

## 第二部分：OpenSpec 特性合规性

### 2.1 FK 注入（FkInjector）

- 已实现：profile 支持多组 `fkInject[]`，loader 解析数组和 `pairs`，见 `src/profile/ProfileLoader.cpp:399`。
- 已实现：validator 要求 `from` 是同 profile route，子列唯一，支持 lookup-derived 与 Excel-derived 分组约束，见 `src/profile/ProfileValidator.cpp:205`、`src/profile/ProfileValidator.cpp:234`、`src/profile/ProfileValidator.cpp:277`。
- 已实现：运行期从父 payload 注入多个 pair，冲突报错，见 `src/mapping/FkInjector.cpp:188`、`src/mapping/FkInjector.cpp:216`。
- 已实现：复合 preflight 用 `AND` 条件和绑定参数，见 `src/validation/ForeignKeyPreflight.cpp:118`、`src/validation/ForeignKeyPreflight.cpp:130`。

### 2.2 行查找（Router / Mapper）

- 已实现：lookup prefetch 先扫 Excel key，再批量 SELECT，见 `src/service/ImportService.cpp:248`、`src/service/ImportService.cpp:313`。
- 已实现：0 命中、>1 命中、空 key、类型 cast 失败均产生行级错误，见 `src/service/ImportService.cpp:146`、`src/service/ImportService.cpp:185`、`src/service/ImportService.cpp:203`。
- 已实现：lookup 输出进入 route-local payload，可参与 conflictVals/FK 注入，见 `src/service/ImportService.cpp:216`、`src/service/ImportService.cpp:228`。

### 2.3 导出列顺序（ExportService / ExportHelpers）

- 已实现：`columnOrder` loader 只接受字符串数组，见 `src/profile/ProfileLoader.cpp:807`。
- 已实现：unknown header、duplicate、raw SQL 互斥校验，见 `src/profile/ProfileValidator.cpp:417`、`src/profile/ProfileValidator.cpp:473`、`src/profile/ProfileValidator.cpp:481`。
- 已实现：`reorderHeaders` 保持“显式列在前 + 自然后缀”，见 `src/service/ExportHelpers.h:9`。
- 已实现：Mixed `classColumn` 未显式列出时 prepend，显式列出时按 `columnOrder`，见 `src/service/ExportService.cpp:747`、`src/service/ExportService.cpp:751`。

### 2.4 导出反向查找（ExportService）

- 已实现：反向 prefetch 用 H 值查 G 表并缓存，见 `src/service/ExportService.cpp:258`、`src/service/ExportService.cpp:341`。
- 已实现：`exportRoundtrip=false` 跳过 reverse lookup，保留 H 列，见 `src/service/ExportService.cpp:265`。
- 已实现：NOT_FOUND/AMBIGUOUS 按 `exportOnMissing` 处理并产生 `E_REVERSE_LOOKUP_*`，见 `src/service/ExportService.cpp:476`、`src/service/ExportService.cpp:497`、`src/service/ExportService.cpp:521`。
- 偏差：反向 lookup 路径会全量加载行到内存，见 `src/service/ExportService.cpp:898`，大表导出不满足流式目标。

### 2.5 时间格式（TemporalConvert / ProfileSpec）

- 已实现：profile/column 级 `dateFormat/datetimeFormat/timeFormat` 结构，见 `src/profile/ProfileSpec.h:47`、`src/profile/ProfileSpec.h:126`。
- 已实现：slot token 校验、epochSec 限制、fallback 校验，见 `src/profile/ProfileLoader.cpp:224`、`src/profile/ProfileLoader.cpp:907`、`src/profile/ProfileLoader.cpp:931`。
- 已实现：导入时空值转 NULL，structured temporal bypass parse，字符串按 primary/fallback 解析，见 `src/mapping/Mapper.cpp:82`、`src/mapping/Mapper.cpp:84`、`src/mapping/Mapper.cpp:90`。
- 已实现：导出时 `E_TIME_PARSE_DB` 为非阻塞 cell 级错误，见 `src/service/ExportService.cpp:65`、`src/service/ExportService.cpp:80`。
- 偏差：导入方向 `formatValue` 失败时直接转 NULL，不产生 `E_TIME_PARSE`，见 `src/mapping/Mapper.cpp:100`、`src/mapping/TemporalConvert.cpp:323`。例如 `datetimeFormat.db.type=epochSec` 但结构不是 DateTime 时会静默 NULL。

## 第三部分：Excel 批量导入导出合规性

### 3.1 ImportService

- 已实现：打开 Excel、读取 header、profile validation、lookup、mapping、FK inject、preflight、统一事务写入，见 `src/service/ImportService.cpp:424`、`src/service/ImportService.cpp:446`、`src/service/ImportService.cpp:511`、`src/service/ImportService.cpp:611`。
- 已实现：默认 all-or-nothing；有 row error 且 `abortOnError=true` 时不写库，见 `src/service/ImportService.cpp:592`。
- 偏差：导入写循环仍内置 `SqlBuilder + QSqlQuery`，未复用同步层 `UpsertExecutor`，见 `src/service/ImportService.cpp:625`、`src/sync/apply/UpsertExecutor.cpp:13`。这与计划中 “import + 上行 + save 三路共用 UpsertExecutor” 不完全一致。

### 3.2 ExportService

- 已实现：单表/多表/Mixed 导出、显式 SQL、columnOrder、reverse lookup、时间格式导出，见 `src/service/ExportService.cpp:590`、`src/service/ExportService.cpp:612`、`src/service/ExportService.cpp:797`。
- 偏差：columnOrder 或 reverse lookup 路径会将所有结果行读入 `QVector` 再写 Excel，见 `src/service/ExportService.cpp:843`、`src/service/ExportService.cpp:899`，对大文件导出内存风险较高。

### 3.3 DataBridge 公共 API

- 已实现：`open/loadProfile/loadProfileFromString/importExcel/exportExcel/generateAutoProfileJson`，见 `src/DataBridge.cpp:99`、`src/DataBridge.cpp:120`、`src/DataBridge.cpp:181`、`src/DataBridge.cpp:318`。
- 已实现：sync active 时拒绝 direct import，见 `src/DataBridge.cpp:181`。
- 偏差：`DataBridge::close` 未主动等待/取消仍在运行的 `BatchTransfer` 任务；`BatchTransfer` 持有 `DataBridge&` 并异步调用 `snapshotProfileCatalog`，见 `src/batch/BatchTransfer.cpp:475`。如果宿主先销毁 bridge，再销毁 batch transfer，存在生命周期约束未由 API 强制。

## 第四部分：安全与数据完整性

### 4.1 SQL 注入风险

- 良好：主路径表名/列名拼接基本使用 `SqlBuilder::quoteIdent`，见 `src/sql/SqlBuilder.cpp:8`、`src/service/ImportService.cpp:339`、`src/validation/ForeignKeyPreflight.cpp:126`、`src/sync/selection/SelectionResolver.cpp:59`。
- 设计授权风险：`ExportService` 直接执行 `profile.exportSpec.explicitSql`，见 `src/service/ExportService.cpp:797`。这是 profile 授权能力，不是传统注入，但必须把 profile 视为可信配置；不可信 profile 会获得任意 SELECT 能力。
- 低风险：`PRAGMA busy_timeout = ` 拼接数字来自 `ConnectionSpec`，见 `src/DataBridge.cpp:53`；建议仍用范围校验防止异常值。

### 4.2 事务安全性

- Critical：`BaselineManager::applyBaseline` 在事务前关闭 FK，`av.reset/ts.resetFromBaseline/rw.resetAll` 失败路径只 rollback，不重新打开 FK，见 `src/sync/baseline/BaselineManager.cpp:222`、`src/sync/baseline/BaselineManager.cpp:255`、`src/sync/baseline/BaselineManager.cpp:263`、`src/sync/baseline/BaselineManager.cpp:270`。
- High：`UpsertExecutor` 与 `ImportService` 复用 prepared query 时反复 `addBindValue`，存在绑定列表累积/陈旧值风险，见 `src/sync/apply/UpsertExecutor.cpp:51`、`src/service/ImportService.cpp:641`。

### 4.3 竞态与线程安全

- Critical：`SyncEngine::sync` 在 `enqueueDrain` 返回后才 `startAckWait`，见 `src/sync/SyncEngine.cpp:166`、`src/sync/SyncEngine.cpp:181`；如果 ACK 在 drain 期间到达，`processAckArtifact` 因 `ackWaiting_` 为 false 不会完成前台任务，见 `src/sync/SyncWorker.cpp:904`。
- High：前台 `sync()` 只用一个 `ackWaiting_` bool；任意一个 changeset ACK 即 `Completed`，见 `src/sync/SyncWorker.cpp:904`，不满足多 peer 足额 ACK。
- Medium：`BatchTransfer` 通过 `QtConcurrent` 持有 `this` 和 `DataBridge&`，析构会等待任务，见 `src/batch/BatchTransfer.cpp:421`；但外部必须保证 `DataBridge` 生命周期长于 transfer，API 未强制。

### 4.4 资源泄漏

- 已处理：`OutboxWriter` 对写失败清理 tmp/final/ready，见 `src/sync/transport/OutboxWriter.cpp:47`、`src/sync/transport/OutboxWriter.cpp:86`。
- 风险：baseline 失败后 FK pragma 泄漏属于连接状态泄漏，见 `src/sync/baseline/BaselineManager.cpp:255`。

## 第五部分：正确性与边界处理

### 5.1 已发现的 Bug

- `src/sync/SyncEngine.cpp:181` — ACK wait 在 drain 之后才开启，可能错过已到达 ACK — 建议像 `syncSelected` 一样在 enqueue 前设置等待目标，并记录本轮 peer/origin/seq 集合。
- `src/sync/SyncWorker.cpp:904` — 任意 changeset ACK 即完成前台同步 — 建议按 peer + origin + epoch + seq 聚合，全部满足后才 `Completed`。
- `src/sync/baseline/BaselineManager.cpp:255` — baseline 重置 `applied_vector` 时传入 `sourceMaxSeq` 但 `AppliedVectorStore::reset` 写 `applied_seq=0` — 建议按 origin_seq 语义记录基线覆盖高水位，不要混用 local_seq。
- `src/sync/apply/AppliedVectorStore.cpp:76` — reset 固定写 0，导致 baseline 后旧 payload 可能被重放或继续 gap — 建议新增 `resetTo(origin, epoch, appliedSeq, generation)`。
- `src/sync/baseline/BaselineManager.cpp:263` — baseline 失败 rollback 后未恢复 `PRAGMA foreign_keys=ON` — 建议 RAII guard 管理 FK 开关，所有 return 路径恢复。
- `src/sync/SyncWorker.cpp:558` — schema mismatch quarantine 保存 raw changeset 而非完整 payload — 建议在 `processArtifact` 层 quarantine 原始 `data`，或让 `drainReady` 按 changeset blob 直接 apply。
- `src/sync/schema/QuarantineStore.cpp:48` — replay 取出 payload 后由 `PayloadCodec::decode` 解码 — 与上条 raw changeset 存储不兼容。
- `src/sync/apply/UpsertExecutor.cpp:53` — 复用 `QSqlQuery` 时不断 `addBindValue` — 建议每次 `finish/clear` 后重新 prepare，或按 index `bindValue(i, v)` 覆盖。
- `src/service/ImportService.cpp:659` — 同样复用 prepared query 并累加绑定值 — 建议与 `UpsertExecutor` 统一修复。
- `src/sync/SyncWorker.cpp:633` — receiver 收到重复 push chunk 会把 `push_progress.status` 写回 `streaming` — 建议 `done/failed` 终态不可回退。
- `src/sync/SyncWorker.cpp:755` — receiver 发送 chunk ACK 后未在本端判定全片完成并标记 `push_progress=done` — 建议按 `totalChunks` 统计本端已应用 chunk 后更新状态。
- `src/service/ExportService.cpp:843` — columnOrder 导出全量缓存所有行 — 建议构建 header index 后流式重排，避免大表内存峰值。
- `src/service/ExportService.cpp:899` — reverse lookup 路径全量缓存 rowDataList — 建议分批预取/分批写出，或明确限制规模。

### 5.2 边界用例覆盖

- 已覆盖倾向：乱序 changeset pending，见 `src/sync/apply/CapturedWriteTemplate.cpp:82`；重复 chunk 同 checksum 幂等，见 `src/sync/apply/CapturedWriteTemplate.cpp:189`；FK 闭包缺父报错，见 `src/sync/selection/FkClosureBuilder.cpp:114`。
- 缺口：多 peer ACK 足额、ACK 先于 wait、baseline 失败恢复 FK、quarantine schema mismatch 重放、prepared query 多行复用、receiver duplicate chunk 状态回退。

### 5.3 错误传播路径

- 已实现：worker error 会进入 `SyncEngine::onWorkerError` 并终止前台状态，见 `src/sync/SyncEngine.cpp:278`。
- 偏差：`BaselineManager::applyBaseline` 内部错误会 wrap 成 `E_SYNC_BASELINE_FAILED`，但 `PRAGMA foreign_keys=ON` 恢复失败未检查，见 `src/sync/baseline/BaselineManager.cpp:286`。
- 偏差：`UpsertExecutor::apply` 行级错误通过 `errors` 返回但函数仍返回 true，见 `src/sync/apply/UpsertExecutor.cpp:56`、`src/sync/apply/UpsertExecutor.cpp:72`；当前 `CapturedWriteTemplate` 已二次检查 rowErrors，见 `src/sync/apply/CapturedWriteTemplate.cpp:309`，但其他调用方容易误用。

## 第六部分：架构质量

### 6.1 模块职责与耦合

- 优点：sync 目录按 capture/apply/conflict/diff/schema/selection/transport/baseline 拆分，结构与设计文档基本一致，见 `src/sync/SyncWorker.h:1` 所在模块布局。
- 问题：`SyncWorker` 集中处理初始化、inbox scan、ACK、baseline、selection push、broadcast、peer evict，单文件 1591 行，协议状态高度耦合，见 `src/sync/SyncWorker.cpp:161`、`src/sync/SyncWorker.cpp:466`、`src/sync/SyncWorker.cpp:989`、`src/sync/SyncWorker.cpp:1168`。

### 6.2 SOLID 原则

- DIP/OCP 部分满足：`RebaseEngine`、`ConflictArbiter`、`Transport` 类独立，见 `src/sync/conflict/RebaseEngine.cpp:32`、`src/sync/conflict/ConflictArbiter.cpp:5`。
- SRP 偏差：`SyncWorker` 内部仍直接构造 selection、payload、baseline 的完整流程，见 `src/sync/SyncWorker.cpp:1446`。

### 6.3 可测试性

- 优点：`ImportService::onPrefetch`、`ForeignKeyPreflight::onProbe` 提供测试 hook，见 `src/service/ImportService.cpp:246`、`src/validation/ForeignKeyPreflight.cpp:119`。
- 缺口：ACK 足额等待、baseline replay、quarantine replay 没有独立状态对象，测试必须驱动 worker 大循环，成本高。

### 6.4 代码重复

- ImportService 与 UpsertExecutor 均维护 UPSERT 执行与 prepared cache，见 `src/service/ImportService.cpp:625`、`src/sync/apply/UpsertExecutor.cpp:13`。
- reverse lookup 与 import lookup 有相似的 tuple key/cast/prefetch 逻辑，见 `src/service/ImportService.cpp:28`、`src/service/ExportService.cpp:96`，可抽公共 LookupBatcher，但不建议在修复 Critical 前重构。

## 第七部分：总结与优先级修复建议

### Critical（必须修复）

- [C-1] `src/sync/SyncEngine.cpp:181` — 前台 `sync()` ACK 等待开启过晚，存在 ACK 丢失竞态 — 在 enqueue drain 前建立本轮 ACK 目标集合。
- [C-2] `src/sync/SyncWorker.cpp:904` — 任意 ACK 即 Completed，不满足足额 ACK — 按 peer/origin/epoch/seq 或 push chunk 全集完成后才结束前台状态。
- [C-3] `src/sync/baseline/BaselineManager.cpp:255` — baseline 未把 applied vector 推进到正确远端高水位 — 区分 local_seq 与 origin_seq，应用基线后记录可跳过的 origin_seq。
- [C-4] `src/sync/baseline/BaselineManager.cpp:263` — baseline 失败路径泄漏 `foreign_keys=OFF` — 用 RAII guard 包住 FK 开关并检查恢复结果。
- [C-5] `src/sync/SyncWorker.cpp:558` — quarantine 保存 raw changeset，启动 replay 期望完整 payload — 保存原始 payload 或调整 QuarantineStore 类型。

### High（强烈建议）

- [H-1] `src/sync/apply/UpsertExecutor.cpp:53` — prepared query 复用时 `addBindValue` 可能累积绑定 — 改为按索引 `bindValue` 或每次重新 prepare。
- [H-2] `src/service/ImportService.cpp:659` — ImportService 存在同类绑定复用风险 — 统一改造为共享 `UpsertExecutor`。
- [H-3] `src/sync/SyncWorker.cpp:633` — 重复 chunk 可将 `push_progress` 终态回退到 streaming — `done/failed` 应为不可回退终态。
- [H-4] `src/sync/SyncWorker.cpp:755` — receiver 端未在全片应用后标记 push done — 应在发送最后一个 chunk ACK 前后更新本地 push_progress。
- [H-5] `src/sync/apply/ChangesetApplier.cpp:197` — rank/seq 同分胜负规则与后置 winner 更新不一致 — 统一为严格全序，加入 origin 作为 tie-breaker。

### Medium（建议修复）

- [M-1] `src/service/ExportService.cpp:843` — columnOrder 路径全量缓存导出行 — 改为流式重排。
- [M-2] `src/service/ExportService.cpp:899` — reverse lookup 路径全量缓存行 — 加批处理或显式规模阈值。
- [M-3] `src/sync/diff/DiffEngine.cpp:134` — rowDiff 仍依赖外部全量 remoteRows — 接入 keyset fetchRemoteRows。
- [M-4] `src/sync/SyncWorker.cpp:1446` — selection push 编排集中在 SyncWorker — 拆为 SelectionPushCoordinator 便于单测。
- [M-5] `src/mapping/Mapper.cpp:100` — temporal serialize 失败静默 NULL — 转为 `E_TIME_PARSE` 行级错误。

### Low（可选优化）

- [L-1] `src/DataBridge.cpp:53` — `busy_timeout` 拼接数字未做范围限制 — 对负值/过大值归一化。
- [L-2] `src/sync/schema/SchemaGuard.cpp:18` — schema guard 只有全局 fingerprint — 后续可扩展表级 payload fingerprint。
- [L-3] `src/service/ImportService.cpp:28` 与 `src/service/ExportService.cpp:96` — lookup tuple/cast 逻辑重复 — Critical 修复后抽公共工具。
- [L-4] `src/sync/transport/OutboxWriter.cpp:118` — POSIX 目录 fsync 对 Windows 兼容注释与实现不完全匹配 — 可用平台宏区分。
