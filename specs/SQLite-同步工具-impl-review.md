# SQLite 同步工具实现评审报告

> 评审日期：2026-06-25
> 评审范围：src/ 全量代码（122 文件 / ~13,630 行）
> 规格基准：设计文档 v0.5 + 实现计划 v0.5 + openspec 5 项规格

## 执行摘要

整体符合度评分：42/100。ETL/OpenSpec 侧约 72/100，已覆盖 `fkInject` 数组解析、row-lookup 预取、`columnOrder`、reverse lookup、三槽时间格式的大部分基础能力；同步引擎侧约 25/100，当前更接近“接口与 DDL 骨架 + 局部实现”，未达到 v0.5 对严格连续应用、全片 ACK、上行选择推送、单写线程边界、场景2保存、可恢复 gap/pending 的闭环要求。

最阻断的是同步核心：`syncSelected()` 空实现，`sync()` 不执行手动 drain，BatchTransfer/Import 改道存在 QtSql 跨线程与嵌套事务，RowWinner 元数据无法阻止低优先级 changeset 后到覆盖业务表。按当前状态不建议交付同步引擎；ETL 可继续作为 MVP 演进，但仍需收敛安全拼 SQL、模块边界和少数规格偏差。

## Critical 级问题（必须修复，阻断核心功能）

### C-01：`syncSelected()` 返回成功但没有实现选择推送
- **文件**：src/sync/SyncEngine.cpp:192
- **规格条款**：设计文档 §4.2 / §5.5 / FR-17 / G-02
- **问题描述**：方法只入队一个空 lambda，未调用 `SelectionResolver`、`FkClosureBuilder`、`FrozenManifest`、`ChunkStreamer`、`OutboxWriter`，也没有 `push_progress`、分片 ACK、完成状态。
- **修复建议**：实现完整 `validateSelection -> resolveSelectionPK -> buildFkClosure -> freezeManifest -> streamChunks -> typed ACK wait` 流程；未完成前返回错误，不允许假成功。

### C-02：`sync()` 不执行手动 drain，状态机会虚假进入 `Exporting`
- **文件**：src/sync/SyncEngine.cpp:120
- **规格条款**：设计文档 §4.2 / K：`sync()` 手动 drain、非阻塞
- **问题描述**：`sync()` 只设置 `Exporting`、启动 ACK deadline，然后入队释放 gate 的任务；没有显式 `scanInbox()` 或 `broadcast()`。如果没有后续周期广播或 ACK，调用方只会等超时。
- **修复建议**：给 `SyncWorker` 增加显式 `drainOnce()` 任务，任务内完成 inbox scan、pending 重扫、outbox 打包发布，再启动 ACK 等待并按 ACK/timeout 推进 `Completed/Failed`。

### C-03：同步导入改道违反单写线程契约并触发嵌套事务
- **文件**：src/sync/SyncWorker.cpp:654
- **规格条款**：设计文档 §2.4 / §2.5 / E-01 / IBatchTransfer L
- **问题描述**：`submitImportSync()` 外层已 `WriteTxn BEGIN IMMEDIATE`，随后调用 `ImportService::run()`，后者再执行 `db.transaction()`（src/service/ImportService.cpp:670）。同时 `DataBridge::runImportOnDb()` 在 worker 线程调用 `refreshCatalog()` 触碰主线程 `db_`（src/DataBridge.cpp:242）。
- **修复建议**：提取无事务 ImportService 写入核心，由 `CapturedWriteTemplate` 分支 C 持事务和 session；profile/catalog 在调用线程复制为值传入 worker，worker 不访问 `DataBridgePrivate::db_`。

### C-04：RowWinner 不能阻止低 rank changeset 后到覆盖业务表
- **文件**：src/sync/apply/ChangesetApplier.cpp:94
- **规格条款**：设计文档 §5.6 / G-01
- **问题描述**：`sqlite3changeset_apply_v2()` 先真实落库；若 SQLite 没触发 conflict callback，`updateWinnersFromChangeset()` 只是不更新 `__sync_row_winner`，无法撤销低 rank 后到写入。
- **修复建议**：apply 前逐行解析 changeset 并用 RowWinner 过滤低优先级行，或 apply 后按 winner 状态恢复胜者行；不能只更新元数据。

### C-05：SelectionPush 分片 ACK/progress 未闭合
- **文件**：src/sync/SyncWorker.cpp:536
- **规格条款**：设计文档 §5.4 / Q-04 / I
- **问题描述**：`PushChunkAck` decode 后直接 `return true`，没有记录 ACK、没有判断全片 ACK、没有把 `push_progress` 置为 `done`。分支 B 写入 `__sync_push_chunk_progress` 时 checksum 固定为空（src/sync/apply/CapturedWriteTemplate.cpp:224）。
- **修复建议**：为 chunk ACK 增加发送端状态表更新；按 `(push_id, chunk_seq, total_chunks, checksum, ok)` 校验，只有所有 chunk ok 才将 `push_progress=done` 并推进 foreground `Completed`。

### C-06：`IBatchTransfer` 无 xlsxPath 参数，实际把 `profileName` 当文件路径
- **文件**：src/batch/BatchTransfer.cpp:95
- **规格条款**：设计文档 §4.3 / IBatchTransfer 8+3
- **问题描述**：`startImport(const ImportOptions&)` 与 `startExport(const ExportOptions&)` 没有文件路径参数；实现约定 `opts.profileName` 是 xlsx 路径（src/batch/BatchTransfer.cpp:147、195），导致 profile 名和文件路径无法同时表达。
- **修复建议**：扩展 `ImportOptions/ExportOptions` 增加 `xlsxPath`，或修改接口签名显式传路径；`profileName` 只保留配置名语义。

### C-07：场景2 `ComparisonSession` 工厂返回未初始化、保存绕过写线程
- **文件**：src/sync/diff/ComparisonSession.cpp:391
- **规格条款**：设计文档 §5.8 / D-16 / E-01
- **问题描述**：工厂创建依赖后直接返回 session，未调用 `initialize()`；`wconn` 实际引用只读连接，`save()` 调 `staging_.save(wconn_, ...)` 不走 `WriteTxn`、不经 `SyncWorker`、不捕获 changelog。
- **修复建议**：工厂接入 SyncContext 和 worker；`save()` 作为写任务进入单写线程，经 `CapturedWriteTemplate` 分支 C 保存 staged rows，并释放 `InboundTableGate`。

## High 级问题（显著影响正确性或安全性）

### H-01：`addWhere()` 和 `SelectionResolver::resolveWhere()` 允许原始 SQL
- **文件**：include/dbridge/sync/SyncSelection.h:58
- **规格条款**：设计文档 §1 / 安全性维度 4
- **问题描述**：Builder 接受任意 `whereExpr`，`resolveWhere()` 直接拼入 SQL（src/sync/selection/SelectionResolver.cpp:84），违反“禁原始 WHERE 直通/参数绑定”。
- **修复建议**：删除原始 `addWhere`，改为结构化谓词 Builder（列、操作符、绑定值）；所有值走 `addBindValue()`，表/列名走白名单 quote。

### H-02：广播路由混用 `local_seq` 与 `origin_seq`
- **文件**：src/sync/SyncWorker.cpp:555
- **规格条款**：设计文档 §5.7 / §5.9
- **问题描述**：`afterLocalSeq` 来自 `lastSentLocalSeq()`，却传给 `RoutingTable::shouldRoute()` 当 `peerAckedSeq` 与 `originSeq` 比较（src/sync/SyncWorker.cpp:568）。两个序列空间不同，会漏发或误发。
- **修复建议**：扫描游标使用 `local_seq`，路由/裁剪水位使用 `(peer, origin, epoch).acked_seq`；成功 typed ACK 后再推进 ACK 水位。

### H-03：ACK 文件名同毫秒冲突会丢 ACK
- **文件**：src/sync/transport/AckChannel.cpp:24
- **规格条款**：设计文档 §5.11 / G-08 / typed ACK
- **问题描述**：同一 flush 内多个 ACK 使用 `ack__{from}__{to}__{ms}.ack`，同毫秒同目标会重名；`OutboxWriter::writeAtomic()` 目标存在时 rename 失败（src/sync/transport/OutboxWriter.cpp:67），调用方还忽略返回值。
- **修复建议**：ACK 文件名加入 UUID/递增序号/ackedSeq；`writeAck` 失败必须上报 `E_SYNC_TRANSPORT` 并保留 pending ACK。

### H-04：Changeset apply 未过滤同步表
- **文件**：src/sync/apply/ChangesetApplier.cpp:94
- **规格条款**：设计文档 §2.5 / §5.1 / 安全性维度 4
- **问题描述**：`sqlite3changeset_apply_v2()` 的 xFilter 为 `nullptr`，入站 changeset 可作用于任意表。
- **修复建议**：实现 xFilter，仅允许 `config.syncTables()`；非同步表或 meta 表变更应拒绝并 quarantine。

### H-05：TableStateStore 增量 checksum 在 UPSERT 路径中计算错误
- **文件**：src/sync/apply/CapturedWriteTemplate.cpp:240
- **规格条款**：设计文档 §6.2 / E-09 / H
- **问题描述**：代码在 `UpsertExecutor.apply()` 之后才判断行是否已存在（src/sync/apply/CapturedWriteTemplate.cpp:249），并读取“旧行”hash（src/sync/apply/CapturedWriteTemplate.cpp:286），实际读到的是新值，导致 insert/update delta 失真。
- **修复建议**：在 UPSERT 前读取 beforeHash 和存在性；同一事务内执行 `read old -> upsert -> apply checksum delta`。

### H-06：`FkInjector` 模块是 no-op，真实逻辑散落在 ImportService
- **文件**：src/mapping/FkInjector.cpp:7
- **规格条款**：fk-injection spec / 设计文档 §3.2 DRY
- **问题描述**：`FkInjector::inject()` 直接返回 true，实际注入在 `ImportService::doFkInject()` 内部（src/service/ImportService.cpp:86），模块接口与实现不一致，复用与测试边界失效。
- **修复建议**：把 `doFkInject()` 移入 `FkInjector`，显式传 `RouteSpec`/route index/错误收集器，ImportService 只编排。

### H-07：fkInject 遇到 child 已有不同值时直接覆盖，缺少 alignment 校验
- **文件**：src/service/ImportService.cpp:154
- **规格条款**：fk-injection spec / conflict-value alignment
- **问题描述**：当 child payload 已包含同名 child column，代码无条件 `binds[toIdx]=fkVal`，没有比较已有值是否与 parent 注入值一致。
- **修复建议**：若 child 已有非 NULL 值且与注入值不等，报 `E_VALIDATE_FK` 并标记该 route 失败；只有空/一致时写入。

### H-08：`SqlBuilder` 和导出 join SQL 未统一 quote 标识符
- **文件**：src/sql/SqlBuilder.cpp:31
- **规格条款**：安全性维度 4
- **问题描述**：UPSERT、SELECT、JOIN、ORDER BY 使用表名/列名字符串拼接（src/sql/SqlBuilder.cpp:60、70、94）。虽然 profile loader 限制简单 identifier，但与“SQL 全部参数绑定/拒绝原始 SQL”的安全目标不一致。
- **修复建议**：集中提供 `quoteIdent()` 和 qualified identifier Builder；所有表/列名必须来自 schema/catalog 白名单。

### H-09：`SyncEngine` 收到 worker 错误不更新 `Failed/result`
- **文件**：src/sync/SyncEngine.cpp:250
- **规格条款**：设计文档 §4.2 / 错误传播维度 6
- **问题描述**：`onWorkerError()` 只追加 errors/logs，不把 `progress_.state` 改为 `Failed`，也不写 `result_`。`E_SYNC_ACK_TIMEOUT` 触发后调用方可能仍看到 `Exporting`。
- **修复建议**：worker 错误应携带是否终止前台 operation；终止类错误同步设置 state/result，并释放 foreground gate。

### H-10：ExportService 时间导出把 DB 空字符串当 NULL，未报 `E_TIME_PARSE_DB`
- **文件**：src/service/ExportService.cpp:70
- **规格条款**：time-format spec / Export parses DB per db-side spec
- **问题描述**：`convertTemporalForExport()` 对非 NULL 但 `toString().trimmed().isEmpty()` 的 DB 值直接返回空 cell，不触发 DB-side parse error；规格只允许 SQL NULL 静默空。
- **修复建议**：导出方向只以 `QVariant::isNull()` 判 NULL；空字符串进入 db-side parser，失败时报 `E_TIME_PARSE_DB`，该 cell 写空但行继续。

### H-11：`BatchTransfer::runExport()` 后台线程直接使用主线程 `DataBridge::db_`
- **文件**：src/batch/BatchTransfer.cpp:195
- **规格条款**：设计文档 §2.4 / 并发维度 3
- **问题描述**：`QtConcurrent` 线程调用 `bridge_.exportExcel()`，该方法使用创建于主线程的 `QSqlDatabase db_`（src/DataBridge.cpp:282），违反 QtSql 线程亲和性。
- **修复建议**：BatchTransfer 后台任务创建本线程读连接，或将导出任务入专属 worker；禁止跨线程复用 `QSqlDatabase`。

## Medium 级问题（影响健壮性或性能）

### M-01：gap 缺口没有 pending 超时/基线回退策略
- **文件**：src/sync/apply/CapturedWriteTemplate.cpp:74
- **规格条款**：设计文档 §6 / G-05
- **问题描述**：AppliedVector 能识别 `Gap`，InboxLedger 也保持 `seen`，但每次重扫都会重复报错；没有 first_seen、gap timeout、baseline fallback。
- **修复建议**：ledger 增加 pending reason/first_seen；短期静默等待缺片，超阈值再发 `E_SYNC_GAP` 并触发 baseline。

### M-02：`SchemaEligibility::hasUpsertTarget()` 永远返回 true
- **文件**：src/sync/schema/SchemaEligibility.cpp:165
- **规格条款**：设计文档 §4.4 / G-04
- **问题描述**：当前只要有 PK 就认为有可用冲突目标；没有验证 WITHOUT ROWID、复合 PK notnull 顺序、partial unique 等边界。
- **修复建议**：基于 `PRAGMA table_info/index_list/index_xinfo` 确认 `ON CONFLICT(...)` 的实际目标可用，并为不合格表返回 `E_SYNC_UNSUPPORTED_SCHEMA`。

### M-03：Rebase 链路没有按 apply_v2 推荐 flags 和失败闭环
- **文件**：src/sync/apply/ChangesetApplier.cpp:94
- **规格条款**：设计文档 §5.6 / E-06 / G-03
- **问题描述**：`sqlite3changeset_apply_v2` flags 为 0，而 `RebaseEngine` 注释要求 `SQLITE_CHANGESETAPPLY_NOSAVEPOINT` 语义；rebase 失败只跳过广播并 emit warning（src/sync/SyncWorker.cpp:582），没有进入 result/error 终态。
- **修复建议**：统一 apply_v2/rebaser 调用契约；rebase 失败映射 `E_SYNC_REBASE_FAILED` 到前台/后台可观测结果，并保留重试。

### M-04：`SyncContext` 文件不存在时退回路径 key
- **文件**：src/sync/SyncContext.cpp:46
- **规格条款**：设计文档 §4.3 / G-07
- **问题描述**：POSIX `stat()` 失败时退回 absolute path，违反“OS 文件标识而非路径字符串”的硬要求；路径别名可能创建两个 context。
- **修复建议**：同步初始化要求数据库文件已存在并可 `stat()`；失败直接 `E_SYNC_INIT`，不要路径 fallback。

### M-05：SelectionPush 入站未维护 `__sync_push_progress`
- **文件**：src/sync/SyncWorker.cpp:419
- **规格条款**：设计文档 §6.1 / §5.4
- **问题描述**：处理分片时只更新 chunk 表，不创建/更新 push 表；若 DDL 外键启用，chunk 插入可能失败但代码忽略 `upsert.exec()` 返回值（src/sync/apply/CapturedWriteTemplate.cpp:237）。
- **修复建议**：收到首片先 upsert `__sync_push_progress(status='receiving')`；每片事务内检查并写 chunk，所有片完成后置 `applied/done`。

### M-06：Export auto join 只使用第一个 fkInject group
- **文件**：src/sql/SqlBuilder.cpp:64
- **规格条款**：fk-injection spec / export-reverse-lookup spec
- **问题描述**：多父/多组 `fkInject` 时导出 JOIN 只取 `route.fkInject[0]`，无法表达多父表注入后的导出关系。
- **修复建议**：按 route graph 明确父边；若一个 child 有多个 parent group，应生成对应 JOIN 或在 profile validation 中拒绝当前不支持的导出形态。

## Low / Info（建议优化项）

### L-01：`DataBridge::syncActive_` 是无锁 bool
- **文件**：src/DataBridgePrivate.h:35
- **规格条款**：并发维度 3
- **问题描述**：SyncEngine 初始化/析构与宿主 import 并发时存在数据竞争。
- **修复建议**：改为 `std::atomic_bool` 或纳入 SyncContext mutex/gate。

### L-02：现有构建目录未注册测试
- **文件**：build/CTestTestfile.cmake:1
- **规格条款**：测试覆盖维度 7
- **问题描述**：执行 `ctest --test-dir build --output-on-failure` 返回 `No tests were found`。源码下有 `tests/unit`，但当前构建产物未纳入 CTest。
- **修复建议**：修复 CMake/qmake 测试注册，CI 至少跑 ETL OpenSpec 单测与新增同步夹具。

## 优先修复清单

| 优先级 | Issue ID | 标题 | 文件 | 预估工作量 |
|---|---|---|---|---|
| P0 | C-03 | 同步导入跨线程与嵌套事务 | src/sync/SyncWorker.cpp | XL |
| P0 | C-04 | RowWinner 无法阻止低 rank 后到覆盖 | src/sync/apply/ChangesetApplier.cpp | XL |
| P0 | C-01 | `syncSelected()` 空实现 | src/sync/SyncEngine.cpp | XL |
| P0 | C-02 | `sync()` 不执行 drain | src/sync/SyncEngine.cpp | L |
| P0 | C-05 | SelectionPush ACK/progress 未闭合 | src/sync/SyncWorker.cpp | L |
| P1 | C-06 | BatchTransfer 接口缺 xlsxPath | src/batch/BatchTransfer.cpp | M |
| P1 | C-07 | ComparisonSession 保存绕过写线程 | src/sync/diff/ComparisonSession.cpp | L |
| P1 | H-01 | 原始 WHERE 拼接 | include/dbridge/sync/SyncSelection.h | M |
| P1 | H-02 | 广播水位混用 | src/sync/SyncWorker.cpp | L |
| P1 | H-05 | table_state 增量计算错误 | src/sync/apply/CapturedWriteTemplate.cpp | M |
| P2 | H-06 | FkInjector no-op | src/mapping/FkInjector.cpp | M |
| P2 | H-10 | DB 空字符串时间导出未报错 | src/service/ExportService.cpp | S |

## 测试覆盖缺口

- 同步核心缺少夹具：AppliedVector gap/no-op/apply、InboxLedger seen/consumed、三时机重扫、typed ACK 超时、RowWinner 到达序无关、SelectionPush chunk 幂等、push 全片 ACK、Outbox 原子发布崩溃恢复。
- 并发缺少夹具：BatchTransfer 后台导入/导出不得跨线程使用 `QSqlDatabase`，同步激活时旧写入口必须阻断或改道。
- 场景2缺少夹具：ComparisonSession 初始化、stale 检测、InboundTableGate defer/release、save 经 `WriteTxn` 捕获 changelog。
- ETL 仍需补充：fkInject child 已有冲突值 alignment、ExportService DB 空字符串时间解析、SqlBuilder 多 fkInject group 导出、raw SQL/identifier quote 安全测试。
- 当前 `ctest --test-dir build --output-on-failure` 未发现测试，CI 状态不能证明现有 `tests/unit` 夹具已执行。

## 总结

当前 ETL 主链路已有较多 OpenSpec 能力，但同步引擎尚未形成可用闭环。最需要先处理的 5 件事是：修复同步导入的线程/事务边界、重做 RowWinner 裁决位置、实现 `syncSelected()`、让 `sync()` 真正 drain、补齐 SelectionPush typed ACK 和 progress。完成这些之前，同步引擎不应进入生产或长时间数据一致性测试。
