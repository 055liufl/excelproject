# SQLite 同步工具实现评审报告（第六轮）

> 评审日期：2026-06-25  
> 评审范围：src/ 全量代码（修复第五轮全部 Critical+High+Medium 后）  
> 规格基准：设计文档 v0.5 + 实现计划 v0.5 + openspec 5 项规格

## 执行摘要

总体符合度：**62/100**，较第五轮约有提升，但不是可交付状态。

- 同步主链路：**52/100**。第五轮多处点状修复存在，但同步导入、RowWinner 到达序无关、场景2写线程、空 `syncTables` 语义仍破坏 FR-1/FR-6/NFR-6。
- 传输与 ACK：**70/100**。typed ACK、空 drain、fsync 主文件检查基本落地；PushChunkAck 路由字段仍不足，目录 fsync 返回值未检查。
- ETL/OpenSpec：**80/100**。`columnOrder`、reverse lookup、`epochSec` 限制基本实现；主要风险转为 SQL 标识符裸拼与大查询内存化。
- 线程与门控：**58/100**。`SyncEngine` 快照字段加锁，但 `stop()` 不释放 gate，`BatchTransfer` 未共享 gate，`ComparisonSession` 仍绕过 `SyncWorker`。
- 安全性：**55/100**。值绑定基本存在，但表名/列名大量裸拼，缺少统一 `quoteIdent()`。

## Critical 级问题

### C-05：同步导入仍嵌套事务，且没有走 CapturedWriteTemplate 分支 C
- **文件**：src/sync/SyncWorker.cpp:800、src/sync/SyncWorker.cpp:819、src/service/ImportService.cpp:573
- **规格条款**：设计 §5.4 分支 C / §5.2 同事务收割 / 计划 T1.6、T3.1
- **问题描述**：`submitImportSync()` 先创建 `WriteTxn`，随后调用 `ImportService::run(...)`，但没有传 `manageTransaction=false`，`ImportService` 默认在 `db.transaction()` 中开启内层事务。SQLite/QSqlDatabase 不支持这种嵌套事务，实际同步导入会失败或进入未定义事务状态。同时该路径没有构造 `WriteParams{LocalWrite}` 调用 `CapturedWriteTemplate::branchBC()`，也没有复用 `UpsertExecutor`，只是在外面手工 `SessionRecorder::begin/sealInto`。
- **修复建议**：将同步导入改为：Excel/Profile → `RowMutation`，再调用 `CapturedWriteTemplate::execute(LocalWrite)`；如果短期仍复用 `ImportService`，至少传 `manageTransaction=false`，并检查 `rec_->begin/sealInto/txn.commit` 返回值，失败必须 rollback。

### C-06：RowWinner 预过滤仍是占位，低 rank 跨批后到仍可能覆盖高 rank
- **文件**：src/sync/apply/ChangesetApplier.cpp:269、src/sync/apply/ChangesetApplier.cpp:319、src/sync/apply/ChangesetApplier.cpp:342
- **规格条款**：设计 §5.6 G-01 / 计划 T1.7b、T2.2 / FR-6
- **问题描述**：`filterByWinner()` 注释称要移除低规范序 changeset 行，但实现无法重建单行 changeset，最终无条件返回原始 changeset。若低 rank 变更后到且 SQLite apply 不触发冲突回调（例如 old-image 匹配的 UPDATE/DELETE），数据库内容已被低 rank 写入；后续 `updateWinnersFromChangeset()` 只是拒绝更新 winner，不会回滚业务表覆盖。
- **修复建议**：不要保留占位预过滤。可选方案：在 apply 前按 changeset 预扫描生成逐行 winner 判定并拒绝整发/重建合法 changeset；或 apply 后对被低 rank 覆盖的行执行同事务恢复。必须补“高 rank 先到、低 rank 跨批后到不覆盖”的可执行测试。

### C-07：ComparisonSession 保存绕过单写线程，InboundTableGate 未接入 worker
- **文件**：src/sync/diff/ComparisonSession.cpp:207、src/sync/diff/ComparisonSession.cpp:233、src/sync/diff/ComparisonSession.cpp:391、src/sync/diff/ComparisonSession.cpp:413、src/sync/SyncWorker.cpp:377
- **规格条款**：设计 §2.4 / §5.8 / §7.6 / NFR-6
- **问题描述**：`createComparisonSession()` 只打开只读连接，且把同一个 `rconn` 同时传作 `wconn`；factory 不调用 `initialize()`。`save()` 直接调用 `staging_.save(wconn_, upsert_, ...)` 写库，没有入 `SyncWorker`、没有 `CapturedWriteTemplate`、没有 changelog/table_state/session capture。`InboundTableGate::shouldDefer()` 只定义未使用，`SyncWorker::processArtifact()` 直接 apply 载荷，没有预扫描表集合并 pending。
- **修复建议**：ComparisonSession factory 应绑定现有 `SyncContext/SyncWorker`，会话初始化必须 open gate；`save()` 入 worker 写队列并走 LocalWrite 分支；`processArtifact()` 在 decode 后预扫描 payload tables，命中 gate 时保持 ledger `seen` 且不 ACK。

### C-08：空 syncTables 未按“全部用户表”展开，可能静默不捕获任何业务变更
- **文件**：src/sync/schema/SchemaEligibility.cpp:14、src/sync/capture/SessionRecorder.cpp:27、include/dbridge/sync/SyncConfig.h:171、include/dbridge/sync/SyncConfig.h:272
- **规格条款**：设计 §4.4 / §5.1 / FR-1、FR-2
- **问题描述**：规格定义 `syncTables` 为空时表示全部用户表。当前 `SchemaEligibility::verify()` 对空列表直接返回 true，`SessionRecorder::begin()` 也只 attach 显式列表。Builder 没要求必须配置 `syncTables`。因此默认配置下初始化成功但 session 没 attach 用户表，业务写不会进入 changeset，属于最危险的“成功但漏同步”。
- **修复建议**：初始化时将空 `syncTables` 展开为所有非 `sqlite_%`、非 `__sync_%` 普通用户表；eligibility 和 session attach 都使用展开后的 canonical 表集合，并持久到 worker 配置快照。

### C-09：SelectionPush/LocalWrite 的 UpsertExecutor 吞掉逐行失败，模板仍会 commit 并 ACK
- **文件**：src/sync/apply/UpsertExecutor.cpp:67、src/sync/apply/UpsertExecutor.cpp:78、src/sync/apply/UpsertExecutor.cpp:83、src/sync/apply/CapturedWriteTemplate.cpp:266、src/sync/apply/CapturedWriteTemplate.cpp:344
- **规格条款**：设计 §5.4 分支 B/C / §7.3 / E_SYNC_APPLY_FK、E_SYNC_APPLY_CONSTRAINT
- **问题描述**：`UpsertExecutor::apply()` 对单行 `exec()` 失败只追加 `RowError` 并继续，最终返回 true。`CapturedWriteTemplate::branchBC()` 不检查 `rowErrors`，继续 seal、mark chunk applied、commit。中心可能部分落片后发送 `PushChunkAck(ok=true)`，破坏选择推送“每片事务原子 + FK 安全 + 成功才 ACK”。
- **修复建议**：同步分支的 `UpsertExecutor` 必须 fail-fast；任何行级 DB/FK/constraint 错误都返回 false，`branchBC()` rollback，错误码映射为 `E_SYNC_APPLY_FK` 或 `E_SYNC_APPLY_CONSTRAINT`，且不得 mark chunk/ACK。

## High 级问题

### H-01：ChangelogStore 仍用 INSERT OR IGNORE，origin_seq 冲突会被静默吞掉
- **文件**：src/sync/capture/ChangelogStore.cpp:136
- **规格条款**：第五轮 C-04 / 设计 §6.1 UNIQUE(origin,stream_epoch,origin_seq)
- **问题描述**：第五轮要求本地写 `origin_seq` 不能用 `INSERT OR IGNORE` 吞冲突。当前 changelog 所有 append 都通过 `insertRow()`，仍是 `INSERT OR IGNORE`。一旦 `origin_seq` 重复，调用方拿到“成功”，但 changelog 未新增，后续广播/ACK/审计都错位。
- **修复建议**：改为普通 `INSERT INTO __sync_changelog ...`；唯一键冲突应返回 false，并上报 `E_SYNC_INIT` 或专用 invariant 错误。

### H-02：stop() 终态没有释放 ForegroundGate
- **文件**：src/sync/SyncEngine.cpp:157、src/sync/SyncEngine.cpp:169、src/sync/SyncEngine.cpp:288
- **规格条款**：第五轮 C-03 / 设计 §7.1
- **问题描述**：`onWorkerProgress()` 和 `onWorkerError()` 会调用 `releaseGateIfTerminal()`，`ForegroundGate::release()` 也是幂等的；但 `SyncEngine::stop()` 直接 `setProgress(Stopped)` 后返回，没有调用 `releaseGateIfTerminal(SyncState::Stopped)`。如果前台操作期间用户 stop，gate 会保持 held，后续 `sync/startImport` 继续 `E_BUSY`。
- **修复建议**：`stop()` 成功设置 `Stopped` 后必须调用 `releaseGateIfTerminal(SyncState::Stopped)`；同时考虑不要 `requestStop()` 停整个后台 worker，因为规格中 stop 只停止前台 operation。

### H-03：缺口载荷虽然保持 seen 且不 ACK，但过早报 E_SYNC_GAP
- **文件**：src/sync/apply/AppliedVectorStore.cpp:20、src/sync/apply/CapturedWriteTemplate.cpp:81、src/sync/SyncWorker.cpp:430、src/sync/SyncWorker.cpp:319
- **规格条款**：设计 §6 G-05 / 计划 S-01
- **问题描述**：`AppliedVectorStore::check()` 正确区分 NoOp/Apply/Gap；`processArtifact()` 在 `ok=false` 时也不会 mark consumed/ACK。但 `branchA()` 一遇 gap 立即返回 `E_SYNC_GAP`，`processChangesetArtifact()` 立即发 Error。计划要求 gap 先作为 pending `seen`，三时机重扫，超时后才 `E_SYNC_GAP` → baseline。
- **修复建议**：gap 应返回可区分的 Pending 状态，不作为 Error 立即污染前台；只有 `InboxLedger::stalePending()` 超阈值后发 `E_SYNC_GAP`。

### H-04：PushChunkAck 缺少目标 peer，ACK 文件名把 pushId 当 toPeer
- **文件**：src/sync/transport/AckChannel.cpp:43、src/sync/transport/AckChannel.cpp:48、src/sync/SyncWorker.cpp:562
- **规格条款**：设计 §5.4 typed ACK / §5.11 ACK 制品 schema
- **问题描述**：`ChangesetAck` 有 `toPeer`，但 `PushChunkAck` 没有。`AckChannel::flush()` 用 `ackArtifactName(nodeId_, ack.pushId, nowMs)`，即 ACK 文件名目标字段是 `pushId` 而不是发送节点。若第三方按文件名路由，选择推送 ACK 无法回到 origin，发送端永远等 ACK 超时。
- **修复建议**：给 `PushChunkAck` 增加 `toPeer/originPeer`，接收端从 payload header 的 `origin` 填入，文件名使用真实 peer；payload 内也保留 peer 供审计。

### H-05：SqlBuilder/ForeignKeyPreflight 仍裸拼标识符，未统一 quoteIdent()
- **文件**：src/sql/SqlBuilder.cpp:31、src/sql/SqlBuilder.cpp:83、src/sql/SqlBuilder.cpp:96、src/validation/ForeignKeyPreflight.cpp:121、src/validation/ForeignKeyPreflight.cpp:124、src/service/ExportService.cpp:274
- **规格条款**：MVP 设计 §8 / B10 安全性 / OpenSpec lookup/fk preflight
- **问题描述**：值基本通过 bind 传入，但表名、列名、alias、orderBy 仍大量直接拼接。Profile 是外部 JSON，恶意或误写标识符会造成 SQL 注入或查询语义破坏。`UpsertExecutor` 局部 quote 也没有处理 `"` 转义，不能替代统一策略。
- **修复建议**：实现 `SqlBuilder::quoteIdent(QString)`，双引号包裹并把内部 `"` 转义为 `""`；所有表名/列名/alias/orderBy/fk preflight 都必须通过 SchemaCatalog 白名单校验 + quote。

### H-06：SessionRecorder 无法区分“无 changeset”和“采集失败”
- **文件**：src/sync/capture/SessionRecorder.cpp:59、src/sync/capture/SessionRecorder.cpp:70、src/sync/capture/SessionRecorder.cpp:96、src/sync/capture/SessionRecorder.cpp:108
- **规格条款**：设计 §5.2 / §5.4 分支 B/C
- **问题描述**：`collectChangeset()` 在失败和 `nChangeset==0` 时都返回默认 `QByteArray()`；`sealInto()` 用 `changeset.isNull()` 判断错误，因此“无变更”会被当成错误，后面的 `changeset.isEmpty()` 成功分支几乎不可达。同步导入当前还忽略 `sealInto()` 返回值，会进一步掩盖该错误。
- **修复建议**：让 `collectChangeset()` 返回结构体 `{ok, changeset}`，或失败返回 null、无变更返回非 null empty（例如 `QByteArray("")`）；调用方必须检查 seal/commit。

### H-07：同步元数据 DDL 表齐但关键索引/状态约束不足
- **文件**：src/sync/SyncDDL.h:10、src/sync/SyncDDL.h:96、src/sync/SyncDDL.h:109
- **规格条款**：设计 §6.1 / §5.11
- **问题描述**：11 张 `__sync_*` 表均存在，`__sync_push_progress` 到 `__sync_push_chunk_progress`/`__sync_frozen_manifest` 的 FK 也存在。但除 changelog 外缺少常用查询索引，例如 push 状态扫描、inbox status/first_seen、quarantine replay；状态值也与规格不一致（规格为 `streaming/done/failed`，实现使用 `sending/receiving/done/failed`）。
- **修复建议**：补齐规格中的关键索引和 CHECK/状态枚举约束；若采用 `sending/receiving`，需要同步修订规格，否则统一为 `streaming`。

## Medium 级问题

### M-01：BatchTransfer 没有使用共享 ForegroundGate
- **文件**：src/batch/BatchTransfer.cpp:40、src/batch/BatchTransfer.cpp:78、src/batch/BatchTransfer.cpp:117
- **规格条款**：设计 §4.3 / §5.9 / B8
- **问题描述**：`startImport/startExport` 只检查自身状态，没有通过 `SyncContext` 的 `ForegroundGate` 与 `sync/syncSelected` 互斥。虽然导入在 sync active 时会尝试走 `ctx->importFn`，但没有 gate acquire/release；导出也完全不入 gate。
- **修复建议**：`createBatchTransfer` 应获取同库 `SyncContext`；`startImport/startExport` 受理前 acquire gate，终态 release，和 `ISyncEngine` 共用同一个前台槽。

### M-02：OutboxWriter 仍忽略目录 fsync 返回值
- **文件**：src/sync/transport/OutboxWriter.cpp:111、src/sync/transport/OutboxWriter.cpp:114
- **规格条款**：第五轮 M-04 / 设计 §5.11
- **问题描述**：主 payload 和 `.ready` 文件的 `flush/fsync` 已检查，但包含目录的 `fsync(dirFd)` 返回值被忽略。rename 与 ready 文件创建的目录项持久性失败时，函数仍返回 true。
- **修复建议**：检查 `::fsync(dirFd)` 返回值，失败时返回 `E_SYNC_TRANSPORT`；Windows 下用平台条件编译表达实际语义。

### M-03：SchemaEligibility 只校验显式 syncTables，未防止配置漏表
- **文件**：src/sync/schema/SchemaEligibility.cpp:17、include/dbridge/sync/SyncConfig.h:171
- **规格条款**：设计 §4.4
- **问题描述**：除 C-08 的空表语义外，当前没有校验 `syncTables` 与 catalog 的最终同步表集合是否被 session/table_state/selection 使用同一份规范列表。后续模块各自读取 `config_.syncTables()`，容易出现 eligibility 通过但 session attach/selection/broadcast 表集合不一致。
- **修复建议**：初始化阶段生成 `SyncTableSet` 值对象，包含表名、PK、冲突目标、schema fingerprint，并传给所有同步模块。

### M-04：TableStateStore 更新失败被吞，DiffEngine 可信度下降
- **文件**：src/sync/apply/CapturedWriteTemplate.cpp:121、src/sync/apply/CapturedWriteTemplate.cpp:338
- **规格条款**：设计 §6.2 / 场景2 FR-12
- **问题描述**：`table_state` 是场景2零全量判等的权威输入，但 Branch A 和 Branch B/C 都把 `applyMutations()` 失败当非致命并清空错误。这样后续 `DiffEngine` 可能基于陈旧 checksum 给出假 green/red。
- **修复建议**：同步写事务内 `table_state` 维护失败应 rollback；如果确实要软失败，必须标记表态 stale 并禁止场景2基于该表态判等。

### M-05：Baseline 应用使用 INSERT OR REPLACE，可能触发 DELETE+INSERT 语义
- **文件**：src/sync/baseline/BaselineManager.cpp:150、src/sync/baseline/BaselineManager.cpp:157
- **规格条款**：MVP 设计 §1 / 同步设计 §5.10
- **问题描述**：导入规格明确禁止 `INSERT OR REPLACE`，同步基线虽然先删除表数据，但逐行 `INSERT OR REPLACE` 仍可能触发 SQLite 的 replace 语义和触发器副作用。若删除顺序/FK 暂未处理好，还会造成非预期级联。
- **修复建议**：基线应用阶段应按拓扑禁用/延迟 FK 或先 DELETE 后普通 INSERT；不要依赖 REPLACE。

### M-06：BatchTransfer 已用 xlsxPath 和 profile/catalog 快照，但同步活跃时导出仍不使用只读快照门控
- **文件**：src/batch/BatchTransfer.cpp:48、src/batch/BatchTransfer.cpp:56、src/batch/BatchTransfer.cpp:86、src/batch/BatchTransfer.cpp:94、src/batch/BatchTransfer.cpp:240
- **规格条款**：B8 / 设计 §4.3
- **问题描述**：第五轮路径误用已修：导入/导出都检查 `options.xlsxPath`，并 snapshot profile/catalog。但导出后台直接开只读连接运行，未与前台 sync gate 协调，也没有明确 read snapshot 生命周期。
- **修复建议**：导出受理也进 ForegroundGate，并在后台连接上显式 `BEGIN`/`COMMIT` read snapshot。

## Low / Info

### L-01：syncSelected 建 push_progress 使用 INSERT OR IGNORE，冲突不会更新状态
- **文件**：src/sync/SyncWorker.cpp:966、src/sync/SyncWorker.cpp:970
- **规格条款**：设计 §5.4 typed ACK / §6.1
- **问题描述**：发送端写首片前会创建 `status='sending'`，但如果同 push_id 重试残留旧行，`INSERT OR IGNORE` 不会刷新 `total_chunks/status/updated_ms`。
- **修复建议**：改成 `ON CONFLICT(push_id) DO UPDATE SET status='sending', total_chunks=excluded.total_chunks, updated_ms=excluded.updated_ms`。

### L-02：SelectionResolver 的 PK/PRAGMA 标识符仍局部裸 quote
- **文件**：src/sync/selection/SelectionResolver.cpp:14、src/sync/selection/SelectionResolver.cpp:54
- **规格条款**：B10 安全性
- **问题描述**：已删除原始 WHERE 执行，但表名/列名仍用简单双引号格式化，没有复用统一 quote/白名单。
- **修复建议**：同 H-05，统一 quoteIdent + catalog 校验。

## 第五轮修复验证

| 项 | 结论 | 核验说明 |
|---|---|---|
| C-01 syncSelected 空 profileName 阻断 | ✓ 已修复 | `DataBridge::snapshotCatalog()` 已实现（src/DataBridge.cpp:301），`syncSelected()` 调用它（src/sync/SyncEngine.cpp:211），worker 内链路走 `SelectionResolver → FkClosureBuilder → ChunkStreamer → OutboxWriter`（src/sync/SyncWorker.cpp:915、937、951、1004）。 |
| C-02/M-05 SelectionPush push_progress 发送端 | △ 部分修复 | 发送端写首片前建 `status='sending'`（src/sync/SyncWorker.cpp:966），接收端成功后发 `PushChunkAck`（src/sync/SyncWorker.cpp:562），全片 ACK 后置 done/Completed（src/sync/SyncWorker.cpp:642）。但 PushChunkAck 无目标 peer，ACK 文件名用 pushId 当 toPeer（src/sync/transport/AckChannel.cpp:48）。 |
| C-03 ForegroundGate 不释放 | △ 部分修复 | `onWorkerProgress`/`onWorkerError` 会释放 terminal gate（src/sync/SyncEngine.cpp:265、284），`release()` 幂等（src/sync/ForegroundGate.h:23）。但 `stop()` 没释放 gate（src/sync/SyncEngine.cpp:157-170）。 |
| C-04 本地写 origin_seq=0 | △ 部分修复 | `localOriginSeq_` 从 MAX 恢复（src/sync/SyncWorker.cpp:183），同步导入 seal 前调用 `nextLocalOriginSeq()`（src/sync/SyncWorker.cpp:825）。但 changelog 仍 `INSERT OR IGNORE`（src/sync/capture/ChangelogStore.cpp:136），且同步导入未走 branch C、仍有嵌套事务。 |
| H-01 SelectionPush 出站缺 schemaFingerprint | ✓ 已修复 | `schemaFp` 来自 `guard_->fingerprint()` 并写入 header（src/sync/SyncWorker.cpp:963、987）。 |
| H-02 空 drain 超时 | ✓ 已修复 | `enqueueDrain()` 返回 bool（src/sync/SyncWorker.h:80），`sync()` 无 payload 时立即 Completed 并释放 gate（src/sync/SyncEngine.cpp:138、148）。 |
| H-03 SchemaEligibility 误拒 INTEGER PRIMARY KEY | ✓ 已修复 | 单列 `INTEGER PRIMARY KEY` 豁免 `notnull=0`（src/sync/schema/SchemaEligibility.cpp:146、177）。 |
| H-05 ACK 写失败静默丢弃 | ✓ 已修复 | `AckChannel::flush()` 收集失败项并保留 pending（src/sync/transport/AckChannel.cpp:30、43、57），调用方上报 `E_SYNC_TRANSPORT`（src/sync/SyncWorker.cpp:675）。 |
| H-07 branchA 错误码 | ✓ 已修复 | schema 不匹配用 `E_SYNC_SCHEMA_MISMATCH`（src/sync/apply/CapturedWriteTemplate.cpp:89），apply 失败区分 FK/constraint（src/sync/apply/CapturedWriteTemplate.cpp:102）。 |
| M-03 resolveWhere 原始 SQL | ✓ 已修复 | `resolveWhere()` 明确失败，不执行 whereExpr（src/sync/selection/SelectionResolver.cpp:73）。 |
| M-04 fsync 未检查 | △ 部分修复 | 主文件和 `.ready` fsync 已检查（src/sync/transport/OutboxWriter.cpp:59、97），目录 fsync 仍未检查（src/sync/transport/OutboxWriter.cpp:114）。 |
| L-01 FkInjector no-op overload | ✓ 已修复 | 旧 overload 返回 false 并给错误（src/mapping/FkInjector.cpp:10）。 |

## 优先修复清单

| 优先级 | ID | 标题 | 文件 | 工作量 |
|---|---|---|---|---|
| P0 | C-05 | 同步导入嵌套事务且未走分支 C | SyncWorker.cpp / ImportService.cpp | L |
| P0 | C-06 | RowWinner 预过滤仍占位 | ChangesetApplier.cpp | L |
| P0 | C-07 | ComparisonSession 绕过 SyncWorker，gate 未接入 | ComparisonSession.cpp / SyncWorker.cpp | L |
| P0 | C-08 | 空 syncTables 静默漏捕获 | SchemaEligibility.cpp / SessionRecorder.cpp | M |
| P0 | C-09 | SelectionPush 行失败仍 commit/ACK | UpsertExecutor.cpp / CapturedWriteTemplate.cpp | M |
| P1 | H-01 | ChangelogStore INSERT OR IGNORE 吞冲突 | ChangelogStore.cpp | S |
| P1 | H-02 | stop() 不释放 ForegroundGate | SyncEngine.cpp | S |
| P1 | H-04 | PushChunkAck 无目标 peer | AckChannel.cpp / PayloadCodec.h | M |
| P1 | H-05 | SQL 标识符裸拼 | SqlBuilder.cpp / ForeignKeyPreflight.cpp / ExportService.cpp | M |
| P2 | M-01 | BatchTransfer 未共享 ForegroundGate | BatchTransfer.cpp / SyncContext.cpp | M |
| P2 | M-02 | 目录 fsync 未检查 | OutboxWriter.cpp | S |

## 总结

第五轮修复确实解决了一批表层问题：`snapshotCatalog()`、空 drain、schema fingerprint、INTEGER PK、ACK pending 保留、`resolveWhere()` 禁 raw SQL、旧 `FkInjector` overload 都已落地。但核心同步不变量仍未闭环：本地同步导入不走统一写模板、RowWinner 到达序无关仍不可执行、场景2未接入单写线程、空 `syncTables` 会静默漏捕获、SelectionPush 分片失败会被错误 ACK。建议先按 P0 清单收口，再跑 FR-1/FR-6/G-05/场景2 的故障夹具，否则继续叠加传输或 ETL 功能会掩盖同步一致性缺陷。
