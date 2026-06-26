# SQLite 同步工具实现评审报告（第九轮）

> 评审日期：2026-06-25  
> 评审范围：src/ 全量代码（修复第八轮全部问题后）  
> 规格基准：设计文档 v0.5 + 实现计划 v0.5 + openspec 5 项规格

## 执行摘要

总体符合度：**68/100**，较第八轮 **63/100** 有进步，但仍未达到可交付闭环。

本轮确认第八轮大部分点修复真实落地：baseline fallback 不再本地自导自入，ACK 已进 ledger，ComparisonSession 公开初始化接口已补，discard 会触发 rescan，多 peer 文件名冲突已缓解，SyncConfig/SyncSelection 接口补齐。主要阻塞从“接口缺失”转为“语义闭环不完整”：逐行胜者仍不能证明到达序无关，场景2 factory 的表态 epoch 错误，baseline 只有隔离没有源端请求/应用协议，反向 lookup 的 H 列 alias 缺失会破坏字段匹配。

维度评分：同步核心 57/100，场景2 55/100，传输可靠性 72/100，ETL openspec 84/100，SQL 安全 70/100，测试覆盖 38/100。

## Critical 级问题

### C-11：RowWinner 低 rank DELETE 保护仍不是可靠的到达序无关实现

- **文件**：src/sync/apply/ChangesetApplier.cpp:102
- **规格条款**：SQLite 同步设计 §5.6、plan T1.7b、FR-6/G-01
- **问题描述**：`apply()` 看似在 `sqlite3changeset_apply_v2()` 前调用 `filterByWinner()`（src/sync/apply/ChangesetApplier.cpp:102），但 `filterByWinner()` 仍是占位实现：一旦尝试重建单行 changeset 即回退原始 changeset（src/sync/apply/ChangesetApplier.cpp:414-420），最后也无条件返回原 changeset（src/sync/apply/ChangesetApplier.cpp:437-442）。因此低 rank DELETE 仍可能先被 SQLite 成功应用，再依赖 post-apply 手工恢复。
- **影响**：G-01/FR-6 要求“高 rank 先到提交、低 rank 跨批后到不覆盖”，当前核心路径没有真正预过滤低 rank DELETE；手工恢复失败时事务也不会失败，终态无法由规格保证。
- **修复建议**：不要保留伪过滤。应在 apply 前构造可用的 filtered changeset，或改为 authoritative apply 前按 winner 对 DELETE 做事务内显式跳过；恢复失败必须返回错误并回滚。

### C-12：RowWinner DELETE 恢复失败被吞掉，apply 仍可能 ACK 破坏后的终态

- **文件**：src/sync/apply/ChangesetApplier.cpp:278
- **规格条款**：SQLite 同步设计 §5.6、plan T1.6/T1.7b
- **问题描述**：C-08 的 `exec()` 错误检查已经存在，但处理方式是 `winners.clear()` 后继续（src/sync/apply/ChangesetApplier.cpp:278-281），`updateWinnersFromChangeset()` 返回 `void`（src/sync/apply/ChangesetApplier.cpp:221），`apply()` 不知道恢复失败（src/sync/apply/ChangesetApplier.cpp:130）。外层 `CapturedWriteTemplate::branchA()` 会继续 advance applied vector、append changelog 并提交（src/sync/apply/CapturedWriteTemplate.cpp:119-153），随后 `processArtifact()` 会 mark consumed 并发 ACK（src/sync/SyncWorker.cpp:509-520）。
- **影响**：低 rank DELETE 已删除业务行、恢复又失败时，本节点仍会推进 applied vector 并 ACK；后续“高 rank 节点可重投递”的注释没有实际重投递触发点。
- **修复建议**：`updateWinnersFromChangeset()` 应返回 bool/错误；恢复 SQL 失败必须让 branchA 回滚，ledger 保持 seen 或 quarantine，不得 ACK。

### C-13：baseline 缺口收口仍没有源端 baseline 协议

- **文件**：include/dbridge/sync/SyncTypes.h:83
- **规格条款**：SQLite 同步设计 §5.10、FR-8，plan T5.1
- **问题描述**：C-09 修复已删除本地 self export/apply，但系统仍没有 baseline payload kind：`PayloadKind` 只有 `Changeset` 和 `SelectionPush`（include/dbridge/sync/SyncTypes.h:83），codec 也只编码这两类业务 payload（src/sync/payload/PayloadCodec.h:152-156）。`runBaselineFallbackFor()` 只发 `E_SYNC_GAP`、复制文件到 quarantineDir 并 mark consumed（src/sync/SyncWorker.cpp:1018-1034）。
- **影响**：缺口超时后不会请求源端权威 baseline，也不会应用源端 baseline、重置 applied-vector/table_state/row_winner。FR-8 仍只能靠人工介入。
- **修复建议**：新增 baseline request/response artifact 和 codec kind；源端导出 baseline，接收端走 `BaselineManager::applyBaseline()`，成功后以源端 `sourceMaxSeq` 重置锚点。

## High 级问题

### H-13：ComparisonSession factory 使用 streamEpoch=0，公开 `tableDiffs()` 表级判等失真

- **文件**：src/sync/diff/ComparisonSession.cpp:498
- **规格条款**：SQLite 同步设计 §5.8、§6.2，plan T4.4
- **问题描述**：C-10 已补 `IComparisonSession::initialize()`（include/dbridge/sync/IComparisonSession.h:56）和 owning wrapper 转发（src/sync/diff/ComparisonSession.cpp:456-459），但 factory 明确用 `kPlaceholderEpoch = 0`（src/sync/diff/ComparisonSession.cpp:498-506）。`DiffEngine::tableDiffs()` 按 `streamEpoch` 读取 `__sync_table_state`（src/sync/diff/DiffEngine.cpp:12-29），真实 worker epoch 是 `SyncWorker::streamEpoch_`（src/sync/SyncWorker.cpp:236），不是 0。
- **影响**：公开 factory 返回的 session 即使能 initialize，`tableDiffs()` 也会把本地表态读成 not found，进而误判 `OnlyRemote` 或 `Identical`（src/sync/diff/DiffEngine.cpp:33-40）。场景2的“表级判等以 checksum 为准”不能成立。
- **修复建议**：把 worker 当前 streamEpoch 写入 `SyncContext`，factory 从 context 注入；或 `DiffEngine` 在缺 table_state 时即时计算本地 checksum，不能用 0 占位。

### H-14：ExportService H 列扩展 SELECT 缺少 alias，reverse lookup 取值键不稳定

- **文件**：src/service/ExportService.cpp:171
- **规格条款**：openspec/export-reverse-lookup，openspec/export-column-order
- **问题描述**：H-10 已 quote `route.table.routeColumn`（src/service/ExportService.cpp:171-172），但没有 `AS quoteIdent(sp.second)`。主 SELECT 原有列由 `SqlBuilder` 使用 `AS source` 稳定字段名（src/sql/SqlBuilder.cpp:72-74），而 H 列扩展直接追加 `"table"."dbColumn"`（src/service/ExportService.cpp:171-175）。后续收集 H 值按 `rowData.value(sp.second)` 取值（src/service/ExportService.cpp:376-383、425-432）。
- **影响**：Qt/SQLite 返回字段名可能是 `dbColumn`、`table.dbColumn` 或带引号表达式；一旦不是 `sp.second`，reverse lookup 会把 H 值当 NULL/miss，输出 A 列错误。
- **修复建议**：H 列 SELECT 应生成 `quoteIdent(route.table).quoteIdent(sp.second) AS quoteIdent(sp.second)`；若有跨表同名 H 列，需用内部唯一 alias 并维护 alias→dbColumn 映射。

### H-15：selection push 入站没有严格连续序列，分片到达序和缺口语义不符合 G-05

- **文件**：src/sync/SyncWorker.cpp:600
- **规格条款**：SQLite 同步设计 §5.4、plan T2.9/T2.10/T2.13、G-05
- **问题描述**：changeset 分支有 `AppliedVectorStore::check()` 严格连续（src/sync/apply/CapturedWriteTemplate.cpp:74-94），但 selection push 使用 `originSeq=0`（src/sync/SyncWorker.cpp:1261）并仅按 `push_id, chunk_seq` 写 `__sync_push_chunk_progress`（src/sync/apply/CapturedWriteTemplate.cpp:178-193、325-345）。`processSelectionPushArtifact()` 收到任意 chunk 都直接应用（src/sync/SyncWorker.cpp:596-718），没有检查前置 chunk 是否已到、是否应 pending。
- **影响**：长推送分片可能后片先落库，拓扑序和“半截不外泄”规格被破坏；`totalChunks` ACK 完成条件不能补偿已经提前落库的后片。
- **修复建议**：接收端应对 selection push 增加 per-push 连续门控：`chunk_seq == next_expected` 才 apply，否则 ledger 保持 seen；或分片先落 staging，全部到齐后按 topo 一次性提交。

### H-16：schema mismatch quarantine 后没有 replay 入口

- **文件**：src/sync/schema/QuarantineStore.cpp:42
- **规格条款**：SQLite 同步设计 §5.10、plan T5.2/T5.4
- **问题描述**：`QuarantineStore::drainReady()` 已实现（src/sync/schema/QuarantineStore.cpp:42-68），但全仓没有调用点；schema mismatch 时只写 quarantine（src/sync/SyncWorker.cpp:533-539），不会在 schema 版本更新后重放。
- **影响**：迁移后重放和“旧版片迁移后到达/隔离后收口”没有闭环，quarantine 变成终态丢弃区。
- **修复建议**：在 schema guard 更新、本地 baseline 完成或周期扫描时调用 `drainReady()`，按原 artifact 语义重新进入 ledger/apply 流程。

## Medium 级问题

### M-08：OutboxWriter 在 `.ready`/目录 fsync 失败后留下 orphan payload

- **文件**：src/sync/transport/OutboxWriter.cpp:84
- **规格条款**：SQLite 同步设计 §5.11，FR-4
- **问题描述**：M-06 的 `dirFd < 0` 返回错误已修复（src/sync/transport/OutboxWriter.cpp:112-119）。但主 payload 已 rename 到 finalPath 后，`.ready` open/flush/fsync 失败直接返回（src/sync/transport/OutboxWriter.cpp:84-102），目录 fsync 失败也直接返回（src/sync/transport/OutboxWriter.cpp:121-129），未删除 final payload 和 ready 残留。
- **影响**：重试时同名 `QFile::rename(tmpPath, finalPath)` 可能失败或覆盖语义不确定；第三方搬运若误扫 payload 而非 ready，会看到 orphan。
- **修复建议**：`.ready` 或目录 fsync 失败时清理 final payload 与 ready；或文件名强制 UUID 每次重试并增加 orphan 清理器。

### M-09：ACK codec 未携带 `toPeer`，文件名成为唯一路由来源

- **文件**：src/sync/payload/PayloadCodec.cpp:344
- **规格条款**：SQLite 同步设计 §5.11 typed ACK
- **问题描述**：`ChangesetAck`/`PushChunkAck` 结构有 `toPeer`（include/dbridge/sync/SyncTypes.h:115、124），但编码时不写入 `toPeer`（src/sync/payload/PayloadCodec.cpp:344-360），解码也不恢复。接收端只能从文件名解析 fromPeer（src/sync/SyncWorker.cpp:728-743）。
- **影响**：ACK 制品内容无法自描述，文件名损坏/转发重命名时无法校验目标；typed ACK 的 schema 不完整。
- **修复建议**：ACK payload 写入 from/to peer，并在 `processAckArtifact()` 校验 `toPeer == config.nodeId()`。

### M-10：`SyncEngine::stop()` 停 worker 后不清 context 函数，存活 ComparisonSession 会调用失效写队列

- **文件**：src/sync/SyncEngine.cpp:176
- **规格条款**：SQLite 同步设计 §4.3、§5.8
- **问题描述**：析构会清空 `ctx_->workerWriteFn/rescanFn`（src/sync/SyncEngine.cpp:17-20），但 `stop()` 只 requestStop/wait 并设置状态（src/sync/SyncEngine.cpp:176-190）。若 ComparisonSession 仍持有 `shared_ptr<SyncContext>`，`save()` 会看到 `workerWriteFn` 仍存在并调用一个已停止 worker（src/sync/diff/ComparisonSession.cpp:348-362）。
- **影响**：场景2 stop 后 save/discard 行为不确定，可能超时或返回 `SyncWorker not ready`，但 gate/rescan 状态无法保证。
- **修复建议**：`stop()` 成功后同步清空 `importFn/workerWriteFn/rescanFn`，并让存活 session 明确进入不可保存状态。

### M-11：SQL 标识符 quoting 仍大量手写，合法特殊表名会失败

- **文件**：src/sync/selection/SelectionResolver.cpp:14
- **规格条款**：SQLite 同步设计 SQL 安全约束、§4.4/§5.5
- **问题描述**：SyncSelection Builder 已拒绝非 simple table（include/dbridge/sync/SyncSelection.h:49-63），但执行层仍手写 quote：`SelectionResolver` PRAGMA/SELECT（src/sync/selection/SelectionResolver.cpp:14、54）、`FkClosureBuilder` PRAGMA/SELECT（src/sync/selection/FkClosureBuilder.cpp:135、159）、`SchemaIntrospector` PRAGMA（src/schema/SchemaIntrospector.cpp:358、408、428、440）、`BaselineManager` DELETE/INSERT（src/sync/baseline/BaselineManager.cpp:127、155、159）、`SchemaGuard` PRAGMA（src/sync/schema/SchemaGuard.cpp:46）。
- **影响**：配置表名来自真实 schema 时，包含双引号或非 simple 但 SQLite 合法的表名会失败；部分手写位置还不转义列名。
- **修复建议**：统一使用 `SqlBuilder::quoteIdent()` 或专门 PRAGMA quote helper；API 层 simple ident 校验不能替代内部 schema 名称 quote。

## Low 级问题

### L-04：`ConflictArbiter` 仍未成为实际裁决入口

- **文件**：src/sync/SyncWorker.cpp:83
- **规格条款**：SQLite 同步设计 §5.6，plan T2.2
- **问题描述**：worker 构造并配置 `ConflictArbiter`（src/sync/SyncWorker.cpp:83、316），但实际胜负判断仍写在 `ChangesetApplier::conflictCb()`（src/sync/apply/ChangesetApplier.cpp:191-199）和 `RowWinnerStore::beats()`（src/sync/apply/RowWinnerStore.cpp:124）。
- **影响**：策略点分散，后续 TargetWins/Manual/Authoritative 下行策略容易分叉。
- **修复建议**：把 rank/seq 比较收敛进 `ConflictArbiter`，RowWinnerStore 只负责持久化。

### L-05：DeadPeerEvictor 缺少代际推进和 outbox 坍缩闭环

- **文件**：src/sync/peer/DeadPeerEvictor.cpp:46
- **规格条款**：SQLite 同步设计 FR-10，plan T5.3
- **问题描述**：逐出只设置 `pending_baseline` 并清 ACK（src/sync/peer/DeadPeerEvictor.cpp:46-49），广播会跳过 evicted peer（src/sync/SyncWorker.cpp:815-817），但没有 stream epoch 代际推进、baseline 制品生成或请求。
- **影响**：peer 进入 pending baseline 后没有恢复路径。
- **修复建议**：补 Healthy/Lagging/Evicted 状态机、代际推进、baseline request/response，以及 outbox 坍缩策略。

## 第八轮修复验证

| 项 | 结论 | 证据 |
|---|---|---|
| C-08 RowWinner 列名映射 | △部分修复 | INSERT/UPDATE 分支确实调用 `PRAGMA table_info`（src/sync/apply/ChangesetApplier.cpp:301-310），JSON key 使用列名（src/sync/apply/ChangesetApplier.cpp:321-345），DELETE 恢复 exec 有检查（src/sync/apply/ChangesetApplier.cpp:278）；但失败只 clear winner、不回滚、不上报（src/sync/apply/ChangesetApplier.cpp:278-281）。 |
| C-09 Baseline fallback | △部分修复 | 不再本地 export+apply，改为发 `E_SYNC_GAP` 并复制到 quarantineDir（src/sync/SyncWorker.cpp:1013-1034）；ledger 标 consumed（src/sync/SyncWorker.cpp:1033-1034）。但没有源端 baseline 协议，FR-8 未闭环。 |
| C-10 ComparisonSession factory | △部分修复 | `IComparisonSession::initialize()` 已是纯虚（include/dbridge/sync/IComparisonSession.h:56），OwningComparisonSession 已转发（src/sync/diff/ComparisonSession.cpp:456-459）；factory 返回可 initialize 的对象（src/sync/diff/ComparisonSession.cpp:474-508）。但 streamEpoch=0 导致 `tableDiffs()` 表态读取失真（src/sync/diff/ComparisonSession.cpp:498-506）。 |
| H-08 多 peer 命名冲突 | ✓已修复 | `changesetArtifactName` 有 `targetPeer` 参数（src/sync/SyncDDL.h:164-175），`broadcastTopeer()` 传 peer（src/sync/SyncWorker.cpp:890-892）；`selectionPushArtifactName` 也有 targetPeer（src/sync/SyncDDL.h:178-187），调用时传 centerPeer（src/sync/SyncWorker.cpp:1277-1281）。 |
| H-09 discard rescan | ✓已修复 | `ComparisonSession::discard()` 在 `releaseAll()` 后调用 `context_->rescanFn()`（src/sync/diff/ComparisonSession.cpp:376-381）。 |
| H-10 ExportService reverse lookup quoting | △部分修复 | `buildHColSelectSuffix()` quote 表/列（src/service/ExportService.cpp:171-172），prefetch SELECT/FROM/WHERE/IN 均 quote（src/service/ExportService.cpp:276-296）。但 H 列没有 alias，字段名匹配仍不稳（src/service/ExportService.cpp:376-383）。 |
| H-11 SyncSelection 表名校验 | ✓已修复 | `addRecord/addRecords` 调 `isSimpleIdent`（include/dbridge/sync/SyncSelection.h:49-63），`build()` 对 invalid table 返回错误（include/dbridge/sync/SyncSelection.h:83-92）。 |
| M-06 OutboxWriter 目录 open 失败 | ✓已修复 | `dirFd < 0` 返回 false 并填错误（src/sync/transport/OutboxWriter.cpp:112-119）。 |
| M-07 ACK 进入 ledger | ✓已修复 | `.ack` 分支先查 ledger，`markSeen`，成功后 `markConsumed`（src/sync/SyncWorker.cpp:451-459）。 |

## 其他逐项核对结论

- RowWinner 的 `PRAGMA table_info` 在 apply 后的 `wconn` 上读取（src/sync/apply/ChangesetApplier.cpp:301-310）。当前同步不传播 DDL，schema guard 在 apply 前校验（src/sync/apply/CapturedWriteTemplate.cpp:96-102），时序上不构成本轮新增 Critical；但仍应避免 fallback `_col_N` 静默写入。
- C-09 quarantine 后 ledger 是 consumed（src/sync/SyncWorker.cpp:1033-1034），下次 `stalePending()` 不会再触发该 artifact。
- 场景2 `InboundTableGate` 已通过 `SyncContext` 共享给 worker：engine 创建 worker 时传 `ctx_->inboundTableGate`（src/sync/SyncEngine.cpp:62），factory 也取同一个 gate（src/sync/diff/ComparisonSession.cpp:477-505）。
- InboxWatcher 只扫描 `*.ready`（src/sync/transport/InboxWatcher.cpp:98-117），ACK 由 `OutboxWriter::writeAck()` 走 `writeAtomic()`，因此 ACK 也需要并获得 `.ready` 哨兵（src/sync/transport/OutboxWriter.cpp:20-22、80-105）。
- AckChannel flush 失败项会保留到 pending 队列（src/sync/transport/AckChannel.cpp:30-58）。
- SyncConfig 的 `centerNodeId()`、peer lag getter 和 Builder setter 均存在（include/dbridge/sync/SyncConfig.h:19、49-65、159-216）。
- ImportService lookup 的 `selectColNames` 使用 G 表列名 `mp.first/sp.first`（src/service/ImportService.cpp:284-288），符合 lookup prefetch SQL 语义；FkInjector 旧 overload 已明确报错（src/mapping/FkInjector.cpp:10-18），ImportService 调用 RouteSpec overload（src/service/ImportService.cpp:525-526）。
- `E_SYNC_ACK_TIMEOUT` 有触发点（src/sync/SyncWorker.cpp:373-380）；`E_SYNC_REBASE_FAILED` 有触发点并跳过发送（src/sync/SyncWorker.cpp:872-876）；`E_SYNC_BASELINE_FAILED` 只在 `BaselineManager` 内部导出/应用失败时包装（src/sync/baseline/BaselineManager.cpp:190-194、209-224），但外部缺 baseline payload 调用链。
- 接收端 branchBC 的 rowErrors 会映射到 `E_SYNC_APPLY_FK` 或 `E_SYNC_APPLY_CONSTRAINT` 并回滚（src/sync/apply/CapturedWriteTemplate.cpp:288-300）。

## 测试覆盖

- 按用户指定命令 `ctest --test-dir build -N`：**Total Tests: 0**。
- 在 `build/tests` 目录直接运行 `ctest -N` 可发现 **17 个测试**，对应 `tests/CMakeLists.txt:17-33`，主要覆盖 profile/ETL/lookup/export/import。
- 未发现 FR-1 崩溃零窗口、FR-6 到达序无关、G-05 严格连续、场景2暂停闸的可执行同步核心测试。现有反向 lookup 测试存在（tests/unit/tst_reverse_lookup_export.cpp:159 起），但不覆盖本轮 H 列 alias 问题的特殊字段名/多表同名场景。

## 优先修复清单

| 优先级 | ID | 标题 | 文件 | 工作量 |
|---|---|---|---|---|
| P0 | C-11 | RowWinner 低 rank DELETE 过滤仍是占位 | src/sync/apply/ChangesetApplier.cpp | L |
| P0 | C-12 | RowWinner 恢复失败仍会提交并 ACK | src/sync/apply/ChangesetApplier.cpp | M |
| P0 | C-13 | 缺口 baseline 没有源端协议闭环 | include/dbridge/sync/SyncTypes.h | XL |
| P1 | H-13 | ComparisonSession factory streamEpoch=0 导致 tableDiffs 失真 | src/sync/diff/ComparisonSession.cpp | M |
| P1 | H-14 | ExportService H 列缺 alias 破坏 reverse lookup | src/service/ExportService.cpp | S |
| P1 | H-15 | selection push 分片未严格连续 | src/sync/SyncWorker.cpp | L |
| P1 | H-16 | quarantine 无重放入口 | src/sync/schema/QuarantineStore.cpp | M |
| P2 | M-08 | OutboxWriter ready/fsync 失败留下 orphan payload | src/sync/transport/OutboxWriter.cpp | S |
| P2 | M-09 | ACK payload 不自描述 toPeer | src/sync/payload/PayloadCodec.cpp | S |
| P2 | M-10 | stop 后 context 函数未清空 | src/sync/SyncEngine.cpp | S |
| P3 | M-11 | SQL 标识符 quote 仍分散手写 | src/sync/selection/SelectionResolver.cpp | M |

## 总结

第八轮的“明显漏接线”多数已修，代码成熟度有提升；但同步核心仍不能以当前实现宣称满足 v0.5：RowWinner 的 DELETE 保护仍会在失败时提交坏终态，baseline 仍无源端权威收口，selection push 未实现分片连续门控，场景2 factory 的表级判等使用错误 epoch。下一轮应优先补同步核心测试夹具，否则这些问题很难靠静态评审收敛。
