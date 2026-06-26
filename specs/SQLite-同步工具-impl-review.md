# SQLite 同步工具实现评审报告（第八轮）

> 评审日期：2026-06-25  
> 评审范围：src/ 全量代码（修复第七轮全部问题后）  
> 规格基准：设计文档 v0.5 + 实现计划 v0.5 + openspec 5 项规格

## 执行摘要

总体符合度：**63/100**。较第七轮有实质前进：`InboundTableGate` 已接入 worker、`ComparisonSession::save()` 已走 worker 写队列、`PRAGMA data_version` stale 检测已落地、`push_progress`/`UpsertExecutor`/`ImportService lookup quote` 等点修复真实存在。

但仍不能视为规格达成：逐行胜者的低 rank DELETE 恢复仍是失效实现；缺口 baseline fallback 只是本地自导自入，不是源端权威基线；场景2公开 factory 无法初始化远端数据；多 peer 制品命名会冲突；同步核心缺少可执行测试夹具。

维度评分：同步核心 52/100，场景2 48/100，传输可靠性 66/100，ETL openspec 82/100，SQL 安全 68/100，测试覆盖 35/100。

## Critical 级问题

### C-08：RowWinner 低 rank DELETE 恢复仍会失败，且失败被吞掉
- **文件**：src/sync/apply/ChangesetApplier.cpp:291
- **规格条款**：设计文档 §5.6、plan T1.7b、FR-6/G-01
- **问题描述**：`winning_content` 已在 DDL 存在（src/sync/SyncDDL.h:138），INSERT/UPDATE 也会保存 JSON（src/sync/apply/ChangesetApplier.cpp:291）。但 JSON key 是列索引字符串 `"0"`, `"1"`（src/sync/apply/ChangesetApplier.cpp:299），恢复时直接当列名拼成 `"0"`, `"1"`（src/sync/apply/ChangesetApplier.cpp:261），没有通过 `PRAGMA table_info` 映射真实列名。低 rank DELETE 成功删除高 rank 当前行后，恢复 SQL 会写不存在的列名，并且 `restoreQ.exec()` 的返回值被忽略（src/sync/apply/ChangesetApplier.cpp:268）。同时 `filterByWinner()` 明确返回原始 changeset（src/sync/apply/ChangesetApplier.cpp:406），无法预过滤 DELETE。
- **修复建议**：`winning_content` 存储应使用真实列名到值的对象，或同时保存 `nCol` 对应的列名数组；DELETE 恢复前用 `sqlite3changeset_op()` 的表名 + `PRAGMA table_info` 建立列序映射。恢复必须使用 `SqlBuilder::quoteIdent()`，检查 `exec()` 失败并让整个 apply 事务回滚。

### C-09：Baseline fallback 没有获取源端权威基线，缺口无法按 FR-8 收口
- **文件**：src/sync/SyncWorker.cpp:1001
- **规格条款**：设计文档 §5.10、§14 FR-8，plan T1.7/T5.1
- **问题描述**：缺口 pending 超时后确实调用 `runBaselineFallbackFor()`（src/sync/SyncWorker.cpp:430），但该函数读取 pending changeset 后，在本地 `wconn` 上 `exportBaseline()`（src/sync/SyncWorker.cpp:1001），再把同一份本地 baseline `applyBaseline()` 回本地（src/sync/SyncWorker.cpp:1017）。这没有向 `dec.header.origin` 请求或消费源端 baseline，也没有更新到源端 `originSeq` 之后的权威切面。
- **修复建议**：将缺口超时转成 baseline request/response 制品，源端导出 baseline；接收端只应用源端 baseline artifact。应用后锚点应设置为源端 baseline 的 `sourceMaxSeq`，并把原 pending gap 制品转为 consumed 或 quarantine。

### C-10：场景2公开 factory 创建的是未初始化会话，公开接口无法注入 remote meta/rows
- **文件**：src/sync/diff/ComparisonSession.cpp:446
- **规格条款**：设计文档 §4.5、§5.8、§7.6，plan T4.4
- **问题描述**：`ComparisonSession::initialize()` 需要 `tables/remoteMetas/remoteRows` 才会计算 diff 并 `gate_.open()`（src/sync/diff/ComparisonSession.cpp:39）。但公开 `createComparisonSession(const SyncConfig&)` 只接收 config（include/dbridge/sync/IComparisonSession.h:56），factory 直接返回未初始化对象（src/sync/diff/ComparisonSession.cpp:474）。`IComparisonSession` 接口也没有 `initialize()` 方法（include/dbridge/sync/IComparisonSession.h:41），调用方拿到会话后无法进入可用状态。
- **修复建议**：公开 factory 参数应包含远端快照描述，或在 `IComparisonSession` 增加初始化方法；factory 内必须完成 `initialize()`，失败返回 nullptr。否则 `tableDiffs()` 永远是空结果，InboundTableGate 也不会打开。

## High 级问题

### H-08：多 peer 广播制品命名会冲突
- **文件**：src/sync/SyncDDL.h:162
- **规格条款**：设计文档 §5.11，plan T0.6/T1.5a
- **问题描述**：设计要求命名包含 `<origin>__<stream_epoch>__<kind>__<seq|push_id.chunk_seq>__<uuid>`。当前 helper 只生成 `%1__%2__%3__changeset.payload`（src/sync/SyncDDL.h:162），selection push 也无 UUID（src/sync/SyncDDL.h:169）。`broadcast()` 对每个 peer 调用 `broadcastTopeer()`（src/sync/SyncWorker.cpp:806），但同一 changelog entry 对所有 peer 生成同名文件（src/sync/SyncWorker.cpp:883）。共享 outbox 下第二个 peer 会命名冲突，Qt `QFile::rename()` 失败时本轮广播中断（src/sync/transport/OutboxWriter.cpp:145）。
- **修复建议**：命名 helper 按规格加入 kind 固定位置、目标 peer 或 UUID；同一 entry 发往不同 peer 必须产生不同 artifact name，并在 payload header 中保留 routeTag。

### H-09：`ComparisonSession::discard()` 释放 gate 后未触发 rescan
- **文件**：src/sync/diff/ComparisonSession.cpp:278
- **规格条款**：设计文档 §5.8，plan T4.2
- **问题描述**：`save()` 的空提交、stale、成功路径释放 gate 后都会调用 `context_->rescanFn()`（src/sync/diff/ComparisonSession.cpp:216、224、272）。但 `discard()` 只有 `staging_.discard(); gate_.releaseAll();`（src/sync/diff/ComparisonSession.cpp:278），不会立即重扫 pending `seen` 制品。
- **修复建议**：`discard()` 与 `save()` 对齐，`releaseAll()` 后调用 `context_->rescanFn()`。

### H-10：ExportService reverse lookup 仍裸拼 SQL 标识符
- **文件**：src/service/ExportService.cpp:274
- **规格条款**：openspec/export-reverse-lookup，设计文档 SQL 安全约束
- **问题描述**：reverse prefetch SQL 直接拼 `selectColNames`、`lk.fromTable`、`lk.select[0].first`、`sp.first`（src/service/ExportService.cpp:274、281、288）。H 列扩展也直接拼 `route.table + "." + sp.second`（src/service/ExportService.cpp:164）。相同问题在 ImportService 已用 `SqlBuilder::quoteIdent()` 修复（src/service/ImportService.cpp:305），导出侧未对齐。
- **修复建议**：所有表名/列名走 `SqlBuilder::quoteIdent()`；`table.column` 拆分后分别 quote；为 H 列追加稳定 alias，避免字段名碰撞。

### H-11：SyncSelection 表名未校验，选择路径可构造注入式标识符
- **文件**：include/dbridge/sync/SyncSelection.h:49
- **规格条款**：设计文档 §4.4、§5.5
- **问题描述**：`Builder::addRecord()` 直接接受任意 table 字符串（include/dbridge/sync/SyncSelection.h:49）。`SelectionResolver` 随后用 `PRAGMA table_info("%1")` 和 `SELECT * FROM "%1"` 手工包引号但不转义（src/sync/selection/SelectionResolver.cpp:14、54）；`FkClosureBuilder` 同样如此（src/sync/selection/FkClosureBuilder.cpp:135、159）。
- **修复建议**：Builder 层拒绝非 simple identifier，或所有执行点统一使用 `SqlBuilder::quoteIdent()`。推荐两者都做：API 输入校验 + SQL builder quote。

### H-12：ConflictArbiter 未参与实际冲突裁决
- **文件**：src/sync/SyncWorker.cpp:83
- **规格条款**：设计文档 §5.6，plan T2.2
- **问题描述**：`ConflictArbiter` 仅构造并设置 rank map（src/sync/SyncWorker.cpp:83、316）。全仓只有这些引用；实际胜负判断写在 `ChangesetApplier::conflictCb()`（src/sync/apply/ChangesetApplier.cpp:191）和 `RowWinnerStore::beats()`（src/sync/apply/RowWinnerStore.cpp:124）。这使策略点失效，也让 `(rank,seq)` 规范序分散重复。
- **修复建议**：把 winner 比较收敛到 `ConflictArbiter` 或让 `RowWinnerStore` 依赖同一策略对象；广播/rebase 前后的权威裁决也应调用该策略。

## Medium 级问题

### M-06：OutboxWriter 忽略目录 open 失败，且 ready 失败后留下不可重试 orphan payload
- **文件**：src/sync/transport/OutboxWriter.cpp:183
- **规格条款**：设计文档 §5.11，FR-4
- **问题描述**：目录 fsync 前 `::open(dir)` 若失败，代码直接跳过并返回成功（src/sync/transport/OutboxWriter.cpp:183）。另外 `.ready` 写/flush/fsync 失败时只删除 `.ready`，已经 rename 成功的主 payload 留在目录中（src/sync/transport/OutboxWriter.cpp:152），同名重试可能在 `rename()` 处失败。
- **修复建议**：目录 open 失败应返回 `E_SYNC_TRANSPORT`；ready 失败应清理 final payload，或采用新 UUID 名称重试并记录 orphan 清理策略。

### M-07：ACK 制品未纳入 inbox ledger，ready 文件会被反复扫描
- **文件**：src/sync/SyncWorker.cpp:451
- **规格条款**：设计文档 §5.11 typed ACK / inbox ledger
- **问题描述**：`processArtifact()` 在 `.ack` 文件上直接 `return processAckArtifact()`（src/sync/SyncWorker.cpp:451），没有 `markSeen/markConsumed`。`InboxWatcher` 会持续看到同一个 `.ack.ready` 并重复处理，ACK 更新虽大体幂等，但会造成无界重复扫描和重复完成事件风险。
- **修复建议**：ACK artifact 也进入 `__sync_inbox_ledger`，成功处理后标 consumed；或处理成功后原子移走 `.ack` 与 `.ready`。

### M-08：legacy time-format null 字段未按规格字段级拒绝
- **文件**：src/profile/ProfileLoader.cpp:214
- **规格条款**：openspec/time-format “Format and fallback empty value definition”
- **问题描述**：side-object 的 `format:null`/`fallback:null` 会被拒绝（src/profile/ProfileLoader.cpp:114、124），但 legacy `excelFormat`/`dbFormat` 直接 `toString()`（src/profile/ProfileLoader.cpp:218、267）。显式 JSON null 会被当空字符串进入后续校验，而不是以“字段必须是 string, not null”报错。
- **修复建议**：legacy 字段读取前显式检查 `isNull()` 和类型；错误消息包含字段名。

### M-09：DeadPeerEvictor 只有阈值判断，缺少规格要求的代际/坍缩闭环
- **文件**：src/sync/peer/DeadPeerEvictor.cpp:19
- **规格条款**：plan T5.3，设计文档 FR-10/C8
- **问题描述**：三维阈值判断已实现（src/sync/peer/DeadPeerEvictor.cpp:27），逐出会设置 `pending_baseline` 并清 ack（src/sync/peer/DeadPeerEvictor.cpp:46）。但没有 `streamEpoch` 代际推进，也没有 outbox 坍缩/基线制品生成闭环；soft 告警也无状态抑制，会周期性重复报。
- **修复建议**：补 peer 状态机：Healthy→Lagging→Evicted；逐出时推进代际、停止增量排队、生成或请求 baseline，并对 soft 告警做边沿触发。

## Low 级问题

### L-03：多处 schema/baseline PRAGMA 仍手写 quote，不支持非常规表名
- **文件**：src/schema/SchemaIntrospector.cpp:26
- **规格条款**：设计文档 §5.1/SQL 安全约束
- **问题描述**：`PRAGMA table_xinfo('%1')`、`PRAGMA index_list('%1')`、baseline `DELETE FROM "%1"`/`INSERT INTO "%1"` 等仍手写 quote（src/schema/SchemaIntrospector.cpp:26、76；src/sync/baseline/BaselineManager.cpp:127、159）。同步 eligibility 多数输入来自真实库表名，遇到包含引号的合法 SQLite 标识符会失败。
- **修复建议**：增加 SQLite PRAGMA 标识符 helper，或统一复用 `SqlBuilder::quoteIdent()`。

## 第七轮修复验证

| 项 | 结论 | 证据 |
|---|---|---|
| C-01 RowWinner 低 rank DELETE 恢复 | △部分修复 | `winning_content` DDL 已有（src/sync/SyncDDL.h:138），INSERT/UPDATE 存 JSON（src/sync/apply/ChangesetApplier.cpp:291），DELETE 有恢复分支（src/sync/apply/ChangesetApplier.cpp:253）；但列索引未映射列名且失败被吞（src/sync/apply/ChangesetApplier.cpp:261、275）。 |
| C-02 InboundTableGate 接入 SyncWorker | △部分修复 | `payloadTables()` + `shouldDefer()` 已接入（src/sync/SyncWorker.cpp:37、488），命中不 markConsumed/不 ACK；`save()` release 后 rescan（src/sync/diff/ComparisonSession.cpp:271），但 `discard()` 未 rescan（src/sync/diff/ComparisonSession.cpp:278）。 |
| C-03 ComparisonSession::save 入队 | ✓已修复 | 无 worker 返回错误（src/sync/diff/ComparisonSession.cpp:250），有 worker 通过 `workerWriteFn` 入队（src/sync/diff/ComparisonSession.cpp:259）。 |
| C-04 stale 检测读 data_version | ✓已修复 | initialize 读取（src/sync/diff/ComparisonSession.cpp:53），save 调 `checkStale()`（src/sync/diff/ComparisonSession.cpp:222），比较并返回 `E_SYNC_STAGE_STALE` 文本（src/sync/diff/ComparisonSession.cpp:287）。 |
| H-01 baseline fallback 接入链路 | △部分修复 | gap 超时触发（src/sync/SyncWorker.cpp:430），但 fallback 是本地自导自入（src/sync/SyncWorker.cpp:1001）。 |
| H-02 DeadPeerEvictor 接入 worker | △部分修复 | broadcast 后调用（src/sync/SyncWorker.cpp:824），ACK 后调用（src/sync/SyncWorker.cpp:745、800），广播排除 evicted peer（src/sync/SyncWorker.cpp:808）；但代际/坍缩闭环缺失。 |
| H-03 tableStateStaleSince 上报 | ✓已修复 | changeset 和 selection push 都上报 warning（src/sync/SyncWorker.cpp:557、694）。 |
| H-04 lookup 标识符 quoting | ✓已修复 | ImportService lookup prefetch 使用 `SqlBuilder::quoteIdent()`（src/service/ImportService.cpp:305）。 |
| H-05 接收端 push_progress ON CONFLICT | ✓已修复 | `processSelectionPushArtifact()` 使用 `ON CONFLICT(push_id) DO UPDATE`（src/sync/SyncWorker.cpp:603）。 |
| H-06 BatchTransfer gate 生命周期 | ✓已修复 | import/export lambda 捕获 `shared_ptr<SyncContext>`（src/batch/BatchTransfer.cpp:60、110）。 |
| H-07 制品命名 helper 接入 | △部分修复 | 调用了 DDL helper（src/sync/SyncWorker.cpp:883、1266），但 helper 本身缺 UUID/peer 维度（src/sync/SyncDDL.h:162）。 |
| M-05 UpsertExecutor quoteIdent | ✓已修复 | `buildUpsertSql()` 使用 `SqlBuilder::quoteIdent()`（src/sync/apply/UpsertExecutor.cpp:220）。 |
| L-01 canonicalSyncTables 写入 SyncContext | ✓已修复 | worker 初始化后写入 ctx（src/sync/SyncWorker.cpp:328）。 |
| L-02 ACK timeout 粒度 | ✓已修复 | 主循环等待按 `ackDeadlineMs_` 缩短（src/sync/SyncWorker.cpp:347），超时发 `E_SYNC_ACK_TIMEOUT`（src/sync/SyncWorker.cpp:373）。 |

## 其他符合度结论

- `RebaseEngine::rebase()` 已调用真实 `sqlite3rebaser_create/configure/rebase` API（src/sync/conflict/RebaseEngine.cpp:110）。
- `syncSelected()` 已串起 SelectionResolver→FkClosureBuilder→ChunkStreamer→OutboxWriter（src/sync/SyncWorker.cpp:1176、1194、1212、1247）。
- `ISyncEngine` 8+1 方法均有实现（src/sync/SyncEngine.cpp:33、143、176、193、198、203、208、213、218）。
- reverse lookup、columnOrder、per-column time-format 主体功能已实现并有测试，但 reverse lookup SQL quote 未达标。
- `ctest -N` 在仓库根目录发现 0 个测试；在 `build/` 下发现 17 个测试，主要覆盖 profile/ETL。未发现 FR-1、FR-6、G-05、场景2、gap-baseline 的同步核心可执行夹具。

## 优先修复清单

| 优先级 | ID | 标题 | 文件 | 工作量 |
|---|---|---|---|---|
| P0 | C-08 | RowWinner DELETE 恢复列映射失效 | src/sync/apply/ChangesetApplier.cpp | M |
| P0 | C-09 | baseline fallback 本地自导自入 | src/sync/SyncWorker.cpp | L |
| P0 | C-10 | ComparisonSession factory 不可初始化 | src/sync/diff/ComparisonSession.cpp | M |
| P1 | H-08 | 多 peer 制品命名冲突 | src/sync/SyncDDL.h | M |
| P1 | H-09 | discard 释放 gate 后不 rescan | src/sync/diff/ComparisonSession.cpp | S |
| P1 | H-10 | ExportService reverse lookup 裸拼 SQL | src/service/ExportService.cpp | M |
| P1 | H-11 | SyncSelection table 未校验 | include/dbridge/sync/SyncSelection.h | S |
| P2 | H-12 | ConflictArbiter 未接入裁决 | src/sync/conflict/ConflictArbiter.cpp | M |
| P2 | M-06 | OutboxWriter 目录 fsync/open/orphan 处理不完整 | src/sync/transport/OutboxWriter.cpp | S |
| P2 | M-07 | ACK 未进 ledger | src/sync/SyncWorker.cpp | S |

## 总结

第七轮的大多数“接线类”修复已经落地，但仍有三个阻塞交付的问题：RowWinner 删除恢复不正确、baseline fallback 语义错误、场景2公开会话无法初始化。同步核心目前还缺少能证明 FR-1/FR-6/G-05/gap-baseline 的测试夹具；在这些夹具补齐前，不能只依赖代码审查确认收敛性。
