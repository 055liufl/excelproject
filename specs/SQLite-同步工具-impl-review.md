# SQLite 同步工具实现评审报告（第七轮）

> 评审日期：2026-06-25
> 评审范围：src/ 全量代码（修复第六轮全部剩余问题后）
> 规格基准：设计文档 v0.5 + 实现计划 v0.5 + openspec 5 项规格

## 执行摘要

总体符合度：**62/100**。较第六轮有实质进展，特别是导入嵌套事务、严格 gap pending、ACK 路由字段、SessionRecorder 空 changeset、部分标识符 quoting、columnOrder/reverse lookup/time-format 的主体链路已补上。但同步核心仍存在多个规格级断点：低 rank DELETE 仍可真实删除高 rank 胜者行；场景2暂停闸没有接入 SyncWorker；ComparisonSession::save() 仍绕过唯一写线程；baseline/死端逐出没有在主同步链路中触发；table_state stale 只被记录不被上报；测试在当前构建目录下 ctest 无法发现。

维度评分：同步一致性 50/100；传输与 ACK 70/100；场景2 35/100；ETL OpenSpec 78/100；SQL 安全 65/100；测试可验证性 30/100。

与上轮对比：修复生效率约 **13/18**。其中 **C-05、C-07（部分）、C-08、C-09、H-01、H-02、H-03、H-04、H-06、H-07、M-01/M-06、M-02、M-05、L-01（发送端）**有可见修复；**C-06、C-07（单写者）、H-05、M-04、L-01（接收端）**仍不完整。

## Critical 级问题

### C-01：低 rank DELETE 仍可删除高 rank 胜者行
- **文件**：src/sync/apply/ChangesetApplier.cpp:97、src/sync/apply/ChangesetApplier.cpp:242、src/sync/apply/ChangesetApplier.cpp:288
- **规格条款**：设计文档 §5.6 / §6.1 / FR-6 / G-01
- **问题描述**：`apply()` 声称通过 `filterByWinner()` 预过滤低 rank rows，但 `filterByWinner()` 最终无条件返回原始 changeset。低 rank DELETE 在 SQLite clean apply 路径不会触发 conflict callback，仍会落地删除当前高 rank 行。`updateWinnersFromChangeset()` 对 DELETE 只调用 `winners.clear()`，不会恢复已删除数据，且注释里的 “manual revert below” 并不存在。
- **修复建议**：不要在无法重建单行 changeset 的情况下声称过滤。可选方案：1）在 apply 前扫描 DELETE 行，对被低 rank 支配的 row 直接拒绝整个 changeset 或走反向补偿；2）扩展 `__sync_row_winner` 保存完整 winning row image，并在低 rank DELETE 后同事务恢复；3）使用 sqlite3session API 生成可应用的过滤 changeset。修复后必须补低 rank DELETE 后到、高 rank INSERT/UPDATE 已胜出的到达序无关测试。

### C-02：场景2 InboundTableGate 没有接入入站处理
- **文件**：src/sync/diff/ComparisonSession.cpp:64、src/sync/diff/ComparisonSession.cpp:351、src/sync/SyncWorker.cpp:343
- **规格条款**：设计文档 §5.8 / §7.6 / FR-12~FR-14
- **问题描述**：`ComparisonSession::initialize()` 打开的是 factory 私有的 `OwnedDeps::gate`，不是 SyncWorker 共享 gate；`SyncWorker::processArtifact()` 没有调用 `InboundTableGate::shouldDefer()`，也没有 payload table 提取与 pending defer 队列。因此场景2比对期间，被比对表的 inbox changeset 仍会照常应用，暂停闸规格未生效。
- **修复建议**：把 `InboundTableGate` 放入 `SyncContext` 或 SyncWorker 所有的共享对象；ComparisonSession factory 从同一 context 获取 gate；`processArtifact()` decode 后提取 payload tables，命中时保持 ledger=seen 并跳过 apply，release 后重新扫描。

### C-03：ComparisonSession::save() 仍破坏单写者保证
- **文件**：src/sync/diff/ComparisonSession.cpp:208、src/sync/diff/ComparisonSession.cpp:236、src/sync/diff/ComparisonSession.cpp:433
- **规格条款**：设计文档 §5.9 / §8 / NFR-6
- **问题描述**：第六轮修复为 factory 新开 `dbridge_cs_rw_*` 写连接，并用 `WriteTxn` 包装 save。但它仍不走 SyncWorker 队列。SQLite WAL 允许多读单写，不允许并发多写；当 SyncWorker 正在 apply/import/broadcast 维护元表时，ComparisonSession 可在另一个线程发起 `BEGIN IMMEDIATE` 写业务表，违反“后台 SyncWorker 唯一写线程”。
- **修复建议**：ComparisonSession 保存必须提交到 SyncWorker 的 write queue，复用 CapturedWriteTemplate/UpsertExecutor 并同事务维护 changelog/table_state。若暂不支持，应禁用 save 或返回明确的 `E_SYNC_STAGE_STALE/E_BUSY`，不能使用第二写连接。

### C-04：场景2 stale 检测是 stub，save 前不能发现本地变更
- **文件**：src/sync/diff/ComparisonSession.cpp:49、src/sync/diff/ComparisonSession.cpp:271
- **规格条款**：设计文档 §5.8 / §7.6 / FR-14
- **问题描述**：初始化只把 `pinnedDataVersion_` 设为 1，`checkStale()` 直接返回 true，且 `save()` 没调用 stale 检查。规格要求 read snapshot 后若 `data_version` 变动，stage 作废并报 `E_SYNC_STAGE_STALE`。
- **修复建议**：初始化时读取 `PRAGMA data_version` 或等价快照版本，save 前重新比较；不一致时释放 gate、丢弃 staging、返回 `E_SYNC_STAGE_STALE`。

## High 级问题

### H-01：baseline fallback 没有接入缺口/迁移主链路
- **文件**：src/sync/SyncWorker.cpp:331、src/sync/baseline/BaselineManager.cpp:268、src/sync/baseline/BaselineManager.cpp:186
- **规格条款**：设计文档 §5.10 / §6.2 / FR-8 / G-09
- **问题描述**：gap 超时只 emit `E_SYNC_GAP`，没有调用 `BaselineManager::exportBaseline/applyBaseline`。`shouldFallbackToBaseline()` 仅判断 `appliedSeq < sourceMinSeq`，没有覆盖缺锚点、迁移后、强制 baseline。`E_SYNC_BASELINE_FAILED` 只在 BaselineManager 内部包裹错误，主链路没有触发点。
- **修复建议**：为 gap stale、缺锚点、schema migration 后状态建立 baseline 任务状态机；失败时向 SyncEngine 上报 `E_SYNC_BASELINE_FAILED` 并保持旧锚点。

### H-02：DeadPeerEvictor 未接入 worker，死端逐出规格未实现
- **文件**：src/sync/peer/DeadPeerEvictor.cpp:15、src/sync/peer/DeadPeerEvictor.cpp:42、src/sync/SyncWorker.cpp:681
- **规格条款**：设计文档 §3.2 / FR-10
- **问题描述**：`DeadPeerEvictor` 有阈值评估和 `setPendingBaseline()`，但全仓只有定义，没有 SyncWorker 周期调用，也没有基于 lagSeq/lagBytes/lastAckMs 计算 peer state。截断水位仍无法排除死对端。
- **修复建议**：在 broadcast/ack 维护后周期计算每个 peer 的三维 lag，达到 hard 阈值时同事务标记 evicted/pending_baseline，并从 changelog 截断水位计算中排除。

### H-03：table_state 失败只记录在 WriteResult，调用方完全忽略
- **文件**：src/sync/apply/CapturedWriteTemplate.cpp:133、src/sync/apply/CapturedWriteTemplate.cpp:365、src/sync/SyncWorker.cpp:439
- **规格条款**：设计文档 §6.2 / §5.8 / M-04
- **问题描述**：`WriteResult::tableStateStaleSince` 已存在并在失败时赋值，但 `SyncWorker::processChangesetArtifact()` 和 `processSelectionPushArtifact()` 未检查该字段，也没有发 `W_SYNC_TABLE_STATE_STALE` 或标记 DiffEngine 禁用快速判等。结果 table_state 损坏时场景2可能继续给出假绿。
- **修复建议**：所有 `tpl_->execute()` 调用后检查 `tableStateStaleSince`，写审计日志/警告，并让 DiffEngine 对该表强制 baseline rebuild 或拒绝使用缓存。

### H-04：SQL 标识符 quoting 覆盖不完整，lookup/reverse lookup 仍裸拼表列名
- **文件**：src/service/ImportService.cpp:305、src/service/ExportService.cpp:274、src/service/ExportService.cpp:164、src/sql/SqlBuilder.cpp:84
- **规格条款**：openspec row-lookup / export-reverse-lookup；设计文档 §13.2 WHERE 注入；H-05
- **问题描述**：`SqlBuilder::quoteIdent()` 已实现，但 Import lookup prefetch 与 Export reverse prefetch 用 `selectColNames.join()`、`lk.fromTable`、`lk.match/select` 直接拼 SQL。`buildHColSelectSuffix()` 也拼 `route.table + "." + sp.second`，`buildAutoJoinSelect()` JOIN 仍手写 `"\"%1\""`，没有 escape 内嵌双引号。ProfileLoader 对 lookup 的 `from/match/select` 未做 `isSimpleIdentifier()` 校验。
- **修复建议**：统一使用 `SqlBuilder::quoteIdent()` 或共享 helper；loader 对 lookup 所有 DB identifier 做简单标识符校验，或允许任意 SQLite identifier 但必须全量 quote+escape。补恶意表名/列名、保留字、带双引号名称测试。

### H-05：接收端 push_progress 仍是 INSERT OR IGNORE，重发不能刷新状态
- **文件**：src/sync/SyncWorker.cpp:486
- **规格条款**：设计文档 §6.1 / L-01
- **问题描述**：发送端 `enqueueSelectionPush()` 已改 `ON CONFLICT DO UPDATE`，但接收端 `processSelectionPushArtifact()` 仍使用 `INSERT OR IGNORE` 创建 `__sync_push_progress`。同一 push 重发、schema 信息变化或失败后重试时，接收端不会刷新 `total_chunks/status/updated_ms`。
- **修复建议**：接收端同样改为 `INSERT ... ON CONFLICT(push_id) DO UPDATE SET status='streaming', total_chunks=excluded.total_chunks, schema_ver=excluded.schema_ver, updated_ms=excluded.updated_ms`，但 failed/done 状态的覆盖规则需明确。

### H-06：BatchTransfer gate 释放未持有 ctx，SyncEngine 析构可使互斥提前失效
- **文件**：src/batch/BatchTransfer.cpp:63、src/batch/BatchTransfer.cpp:78、src/batch/BatchTransfer.cpp:82
- **规格条款**：设计文档 §4.3 / §5.9 / NFR-6
- **问题描述**：startImport/startExport 获取 gate 后只捕获 `hasGate`，后台完成时重新 `getExisting(bridge_.dbPath())` 释放。如果 SyncEngine 在批任务期间析构并 release registry，原 gate 对象可销毁；之后新的 SyncEngine 可创建新 context 并开始前台写，批任务仍在运行，互斥失效。
- **修复建议**：lambda 捕获 `std::shared_ptr<SyncContext> ctx` 与 canonical dbPath，后台完成直接 `ctx->gate.release()`；同时避免跨线程访问 `bridge_.dbPath()`。

### H-07：传输制品命名未遵循稳定契约
- **文件**：src/sync/SyncWorker.cpp:755、src/sync/SyncWorker.cpp:1022、src/sync/SyncDDL.h:161
- **规格条款**：设计文档 §5.11 / FR-4 / G-08
- **问题描述**：DDL helper 定义了 `<origin>__<epoch>__<seq>__changeset.payload` 和 selection push 命名，但实际 broadcast 使用 `peer_localSeq.payload`，selection push 使用 `%node_sel_%push_%chunk.payload`。这破坏与第三方搬运器之间的稳定契约，也削弱按 origin/epoch/seq 人工排障能力。
- **修复建议**：所有 outbox 写入统一调用 `ddl::changesetArtifactName()` / `ddl::selectionPushArtifactName()`，必要时把目标 peer 放入目录/routeTag，而不是改文件名语义。

## Medium 级问题

### M-01：ConflictArbiter 未参与实际 apply 裁决
- **文件**：src/sync/conflict/ConflictArbiter.cpp:13、src/sync/SyncWorker.cpp:233、src/sync/apply/ChangesetApplier.cpp:186
- **规格条款**：设计文档 §5.6 / §10
- **问题描述**：`ConflictArbiter` 只提供 `beats()`，SyncWorker 仅 setRankMap，实际 `ChangesetApplier::conflictCb()` 复制了一份 rank/seq 比较逻辑。策略扩展点没有接入，ConflictPolicy 也未参与。
- **修复建议**：把 arbiter 注入 ChangesetApplier，统一冲突策略；或删除未用扩展点，避免设计承诺与实现分叉。

### M-02：Rebase 失败只按 Error 上报，未区分临时/永久
- **文件**：src/sync/SyncWorker.cpp:733、src/sync/conflict/RebaseEngine.cpp:17、src/sync/conflict/RebaseEngine.cpp:27
- **规格条款**：设计文档 §7.4 / G-09
- **问题描述**：`RebaseEngine` 已真实调用 `sqlite3rebaser_*`，但错误统一返回字符串，SyncWorker 统一 emit `E_SYNC_REBASE_FAILED` 后跳过发送，未区分输入损坏、schema 不兼容、暂时缺 rebase buffer 等可重试/永久失败。
- **修复建议**：为 rebase 返回结构化错误类别；永久失败隔离 payload，临时失败保留待重试，并避免无界跳过导致 peer 永远收不到该 local_seq。

### M-03：ComparisonSession::fetchRemoteRows 不是 keyset pagination
- **文件**：src/sync/diff/ComparisonSession.cpp:163、src/sync/diff/ComparisonSession.cpp:172、src/sync/diff/ComparisonSession.cpp:177
- **规格条款**：设计文档 §5.8 / IComparisonSession
- **问题描述**：接口名要求 keyset page token，但实现把 token 当数组 offset；返回值也没有 next token。远端 rows 顺序变化或大表分页时会重复/漏行。
- **修复建议**：token 应编码最后一个稳定 PK，查询/遍历按 PK 排序并从 `pk > lastPk` 开始；接口若缺 next token，需要扩展返回结构或约定调用方用最后一行 PK。

### M-04：Baseline apply 清空 RowWinner，未按基线权威态重建逐行胜者
- **文件**：src/sync/baseline/BaselineManager.cpp:244
- **规格条款**：设计文档 §5.10 / §6.1
- **问题描述**：规格要求 baseline 后以基线 origin/rank 重置逐行胜者。当前只 `rw.resetAll()`，后续低 rank changeset 可把基线行当作无 incumbent 处理。
- **修复建议**：baseline artifact 应携带或推导每行 pk/content hash，并在同事务插入 `__sync_row_winner`，rank/origin 使用基线来源。

### M-05：UpsertExecutor quote helper 不 escape 双引号
- **文件**：src/sync/apply/UpsertExecutor.cpp:97
- **规格条款**：H-05 / SQL 安全
- **问题描述**：`quote()` 只是 `"` + s + `"`，未把内嵌 `"` 转义为 `""`。同步 selection push 的 row/table/column 来自 payload，理论上应按 schema allow-list 限制，但当前没有统一 helper。
- **修复建议**：复用 `SqlBuilder::quoteIdent()` 或在 sync 层建同等实现，并在 SelectionPush 解码后校验 table/column 属于 `canonicalSyncTables_` 和实际 schema。

### M-06：ctest 当前构建目录发现 0 个测试
- **文件**：CMakeLists.txt:127、tests/CMakeLists.txt:17
- **规格条款**：测试覆盖 / 第四步 B8
- **问题描述**：源码 CMakeLists 有 `add_test()`，但当前工作区执行 `ctest -N` 和 `ctest --test-dir build -N` 均显示 `Total Tests: 0`。评审无法通过 ctest 验证任何修复。
- **修复建议**：重新配置 build with `BUILD_TESTING=ON`，确保 `build/tests/CTestTestfile.cmake` 注册用例；CI 必须跑同步关键路径测试。

## Low / Info

### L-01：canonicalSyncTables_ 没有回写 SyncConfig 快照
- **文件**：src/sync/SyncEngine.cpp:54、src/sync/SyncWorker.cpp:128、include/dbridge/sync/SyncConfig.h:28
- **规格条款**：C-08 / 设计文档 §4.4
- **问题描述**：Worker 内部已使用 `canonicalSyncTables_` 做 eligibility、fingerprint、session attach 和 selection push，但 `ctx_->config` 仍保存原始空 `syncTables()`。后续如果 ComparisonSession 或外部诊断从 context.config 读同步表，仍会看到空集合。
- **修复建议**：把 canonical 表集合放入 SyncContext 独立字段，或构造规范化 SyncConfig 后再分发给所有模块。

### L-02：ACK timeout 检测粒度受 broadcastIntervalMs 影响
- **文件**：src/sync/SyncWorker.cpp:255、src/sync/SyncWorker.cpp:273
- **规格条款**：设计文档 §5.11 / E_SYNC_ACK_TIMEOUT
- **问题描述**：timeout 检查只在主循环醒来后执行，最大延迟接近 `broadcastIntervalMs`。若 `ackMaxDelayMs` 小于 broadcast interval，错误上报会晚于配置。
- **修复建议**：等待时间取 `min(broadcastIntervalMs, ackDeadlineMs-now)`，或使用 worker 内 timer/condition wake。

## 第六轮修复验证

| ID | 结论 | 验证 |
| --- | --- | --- |
| C-05 同步导入嵌套事务 | ✓已修复 | `SyncWorker::submitImportSync()` 在 src/sync/SyncWorker.cpp:835 调用 `ImportService::run(..., false)`；`ImportService::run()` 在 src/service/ImportService.h:18 接受参数，并在 src/service/ImportService.cpp:572 仅 `manageTransaction` 为 true 时开事务。 |
| C-06 RowWinner 低 rank DELETE 覆盖高 rank | △部分修复 | `RowWinnerStore::clear()` 已实现于 src/sync/apply/RowWinnerStore.cpp:88；DELETE 路径会检测并 clear 于 src/sync/apply/ChangesetApplier.cpp:242。但低 rank DELETE 已经落地，未恢复数据，`filterByWinner()` 仍返回原 blob。 |
| C-07 ComparisonSession save 写线程 | △部分修复 | factory 新开写连接于 src/sync/diff/ComparisonSession.cpp:433，save 用 `WriteTxn` 于 src/sync/diff/ComparisonSession.cpp:236；但仍不走 SyncWorker 队列，单写者问题仍在。 |
| C-08 空 syncTables 展开 | ✓已修复 | `SchemaEligibility::expandSyncTables()` 在 src/sync/schema/SchemaEligibility.cpp:14 实现；worker 在 src/sync/SyncWorker.cpp:128 展开并用 canonical 列表做 verify/fingerprint/session attach。 |
| C-09 SelectionPush 行失败 rollback | ✓已修复 | branchBC 在 src/sync/apply/CapturedWriteTemplate.cpp:290 检查 `rowErrors` 并 rollback。 |
| H-01 ChangelogStore INSERT OR IGNORE | ✓已修复 | `insertRow()` 使用普通 INSERT 于 src/sync/capture/ChangelogStore.cpp:138。 |
| H-02 stop() gate 释放 | ✓已修复 | `SyncEngine::stop()` 在 src/sync/SyncEngine.cpp:170 调用 `releaseGateIfTerminal(Stopped)`。 |
| H-03 gap 过早报错 | ✓已修复 | branchA gap 返回 `GAP_PENDING` 于 src/sync/apply/CapturedWriteTemplate.cpp:81；processArtifact 保持 ledger seen 于 src/sync/SyncWorker.cpp:441。 |
| H-04 PushChunkAck toPeer | ✓已修复 | `PushChunkAck::toPeer` 在 include/dbridge/sync/SyncTypes.h:124；AckChannel flush 用 toPeer 于 src/sync/transport/AckChannel.cpp:47。 |
| H-05 SQL 标识符 quoteIdent | △部分修复 | `SqlBuilder::quoteIdent()` 在 src/sql/SqlBuilder.cpp:8；buildUpsert 已用。Import/Export lookup 与 reverse lookup 仍裸拼，见 H-04。 |
| H-06 SessionRecorder 区分 | ✓已修复 | `collectChangeset()` 无变更返回非 null 空数组于 src/sync/capture/SessionRecorder.cpp:108；`sealInto()` 用 `isNull()` 于 src/sync/capture/SessionRecorder.cpp:60。 |
| H-07 push_progress status=streaming | ✓已修复 | 发送端 src/sync/SyncWorker.cpp:991、接收端 src/sync/SyncWorker.cpp:490 均写 streaming。 |
| M-01/M-06 BatchTransfer gate | △部分修复 | startImport/startExport 通过 registry 获取 gate 于 src/batch/BatchTransfer.cpp:63、117；但后台未捕获 ctx，存在生命周期互斥漏洞。 |
| M-02 目录 fsync | ✓已修复 | OutboxWriter 检查目录 fsync 返回值于 src/sync/transport/OutboxWriter.cpp:114。 |
| M-04 table_state 失败 | △部分修复 | 字段存在并赋值于 src/sync/apply/CapturedWriteTemplate.h:57、src/sync/apply/CapturedWriteTemplate.cpp:134；调用方未上报/处理。 |
| M-05 BaselineManager INSERT OR REPLACE | ✓已修复 | baseline apply 用普通 INSERT 于 src/sync/baseline/BaselineManager.cpp:159。 |
| L-01 push_progress ON CONFLICT DO UPDATE | △部分修复 | 发送端已 DO UPDATE 于 src/sync/SyncWorker.cpp:988；接收端仍 INSERT OR IGNORE 于 src/sync/SyncWorker.cpp:488。 |

## 优先修复清单

| 优先级 | ID | 标题 | 文件 | 工作量 |
| --- | --- | --- | --- | --- |
| P0 | C-01 | 低 rank DELETE 仍可删除高 rank 胜者行 | src/sync/apply/ChangesetApplier.cpp | L |
| P0 | C-02 | InboundTableGate 未接入 SyncWorker | src/sync/SyncWorker.cpp / src/sync/diff/ComparisonSession.cpp | M |
| P0 | C-03 | ComparisonSession::save 破坏单写者 | src/sync/diff/ComparisonSession.cpp | L |
| P1 | H-01 | baseline fallback 未接入主链路 | src/sync/SyncWorker.cpp / src/sync/baseline/BaselineManager.cpp | L |
| P1 | H-02 | DeadPeerEvictor 未接入 | src/sync/peer/DeadPeerEvictor.cpp / src/sync/SyncWorker.cpp | M |
| P1 | H-04 | lookup/reverse lookup 标识符裸拼 | src/service/ImportService.cpp / src/service/ExportService.cpp | M |
| P1 | H-03 | table_state stale 未上报 | src/sync/SyncWorker.cpp | S |
| P2 | M-06 | ctest 发现 0 测试 | CMakeLists.txt / build 配置 | S |

## 总结

第七轮不能判定同步引擎达到 v0.5 规格。第六轮多个局部修复已经落地，但关键一致性问题仍集中在“同事务落库后的真实收敛”和“唯一写线程”两条主线上。下一轮应先修 C-01/C-02/C-03，并补对应崩溃零窗口、FR-6 到达序无关、G-05 严格连续、场景2暂停闸测试；否则继续修外围功能也无法证明同步核心正确。
