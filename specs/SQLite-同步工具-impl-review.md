# SQLite 同步工具实现评审报告（第五轮）

> 评审日期：2026-06-25
> 评审范围：src/ 全量代码（修复第四轮全部 Critical+High+Medium 后）
> 规格基准：设计文档 v0.5 + 实现计划 v0.5 + openspec 5 项规格

## 执行摘要

总体符合度：48/100。较第四轮（42/100）有进步，主要体现在错误码补齐、`xlsxPath`、`syncActive_` atomic、`addWhere()` 前置拒绝、ACK 文件名 UUID、xFilter、部分 ETL OpenSpec 能力等。但同步引擎仍未达到 v0.5 的最小可运行闭环：`syncSelected()` 仍不可用，SelectionPush ACK/progress 不闭合，前台门控不会释放，分支 C 本地捕获使用固定 `origin_seq=0`，RowWinner 过滤仍是占位，场景2保存仍绕过 SyncWorker。

维度评分：同步核心 32/100；Transport/ACK 45/100；选择推送 20/100；场景2 25/100；ETL/OpenSpec 82/100；并发/事务 55/100；安全 60/100；测试可证明性 35/100。

## Critical 级问题（阻断核心功能）

### C-01：`syncSelected()` 在调用线程阶段即因空 profileName 失败
- **文件**：src/sync/SyncEngine.cpp:202，src/DataBridge.cpp:288
- **规格条款**：设计文档 §4.2 / §5.5 / §7.3；实现计划 T2.13
- **问题描述**：`SyncEngine::syncSelected()` 为了取得 `SchemaCatalog` 调用 `snapshotProfileCatalog(QString(), nullptr, &catalog, ...)`，但 `snapshotProfileCatalog()` 无条件查找 profile，空 `profileName` 会触发 “Profile not loaded: ” 并返回 false。选择推送无法进入 `SelectionResolver/FkClosureBuilder/ChunkStreamer` 链路。
- **修复建议**：拆出只快照 catalog 的线程安全方法，或允许 `profile == nullptr` 时跳过 profile 查找。`syncSelected()` 的受理前校验应只校验 selection 与同步状态，不依赖 ETL profile。

### C-02：SelectionPush 发送/接收没有 typed `PushChunkAck` 闭环
- **文件**：src/sync/SyncWorker.cpp:369，src/sync/SyncWorker.cpp:537，src/sync/SyncWorker.cpp:880，src/sync/apply/CapturedWriteTemplate.cpp:288
- **规格条款**：设计文档 §5.4 typed ACK / §5.5 / §7.3；实现计划 T2.9/T2.13；Q-04/C-05
- **问题描述**：`processArtifact()` 对所有成功制品都发送 `ChangesetAck`，SelectionPush 成功后没有 `schedulePushChunkAck()`。发送端 `enqueueSelectionPush()` 也没有创建本地 `__sync_push_progress`，因此 `processAckArtifact()` 即使收到 `PushChunkAck`，向 `__sync_push_chunk_progress` 插入也会因外键缺父行失败（返回值被忽略）。同时分支 B 写入 chunk progress 的 checksum 固定为空，无法校验重复 chunk “同 checksum no-op / 异 checksum corruption”。
- **修复建议**：SelectionPush 成功应用后发送 `PushChunkAck{push_id, chunk_seq, total_chunks, checksum, ok}`；发送端在写出首片前创建 `push_progress(status='sending')`；ACK 处理检查 SQL 返回值，所有片 ACK 后置 `done` 并推进前台 `Completed`。

### C-03：前台 `ForegroundGate` 成功或失败后都不会释放
- **文件**：src/sync/SyncEngine.cpp:248，src/sync/SyncEngine.cpp:253，src/sync/ForegroundGate.h:23
- **规格条款**：设计文档 §5.9 / §7.1 / FR-11；实现计划 T1.12/T3.3
- **问题描述**：`sync()`/`syncSelected()` 获取 gate 后，`onWorkerProgress()` 只复制进度，`onWorkerError()` 明确“不释放 gate”。代码中除 `syncSelected()` 快照失败外没有释放路径。第一次前台同步结束或超时后，后续前台操作永久 `E_BUSY`。
- **修复建议**：在 `Completed/Failed/Stopped` 终态统一释放 gate；最好由 `SyncEngine` 拥有 gate 生命周期，worker 只发状态/错误信号，避免双释放。

### C-04：本地写捕获使用固定 `origin_seq=0`，第二次本地写起被 changelog 忽略
- **文件**：src/sync/SyncWorker.cpp:768，src/sync/capture/SessionRecorder.cpp:77，src/sync/SyncDDL.h:28，src/sync/capture/ChangelogStore.cpp:136
- **规格条款**：设计文档 §5.4 分支 C / §6.1 `__sync_changelog`；实现计划 T1.6/T3.1
- **问题描述**：同步导入分支手工调用 `sealInto(... originSeq=0 ...)`，`__sync_changelog` 对 `(origin, stream_epoch, origin_seq)` 有唯一约束，`ChangelogStore` 又使用 `INSERT OR IGNORE`。同一节点第二次本地写仍是 `origin_seq=0`，会被静默忽略，后续广播丢增量。
- **修复建议**：为本地 origin 维护单调 `origin_seq`（持久化或从 changelog max 恢复），每次分支 C seal 前分配新 seq；`INSERT OR IGNORE` 对 changelog 不应吞掉不变量冲突，必须返回错误。

### C-05：同步导入仍存在嵌套事务，且没有真正走分支 C/UpsertExecutor
- **文件**：src/sync/SyncWorker.cpp:743，src/sync/SyncWorker.cpp:762，src/service/ImportService.cpp:573
- **规格条款**：设计文档 §2.4 / §5.4 / E-01；实现计划 T3.1；第四轮 C-03
- **问题描述**：`submitImportSync()` 外层先 `WriteTxn BEGIN IMMEDIATE` 并开启 session，随后调用 `ImportService::run(..., manageTransaction=true)`，内部再执行 `db.transaction()`。这仍违反单事务模板，也没有通过 `CapturedWriteTemplate::execute(LocalWrite)` 和 `UpsertExecutor` 统一维护 table_state。
- **修复建议**：调用 `ImportService::run(..., manageTransaction=false)` 只能作为短期止血；长期应抽出 `RoutePayload -> RowMutation`，由 `CapturedWriteTemplate` 分支 C 持事务、fresh capture、UpsertExecutor、table_state、seal。

### C-06：RowWinner 低优先级过滤仍是占位，无法证明到达序无关
- **文件**：src/sync/apply/ChangesetApplier.cpp:94，src/sync/apply/ChangesetApplier.cpp:319，src/sync/apply/ChangesetApplier.cpp:342
- **规格条款**：设计文档 §5.6 / G-01；实现计划 T1.7b/T2.2；第四轮 C-04
- **问题描述**：`filterByWinner()` 声称预过滤低 rank 行，但实际无法重建 filtered changeset，最终总是返回原始 changeset。若 SQLite 没触发 conflict callback，低优先级后到写仍可能落业务表；代码中的“manual revert below”并不存在。
- **修复建议**：实现可执行的行级预判/过滤，或 apply 后按 `__sync_row_winner.content_hash` 恢复胜者内容。必须补“高 rank 先到、低 rank 跨批后到” fixture。

### C-07：场景2 `ComparisonSession` 仍未初始化且保存使用只读连接
- **文件**：src/sync/diff/ComparisonSession.cpp:391，src/sync/diff/ComparisonSession.cpp:413，src/sync/diff/ComparisonSession.cpp:233
- **规格条款**：设计文档 §5.8 / §7.6 / D-16；实现计划 T4.3/T4.4；第四轮 C-07
- **问题描述**：工厂只构造对象，不调用 `initialize()`；`wconn` 仍传入同一个 read-only `rconn`，注释也承认 save 需要后续接线。`save()` 直接 `staging_.save(wconn_, ...)`，不入 SyncWorker，不经 CapturedWriteTemplate，不捕获 changelog。
- **修复建议**：工厂参数必须包含 watched tables/remote meta/remote rows 并完成初始化；`save()` 入 SyncWorker，经分支 C + UpsertExecutor 保存，提交后释放 InboundTableGate。

## High 级问题

### H-01：SelectionPush 出站 payload 缺 schema fingerprint，接收端会被 SchemaGuard 拒绝
- **文件**：src/sync/SyncWorker.cpp:882，src/sync/apply/CapturedWriteTemplate.cpp:180
- **规格条款**：设计文档 §5.3 / §5.4 / §5.5
- **问题描述**：`enqueueSelectionPush()` 设置了 `schemaVer`，但未填 `hdr.schemaFingerprint`。接收端分支 B 调 `guard_.verifyPayload(p.schemaVer, p.schemaFp, ...)`，空 fingerprint 与本地 fingerprint 不匹配时整片失败。
- **修复建议**：发送 selection push 时填入当前 `SchemaGuard::fingerprint()`；payload codec decode 后也应校验 header 必填字段，缺失直接 `E_SYNC_PAYLOAD_CORRUPT`。

### H-02：`sync()` 空 drain 也会等待 ACK 并超时
- **文件**：src/sync/SyncEngine.cpp:142，src/sync/SyncWorker.cpp:798，src/sync/SyncWorker.cpp:252
- **规格条款**：设计文档 §4.2 / §7.1
- **问题描述**：`sync()` 一律进入 `Exporting` 并 `startAckWait()`。若 drain 后没有任何 outbox payload，系统没有 ACK 会返回，最终 `E_SYNC_ACK_TIMEOUT`。手动 drain 的“无新变化”应完成，而不是失败。
- **修复建议**：`enqueueDrain()` 返回本轮是否写出需要 ACK 的制品；无制品时直接 `Completed` 并释放 gate。

### H-03：`SchemaEligibility::hasUpsertTarget()` 会误拒常见 `INTEGER PRIMARY KEY`
- **文件**：src/sync/schema/SchemaEligibility.cpp:153，src/sync/schema/SchemaEligibility.cpp:192
- **规格条款**：设计文档 §4.4 / G-04；实现计划 T1.0c
- **问题描述**：`introspect()` 已把单列 `INTEGER PRIMARY KEY` 视为隐式 NOT NULL，但 `hasUpsertTarget()` 又重新要求 PRAGMA `notnull=1`，SQLite 对 `INTEGER PRIMARY KEY` 常返回 0，导致合法同步表初始化失败。
- **修复建议**：复用 `introspect()` 的 PK not-null 判定；同时补充 partial/expression unique index 不可作为配置 conflict target 的测试。

### H-04：`SqlBuilder` 仍大量裸拼标识符，且多 fkInject group JOIN 会重复同一 child 表
- **文件**：src/sql/SqlBuilder.cpp:31，src/sql/SqlBuilder.cpp:60，src/sql/SqlBuilder.cpp:64，src/sql/SqlBuilder.cpp:90
- **规格条款**：安全性维度 D；fk-injection spec；export-reverse-lookup spec
- **问题描述**：UPSERT、SELECT、FROM、ORDER BY 仍拼接裸表名/列名。M-06 的“遍历全部 fkInject group”只做了一半：同一 child 有多个 parent group 时会生成多个 `LEFT JOIN "child"`，没有 alias，SQL 语义错误或歧义。
- **修复建议**：集中实现 `quoteIdent/quoteQualifiedIdent`，所有标识符必须来自 catalog/profile 白名单；多父 join 使用稳定 alias，或在当前能力边界内拒绝不支持的导出形态。

### H-05：ACK/transport 写失败仍被静默丢弃
- **文件**：src/sync/transport/AckChannel.cpp:31，src/sync/transport/AckChannel.cpp:40
- **规格条款**：设计文档 §5.11 / G-08 / E_SYNC_TRANSPORT
- **问题描述**：ACK 文件名已加入 UUID，但 `writer_.writeAck()` 的返回值仍被忽略。磁盘满、目录权限、rename 失败会导致 ACK 丢失，发送端只能超时。
- **修复建议**：`AckChannel::flush()` 返回失败列表或发出错误；失败 ACK 保留在 pending 队列并上报 `E_SYNC_TRANSPORT`。

### H-06：InboundTableGate 没接入 SyncWorker，场景2暂停闸无效
- **文件**：src/sync/SyncWorker.cpp:286，src/sync/diff/InboundTableGate.cpp:10
- **规格条款**：设计文档 §5.8 / §7.6；实现计划 T4.2
- **问题描述**：`InboundTableGate` 只被 `ComparisonSession` 本地对象使用，`SyncWorker::scanInbox()` 应用入站 payload 前从不预扫描 payload 涉及表，也不调用 `shouldDefer()`。会话期间入站变更不会 pending，不满足场景2隔离。
- **修复建议**：Gate 应归属 SyncContext/SyncWorker；apply 前解出 payload table set，命中 watched tables 时 ledger 保持 pending、不给 ACK，release 后按到达序重放。

### H-07：分支 A 错误码未映射到规格错误码
- **文件**：src/sync/apply/CapturedWriteTemplate.cpp:91，src/sync/apply/CapturedWriteTemplate.cpp:102，src/sync/SyncWorker.cpp:407
- **规格条款**：设计文档 §4.6 / §5.4；错误传播维度 E
- **问题描述**：schema mismatch 返回 `"SCHEMA_MISMATCH"`，apply 失败返回 `"APPLY_FAILED"`，没有区分 `E_SYNC_APPLY_FK`、`E_SYNC_APPLY_CONSTRAINT`、`E_SYNC_SCHEMA_MISMATCH`。上层 `errors()` 得到非 registry code。
- **修复建议**：`ChangesetApplier` 暴露原生 conflict/rc 分类；CapturedWriteTemplate 只返回 `Errors.h` 中定义的稳定码。

## Medium 级问题

### M-01：gap 超时只报错，不触发 baseline，且阈值硬编码
- **文件**：src/sync/SyncWorker.cpp:305，src/sync/transport/InboxLedger.cpp:93
- **规格条款**：设计文档 §6 / G-05；实现计划 S-01/M-01
- **问题描述**：`stalePending()` 已实现，但阈值固定 30 秒，不来自 `SyncConfig`；超时后只反复发 `E_SYNC_GAP`，没有进入 BaselineManager。
- **修复建议**：增加 config 化 gapTimeout；首次超时落一次错误并启动 baseline fallback，成功后清理 pending ledger。

### M-02：TableStateStore 增量只在部分路径可信
- **文件**：src/sync/apply/CapturedWriteTemplate.cpp:198，src/sync/SyncWorker.cpp:762，src/service/ImportService.cpp:586
- **规格条款**：设计文档 §6.2 / E-09
- **问题描述**：分支 B/C 的 beforeHash 预读已修，但同步导入当前直接走 `ImportService` 自己的 SQL 循环，不进入 `CapturedWriteTemplate::branchBC`，因此不会维护 `__sync_table_state`。
- **修复建议**：同步导入改成 RowMutation + CapturedWriteTemplate 分支 C；禁止同步模式下存在第二套写循环。

### M-03：`SelectionResolver::resolveWhere()` 仍保留原始 SQL 执行能力
- **文件**：include/dbridge/sync/SyncSelection.h:75，src/sync/selection/SelectionResolver.cpp:73
- **规格条款**：设计文档 §4.4 / 安全性维度 D；第四轮 H-01
- **问题描述**：Builder 已在 `build()` 中拒绝 raw `addWhere()`，但 `SyncSelection` 内部仍保留 whereClauses，resolver 仍直接拼接 `WHERE %2`。如果未来构造绕过 Builder 或调用方忽略 build 错误，危险路径仍在。
- **修复建议**：MVP 删除 `WhereClause` 执行路径，或改为结构化谓词 + bind。

### M-04：Outbox 原子发布没有检查 `fsync()` 返回值
- **文件**：src/sync/transport/OutboxWriter.cpp:58，src/sync/transport/OutboxWriter.cpp:90，src/sync/transport/OutboxWriter.cpp:101
- **规格条款**：设计文档 §5.11 / G-08
- **问题描述**：`fsync()` 返回值被忽略，目录 fsync 失败也当成功。写盘错误可能被误判为已发布。
- **修复建议**：检查文件和目录 fsync 返回值；失败时删除 tmp/ready，返回 `E_SYNC_TRANSPORT`。

### M-05：FK preflight SQL 仍裸拼表名和列名
- **文件**：src/validation/ForeignKeyPreflight.cpp:121
- **规格条款**：fk-injection spec；安全性维度 D
- **问题描述**：preflight 的 `SELECT 1 FROM ... WHERE col = ?` 只绑定值，表名/列名仍裸拼。虽然 profile loader 限制简单 identifier，但安全目标要求统一 quote 和白名单。
- **修复建议**：复用 SqlBuilder 的 identifier quote；表/列必须从 SchemaCatalog 验证后进入 SQL。

## Low / Info

### L-01：`FkInjector::inject(QVector<RoutePayload>&, QString*)` 仍是 no-op 公共接口
- **文件**：src/mapping/FkInjector.cpp:10
- **规格条款**：fk-injection spec；DRY/SRP
- **问题描述**：带 `RouteSpec` 的 overload 已有真实逻辑并被 `ImportService` 使用，但旧 overload 仍公开且返回 true，容易被误用。
- **修复建议**：删除旧 overload，或让其显式返回 false 并说明必须传 RouteSpec。

### L-02：当前 build 未注册测试
- **文件**：build/CTestTestfile.cmake:1
- **规格条款**：实现计划 §5 测试断言
- **问题描述**：执行 `ctest --test-dir build --output-on-failure` 返回 `No tests were found!!!`。源码下有大量 Qt 单测，但当前构建产物不能证明它们在 CI 中执行。
- **修复建议**：修复 CMake/qmake 测试注册；同步夹具应纳入可运行 CI。

## 第四轮修复验证

| Issue | 状态 | 结论 |
|---|---|---|
| C-01 syncSelected 空实现 | △ 部分修复 | 有 worker 链路代码，但被空 profile snapshot 阻断，且 SelectionPush schemaFp/ACK 缺失。 |
| C-02 sync() 不执行 drain | △ 部分修复 | 已有 `enqueueDrain()`；但无 payload 仍 ACK timeout，gate 不释放。 |
| C-03 同步导入跨线程/嵌套事务 | △ 部分修复 | profile/catalog 快照已修；嵌套事务与非分支 C 写入仍未修。 |
| C-04 RowWinner 不能阻止低 rank 后到 | ✗ 未修复 | `filterByWinner()` 仍返回原 changeset。 |
| C-05 SelectionPush ACK/progress 未闭合 | ✗ 未修复 | 有 ACK 处理片段，但接收端不发 PushChunkAck，发送端无 push_progress。 |
| C-06 IBatchTransfer 缺 xlsxPath | ✓ 已修复 | `ImportOptions/ExportOptions` 已含 `xlsxPath`。 |
| C-07 ComparisonSession 保存绕过写线程 | ✗ 未修复 | 工厂仍未初始化，save 仍用 read-only rconn。 |
| H-01 raw addWhere | △ 部分修复 | Builder 拒绝；resolver 原始 SQL 路径仍存在。 |
| H-02 广播水位混用 | ✓ 已修复 | 路由判据已用 `ackedSeq(peer, origin, epoch)`。 |
| H-03 ACK 文件名冲突 | △ 部分修复 | 文件名已有 UUID；ACK 写失败仍忽略。 |
| H-04 Changeset xFilter | ✓ 已修复 | apply_v2 已传 `filterCb`，拒 `__sync_*` 和非同步表。 |
| H-05 table_state beforeHash | △ 部分修复 | branchBC 预读已修；同步导入未走 branchBC。 |
| H-06 FkInjector no-op | △ 部分修复 | 新 overload 真实注入；旧 public overload 仍 no-op。 |
| H-07 fkInject alignment | ✓ 已修复 | child 已有不一致值时报 `E_VALIDATE_FK`。 |
| H-08 SqlBuilder quote | ✗ 未修复 | 多处裸拼标识符仍存在。 |
| H-09 worker error 不更新 Failed/result | △ 部分修复 | 状态/result 会更新；gate 不释放。 |
| H-10 temporal export 空字符串 | ✓ 已修复 | 非 NULL 空字符串进入 DB parser 并报 `E_TIME_PARSE_DB`。 |
| H-11 BatchTransfer 后台使用 db_ | ✓ 已修复 | 导入/导出后台均创建线程内连接并使用快照。 |
| M-01 gap 超时 | △ 部分修复 | `stalePending()` 已有；未 baseline，阈值硬编码。 |
| M-02 eligibility upsert target | △ 部分修复 | 不再永远 true，但误拒 `INTEGER PRIMARY KEY`。 |
| M-03 rebase flags/失败闭环 | △ 部分修复 | flags 和错误级别已改；无完整重试/回滚闭环。 |
| M-04 SyncContext 文件不存在 fallback | ✓ 已修复 | POSIX/Windows 均不再回退路径 key。 |
| M-05 SelectionPush push_progress | △ 部分修复 | 接收端首片创建；发送端 progress/ACK 闭环缺失。 |
| M-06 Export 多 fkInject group | △ 部分修复 | 遍历了 groups，但重复 join/alias/quote 仍不正确。 |
| L-01 syncActive_ bool | ✓ 已修复 | 已改 `std::atomic<bool>`。 |

## 优先修复清单

| 优先级 | Issue ID | 标题 | 文件 | 预估工作量 |
|---|---|---|---|---|
| P0 | C-04 | 本地写 `origin_seq=0` 导致 changelog 丢增量 | src/sync/SyncWorker.cpp | M |
| P0 | C-02 | SelectionPush typed ACK/progress 闭环缺失 | src/sync/SyncWorker.cpp | L |
| P0 | C-03 | ForegroundGate 不释放 | src/sync/SyncEngine.cpp | S |
| P0 | C-06 | RowWinner 过滤仍占位 | src/sync/apply/ChangesetApplier.cpp | XL |
| P0 | C-05 | 同步导入嵌套事务/未走分支 C | src/sync/SyncWorker.cpp | L |
| P1 | C-01 | syncSelected 空 profile snapshot 阻断 | src/sync/SyncEngine.cpp | S |
| P1 | C-07 | ComparisonSession 保存绕过写线程 | src/sync/diff/ComparisonSession.cpp | L |
| P1 | H-01 | SelectionPush 缺 schema fingerprint | src/sync/SyncWorker.cpp | S |
| P1 | H-03 | eligibility 误拒 INTEGER PRIMARY KEY | src/sync/schema/SchemaEligibility.cpp | S |
| P2 | H-04 | SqlBuilder 标识符 quote 与多 JOIN alias | src/sql/SqlBuilder.cpp | M |

## 测试覆盖缺口

- 同步核心仍缺可运行夹具：本地多次写 `origin_seq` 单调、changelog 唯一冲突不得吞、RowWinner 低 rank 跨批后到、SelectionPush 全片 ACK、foreground gate 终态释放、空 drain Completed。
- Transport 缺故障注入：ACK 写失败保留 pending、fsync/rename 失败、半截主文件 + `.ready`、pending gap 跨重启续判、gap baseline fallback。
- 场景2缺测试：InboundTableGate 与 SyncWorker 集成、save 经 CapturedWriteTemplate、`E_SYNC_STAGE_STALE`。
- 安全缺测试：SqlBuilder/ForeignKeyPreflight 对保留字/特殊标识符 quote；raw where 不可达。
- 构建层缺口：当前 `ctest --test-dir build --output-on-failure` 未发现测试，不能证明 `tests/unit` 已执行。

## 总结

第五轮后，ETL 侧多数 OpenSpec 能力已接近可验收，但同步引擎仍不能作为一致性系统交付。最应先修的是：本地 `origin_seq` 单调、SelectionPush typed ACK、gate 释放、RowWinner 真过滤、同步导入分支 C 事务边界。完成这些前，双向同步、选择推送和场景2都仍是高风险状态。
