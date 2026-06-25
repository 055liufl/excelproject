# dbridge 全量代码评审（第三轮）
评审版本：commit c0081c7（三轮修复后）
对照规格：ETL 设计文档 + ETL MVP 实现 + 同步引擎设计 v0.5 + 计划 v0.5

## A. 总体结论

ETL 层吻合度：中高。Profile 驱动、Schema 自省、lookup 预取、fkInject、TopoSorter、SQLite `ON CONFLICT DO UPDATE` 等主链路已具备，且 time format、columnOrder、exportOnMissing 等后续字段也已纳入。但 `ImportService` 当前对行级校验错误采取“跳过错误行、继续写入其他行”的语义，和 MVP “只要发现错误，终止本次导入，不写入任何数据”存在硬偏差。

同步引擎吻合度：低到中低。接口外形、DDL、单写线程、Session Extension 闸门、RowWinnerStore、AppliedVector、InboxLedger、OutboxWriter 等骨架已经存在，但若按 v0.5 的 G-01/G-03/G-05/G-08 与 M1/M2 DoD 断言，仍有多处 correctness blocker。当前更接近“可编译原型 + 部分修复点落地”，还不能作为星型增量同步的可靠实现。

Critical 问题数：4。High 问题数：10。当前整体状态：ETL 可继续小范围修正后作为 MVP 使用；同步引擎不建议交付，需要先修正广播锚点、RowWinner 后到低 rank 覆盖、`syncSelected()` 空实现、BatchTransfer/Import 改道线程与事务问题。

## B. ETL 层评审

### ImportService / ExportService

已满足的主能力：

- 结构校验：`ImportService::run` 在读 header 后调用 `ProfileValidator::validate`，覆盖表、列、header、conflict key、lookup、fkInject、parent route 等检查（`src/service/ImportService.cpp:508`，`src/profile/ProfileValidator.cpp:37`）。
- 类型/词法校验：`Mapper::compileValidators` 与 row mapping 阶段执行 validator token，错误进入 `ErrorCollector`（`src/service/ImportService.cpp:529`，`src/service/ImportService.cpp:608`）。
- 业务约束：lookup 预取、lookup key 空/类型/未命中/歧义、FK preflight、批内唯一性均在写库前执行（`src/service/ImportService.cpp:316`，`src/service/ImportService.cpp:573`，`src/service/ImportService.cpp:619`，`src/service/ImportService.cpp:629`）。
- Upsert：写入使用 `SqlBuilder::buildUpsert` 生成 `INSERT ... ON CONFLICT(...) DO UPDATE SET ...`，冲突列全是 PK 时退化为 `DO NOTHING`，没有使用 `INSERT OR REPLACE`（`src/service/ImportService.cpp:683`，`src/sql/SqlBuilder.cpp:31`）。
- lookup 预取：按 lookup identity 合并，预扫描 Excel 后分批 SELECT，避免逐行查库（`src/service/ImportService.cpp:316`）。
- fkInject：真正执行在 `ImportService.cpp` 的 `doFkInject`，支持多 pair、NULL strict、祖先失败级联抑制、同步 conflictVals（`src/service/ImportService.cpp:86`）。
- ExportService：支持 auto join select、mixed class 聚合、reverse lookup 预取、columnOrder、time format DB→Excel 转换（`src/service/ExportService.cpp:546`）。

主要残留问题：

- ETL-01：`ImportService` 行级错误不整体 abort。代码只在 table-level error 时返回，row-level error 会加入 `failedExcelRows` 并跳过该行，随后仍提交其他行（`src/service/ImportService.cpp:652`，`src/service/ImportService.cpp:677`，`src/service/ImportService.cpp:687`）。这违反 MVP §1 “只要发现错误，终止本次导入，不写入任何数据”。
- ETL-02：`ImportOptions::abortOnError` 没有参与 `ImportService::run` 的分支控制（`include/dbridge/Types.h:16`，`src/service/ImportService.cpp:480`）。字段存在，但实际语义被固定为“行级跳过”。
- ETL-03：`src/mapping/FkInjector.cpp` 是 no-op，真实 fkInject 逻辑内嵌在 `ImportService.cpp`。功能可用，但 DRY/模块边界不符合设计中独立 `FkInjector` 的复用预期（`src/mapping/FkInjector.cpp:7`，`src/service/ImportService.cpp:86`）。
- ETL-04：`SchemaIntrospector` 覆盖 `table_xinfo`、`index_list`、`foreign_key_list`，但 PRAGMA SQL 用字符串拼接 table/index 名（`src/schema/SchemaIntrospector.cpp:26`，`src/schema/SchemaIntrospector.cpp:76`，`src/schema/SchemaIntrospector.cpp:108`）。Profile 侧限制简单 identifier，普通使用风险低；若未来支持 quoted identifier，需要统一 quote helper。

### ProfileSpec / Public API / Schema

- `ProfileSpec` 字段完整性较好：routes/classes、conflict、lookups、fkInject、date/datetime/time format、columnOrder、exportOnMissing 均已建模（`src/profile/ProfileSpec.h:41`，`src/profile/ProfileSpec.h:82`，`src/profile/ProfileSpec.h:93`，`src/profile/ProfileSpec.h:108`）。
- `DataBridge` MVP 同步接口完整：`open/close/loadProfile/loadProfileFromString/importExcel/exportExcel/generateAutoProfileJson` 均存在（`include/dbridge/DataBridge.h:22`）。
- `SchemaCatalog/SchemaIntrospector` 覆盖列、索引、外键，且用 `table_xinfo` 识别 generated columns（`src/schema/SchemaCatalog.h:8`，`src/schema/SchemaIntrospector.cpp:23`）。
- `TopoSorter` 是 Kahn 算法，能检测 parent route cycle（`src/mapping/TopoSorter.cpp:10`）。
- `Errors.h` 已包含 ETL 主错误码与同步 v0.5 新增码（`include/dbridge/Errors.h:5`，`include/dbridge/Errors.h:51`）。

## C. 同步引擎发现清单

| 编号 | 严重级别 | 位置(文件:函数) | 类别 | 问题 | 修复建议 |
|---|---|---|---|---|---|
| K-01 | Critical | `src/sync/SyncWorker.cpp:broadcastTopeer` | 增量广播/ACK | 广播锚点混用 `last_sent_seq` 与 routing 判定。`afterLocalSeq` 是本地 `local_seq`，却传给 `RoutingTable::shouldRoute` 当作 `peerAckedSeq` 去比较 `originSeq`，两者不是同一序列空间；当 `local_seq` 已较大而远端 originSeq 较小，会错误跳过应发送变更（`SyncWorker.cpp:555`，`SyncWorker.cpp:568`，`RoutingTable.cpp:10`）。同时发送后立即 `updateLastSent`，不是“ACK 前移当且仅当收 ACK”。 | 将“扫描游标”和“ACK 锚点”分账：扫描游标只能用于批内续扫，不可作为可靠投递水位；路由判定必须查 `(peer, origin, epoch).acked_seq`。成功 ACK 后再推进可裁剪锚点。 |
| K-02 | Critical | `src/sync/apply/ChangesetApplier::apply/updateWinnersFromChangeset` | G-01 冲突仲裁 | 低 rank 变更如果没有触发 SQLite apply_v2 conflict callback，会先真实覆盖业务表，然后 `updateWinnersFromChangeset` 只是不更新 `__sync_row_winner`，无法撤销已落库的低 rank 后到写入（`ChangesetApplier.cpp:94`，`ChangesetApplier.cpp:113`，`RowWinnerStore.cpp:43`）。这不能保证“低 rank 跨批后到不覆盖”。 | 在 apply 前或 changeset filter 中逐行查询 RowWinner，低 rank 后到应直接 OMIT/跳过；或 apply 后按 winner 状态回滚/重放胜者行。仅维护 metadata 不足以保证业务表终态。 |
| K-03 | Critical | `src/sync/SyncEngine::syncSelected` | M2 上行选择 | `syncSelected()` 只 enqueue 一个空 lambda，未调用 `SelectionResolver/FkClosureBuilder/FrozenManifest/ChunkStreamer/OutboxWriter`，也不更新进度、结果、ACK 等待（`SyncEngine.cpp:192`）。 | 按 plan T2.5-T2.13 接入完整选择推送流水线；在未实现前应返回明确 `E_SYNC_UNSUPPORTED` 或 `E_SYNC_SELECTION_EMPTY`，避免假成功。 |
| K-04 | Critical | `src/sync/SyncWorker::submitImportSync` + `src/service/ImportService::run` | 线程/事务/写捕获 | BatchTransfer 导入改道不可靠：`submitImportSync` 外层 `WriteTxn` 后调用 `ImportService::run`，后者再次 `db.transaction()`，Qt SQLite 嵌套事务通常失败（`SyncWorker.cpp:654`，`ImportService.cpp:669`）。同时 `DataBridge::runImportOnDb` 在 worker 线程调用 `refreshCatalog()` 读取主线程 `db_`，违反 QtSql 线程亲和性（`DataBridge.cpp:242`）。 | 提取无事务版 ImportService/UpsertExecutor 路径，由 `CapturedWriteTemplate` 分支 C 持事务和 session；DataBridge 的 profile/catalog 快照必须在调用线程复制成值后传入 worker，不得在 worker 触碰主连接。 |
| K-05 | High | `src/batch/BatchTransfer::runImport/runExport` | Public API 语义 | `IBatchTransfer::startImport(const ImportOptions&)` 没有 xlsxPath 参数，实现约定把 `opts.profileName` 当 xlsx 路径，导致 profileName 同时作为文件路径和 profile id，实际无法同时表达二者（`BatchTransfer.cpp:95`，`BatchTransfer.cpp:143`）。 | 扩展 `ImportOptions/ExportOptions` 或 `IBatchTransfer` 方法签名，显式传 `xlsxPath`；保持 `profileName` 只表示 profile。 |
| K-06 | High | `src/sync/SyncEngine::sync` + `src/sync/SyncWorker::startAckWait` | 状态机/ACK | `sync()` 在调用线程先置 `Exporting` 并启动 ACK deadline，再 enqueue 一个只 release gate 的任务；任务不保证先于长任务执行，gate 可能长时间不释放。ACK timeout 只 emit error，`SyncEngine::onWorkerError` 不把 state 置为 `Failed`（`SyncEngine.cpp:138`，`SyncEngine.cpp:140`，`SyncWorker.cpp:247`，`SyncEngine.cpp:250`）。 | foreground gate release 与 ACK deadline 应在 worker 真正完成打包/发布后启动；收到 `E_SYNC_ACK_TIMEOUT` 时同步更新 state/result 为 Failed。 |
| K-07 | High | `src/sync/apply/CapturedWriteTemplate::branchBC` | J-16 table_state | J-16 的 `SELECT EXISTS` 在 `UpsertExecutor.apply()` 之后执行，因此 upsert 后目标行必然存在，insert/update 判断失真；随后 old row hash 也读取的是 upsert 后的新值，导致 checksum delta 错误（`CapturedWriteTemplate.cpp:198`，`CapturedWriteTemplate.cpp:240`，`CapturedWriteTemplate.cpp:249`，`CapturedWriteTemplate.cpp:286`）。 | 在执行 UPSERT 前读取存在性和 beforeHash，并在同一 `BEGIN IMMEDIATE` 事务内完成“读旧值 → upsert → 计算 delta → 更新 table_state”。 |
| K-08 | High | `src/sync/transport/AckChannel::flush` + `SyncDDL::ackArtifactName` | ACK 文件命名 | 多个 pending ACK 在同一毫秒、同一 from/to 下使用同一个 `ack__{from}__{to}__{ms}.ack` 文件名，`AckChannel` 又忽略 `writeAck` 返回值；Qt `QFile::rename` 目标存在时会失败，ACK 会丢失（`AckChannel.cpp:24`，`AckChannel.cpp:30`，`OutboxWriter.cpp:67`，`SyncDDL.h:173`）。 | 文件名加入单调计数/UUID/ackedSeq；检查 `writeAck` 失败并上报 `E_SYNC_TRANSPORT`。 |
| K-09 | High | `src/sync/SyncWorker::processAckArtifact` | ACK 路由 | ACK 文件名解析出 `fromPeer`，但没有校验 `toPeer` 是否等于本节点；ACK payload 也不包含 toPeer。节点可能处理不属于自己的 ACK（`SyncWorker.cpp:508`，`SyncWorker.cpp:520`）。 | 解析并校验文件名中的 toPeer，ACK payload 也应带 toPeer 或 recipient，非本节点直接忽略。 |
| K-10 | High | `src/sync/SyncWorker::processChangesetArtifact` + `ChangesetApplier::apply` | Schema/安全边界 | `sqlite3changeset_apply_v2` 的 xFilter 为 nullptr，入站 changeset 可修改所有表，而不是仅限 `syncTables`（`ChangesetApplier.cpp:94`）。 | 实现 xFilter，仅允许 config.syncTables 和必要 sync meta 表；非同步表载荷 quarantine。 |
| K-11 | High | `src/sync/diff/ComparisonSession.cpp:createComparisonSession` | 场景2 接口完整性 | 工厂返回的 session 没有执行 `initialize()`，接口上也没有 initialize 方法；返回对象的 `wconn` 实际是 read-only `rconn`，`save()` 会在只读连接上写入，且不走 `CapturedWriteTemplate`（`ComparisonSession.cpp:391`，`ComparisonSession.cpp:413`）。 | 工厂应接入 SyncContext/SyncWorker，创建会话时完成远端 meta/rows 初始化；save 必须入单写线程并走分支 C。 |
| K-12 | High | `src/sync/SyncContext.cpp:getExisting` + `SyncEngine::initialize/~SyncEngine` | 生命周期 | `BatchTransfer` 通过 `getExisting()` 拿到 shared_ptr 后调用 `ctx->importFn`；该 lambda 捕获 `SyncEngine this` 与 `worker_`（`SyncEngine.cpp:106`）。若 SyncEngine 析构与 BatchTransfer 任务并发，ctx 可能仍活着但 lambda 指向正在析构的 engine/worker。 | `importFn` 不应捕获 `SyncEngine*`；改为捕获 `std::weak_ptr` 到 worker/context，调用前 lock，析构时先清空 importFn 并等待在途任务。 |
| K-13 | High | `src/DataBridge.cpp:setSyncActive/importExcel` | J-09 写边界 | `syncActive_` 是普通 bool，无锁/atomic；SyncEngine 初始化/析构线程与宿主 import 线程并发存在数据竞争。并且只挡 `DataBridge::importExcel`，`runImportOnDb` 是 public 且不检查，BatchTransfer fallback 仍可绕开 session（`DataBridge.cpp:176`，`DataBridge.cpp:180`，`DataBridge.cpp:224`）。 | 用共享 ForegroundGate/atomic 管理同步激活；所有写入口统一路由或拒绝，`runImportOnDb` 不应作为 public bypass。 |
| K-14 | Medium | `src/sync/apply/CapturedWriteTemplate::branchA` + `InboxLedger` | G-05 缺口处理 | 严格连续 check 存在，gap 时 ledger 保持 `seen`，但每次重扫都会立即报 `E_SYNC_GAP`，没有 gapTimeout/阈值，也没有回退 baseline（`CapturedWriteTemplate.cpp:74`，`SyncWorker.cpp:342`，`SyncWorker.cpp:350`）。 | gap 首次进入 pending 状态，只记录 first_seen；超阈值再 emit `E_SYNC_GAP` 并触发 baseline。 |
| K-15 | Medium | `src/sync/selection/FkClosureBuilder::topoSort` | FK 闭包 | 同表自引用 FK 如果引用自身同一 PK，topoSort 会给自己加自环并报 `E_SYNC_FK_CYCLE_UNSUPPORTED`（`FkClosureBuilder.cpp:149`，`FkClosureBuilder.cpp:153`，`FkClosureBuilder.cpp:158`）。 | 对 `refIdx == i` 的自引用行跳过依赖边；只有多行环才按 cycle 处理。 |
| K-16 | Medium | `src/sync/SyncWorker.cpp:processSelectionPushArtifact` | SelectionPush 幂等 | push chunk progress 的重复 chunk 只看 `status='applied'`，没有校验重复 chunk checksum 一致；写入 progress 时 checksum 固定为空字符串（`CapturedWriteTemplate.cpp:162`，`CapturedWriteTemplate.cpp:224`）。 | 以 `(push_id, chunk_seq)` 存 checksum；重复同 checksum no-op，异 checksum 报 `E_SYNC_PAYLOAD_CORRUPT`。 |
| K-17 | Medium | `src/sync/schema/TableStateStore::updateRow` | table_state 并发/原子性 | updateRow 先 `readChecksum/readRowCount`，再 UPSERT 写回，读改写不是单 SQL 原子操作；当前单写线程降低竞态，但若任何路径绕过单写者会丢 delta（`TableStateStore.cpp:184`）。 | 使用 SQL 表达式在 UPSERT 中基于现值做增量，或强制所有 table_state 更新都在同一写连接独占事务内并消除 bypass。 |
| K-18 | Medium | `src/sync/SyncWorker.cpp:processChangesetArtifact/broadcastTopeer` | Rebase buffer 生命周期 | rebase buffer LRU 仅 500 条，按 apply 后插入淘汰；广播可能在 buffer 被淘汰后才处理旧 changelog entry，代码会直接发送未 rebased changeset（`SyncWorker.cpp:394`，`SyncWorker.cpp:575`）。 | rebase buffer 与 changelog entry 建引用/持久化关系；缺 rebase buffer 时应报 `E_SYNC_REBASE_FAILED` 并停止外发，而不是静默发送原 blob。 |

## D. 设计不变量核查表

| 不变量 | 状态 | 代码依据（函数:行） |
|---|---|---|
| G-01 RowWinnerStore：apply 后 updateWinnersFromChangeset + 冲突回调 sqlite3changeset_pk | 部分落地，不满足终态不变量 | `RowWinnerStore::put` 有 rank/seq winner（`RowWinnerStore.cpp:43`）；冲突回调用 `sqlite3changeset_pk`（`ChangesetApplier.cpp:144`）；apply 后更新 winner（`ChangesetApplier.cpp:113`）。但非冲突低 rank 后到会先覆盖业务表，见 K-02。 |
| G-02 长推送撞迁移半截语义 | 基本未实现 | 仅 selection push schemaVer 不同时报 `E_SYNC_PUSH_SCHEMA_MOVED`（`SyncWorker.cpp:423`）；没有 stop 取消后 baseline 收口、静默窗排空、旧片整发拒收完整流程。 |
| G-03 CapturedWriteTemplate 三分支 | 部分落地 | 分支 A 不 fresh capture，appendForward 原 blob（`CapturedWriteTemplate.cpp:63`，`CapturedWriteTemplate.cpp:123`）；B/C 共用 fresh session + UpsertExecutor（`CapturedWriteTemplate.cpp:149`，`CapturedWriteTemplate.cpp:190`）。但 BatchTransfer 导入未真正走该分支 C，另写了一套 wrapper（`SyncWorker.cpp:654`）。 |
| G-04 eligibility 在 wconn 建立后调用 | 已落地 | worker `run()` 内 open wconn、DDL 后调用 `SchemaEligibility::verify`，在 tpl/session capture 前返回错误（`SyncWorker.cpp:71`，`SyncWorker.cpp:120`）。 |
| G-05 AppliedVector 严格连续 + 缺口 ledger seen | 部分落地 | `AppliedVectorStore::check` 只允许 `seq==applied+1`（`AppliedVectorStore.cpp:20`）；artifact 先 mark seen，成功才 consumed/ACK（`SyncWorker.cpp:342`，`SyncWorker.cpp:350`）。缺 gapTimeout/baseline，见 K-14。 |
| G-06 DiffEngine 三元组判等（不含 high_water） | 已落地 | Identical 判定使用 `localChecksum == rm.checksum && localFp == rm.schemaFp && localRowCount == rm.rowCount`，不读 high_water（`DiffEngine.cpp:41`）。 |
| G-07 SyncContext dev/inode key + canonicalKey_ release | 部分落地 | POSIX 用 `(st_dev, st_ino)`，release 使用 initialize 保存的 `canonicalKey_`（`SyncContext.cpp:45`，`SyncEngine.cpp:46`，`SyncEngine.cpp:22`）。URI 归一化、未建库 key 升级、Windows case/路径边界仍较薄。 |
| G-08 OutboxWriter 原子发布 + InboxWatcher scan + ledger | 部分落地 | 写 tmp→flush/fsync→rename→ready→dir fsync（`OutboxWriter.cpp:24`）；Inbox scan `.ready` 并 ledger seen/consumed（`InboxWatcher.cpp:19`，`InboxLedger.cpp:20`）。但 QFile rename 不覆盖、ACK 命名冲突、三时机扫描和 transport 错误处理不完整，见 K-08。 |

## E. 上轮修复验收

| J 编号 | 状态 | 依据 |
|---|---|---|
| J-01 readRangeAll / excludeOrigin / lastSentLocalSeq | 未通过 | `readRangeAll(origin != peer)` 能包含本节点变更（`ChangelogStore.cpp:71`），但 `lastSentLocalSeq` 与 `originSeq` 混比导致漏发，且发送后即推进 last_sent（`SyncWorker.cpp:550`）。 |
| J-02 sync() 状态机 / ackWaiting atomic / ACK progressUpdated | 部分通过 | `ackWaiting_` 是 atomic（`SyncWorker.h:145`），ACK 到达 emit Completed（`SyncWorker.cpp:525`）；timeout 只报错不置 Failed，gate release 依赖 worker 空任务执行（`SyncEngine.cpp:140`）。 |
| J-03 shared_ptr<promise> | 部分通过 | promise 生命周期本身由 `shared_ptr` 保住（`SyncWorker.cpp:632`）；但 lambda 捕获 `this/bridgePtr`，并发析构与跨线程 DataBridge 访问仍不安全（`SyncWorker.cpp:642`）。 |
| J-04 push schema moved | 部分通过 | schemaVer 不同会报 `E_SYNC_PUSH_SCHEMA_MOVED`（`SyncWorker.cpp:423`）；但没有整 push 状态机和半截片收口。 |
| J-05 selection push pkColumns | 部分通过 | 从 `PRAGMA table_info` 填 pkColumns（`SyncWorker.cpp:443`）；但 `syncSelected()` 发送端未实现，无法端到端断言。 |
| J-06 | 未见可断言闭环 | 当前代码没有清晰标注或完整端到端实现证据，相关选择推送/分片仍缺主流程。 |
| J-07 | 未见可断言闭环 | 当前代码没有清晰标注或完整端到端实现证据。 |
| J-08 corrupt quarantine | 基本通过 | decode 失败 mark corrupt 并复制到 quarantineDir、删除原文件（`SyncWorker.cpp:327`）。 |
| J-09 E_SYNC_WRITE_BLOCKED | 部分通过 | `DataBridge::importExcel` 同步激活后拒绝（`DataBridge.cpp:180`）；但 bool 非 atomic，且只覆盖该入口，见 K-13。 |
| J-10 getExisting 不增 refCount | 部分通过 | `getExisting()` 不创建也不增 refCount（`SyncContext.cpp:78`）；shared_ptr 可保 ctx alive，但 importFn 捕获 engine raw pointer 有并发析构风险，见 K-12。 |
| J-11 Outbox 原子发布 | 部分通过 | tmp/ready/fsync 顺序存在（`OutboxWriter.cpp:24`）；但 `QFile::rename` 不覆盖目标，ACK 同名冲突未处理，见 K-08。 |
| J-12 table_state not found 区分 | 基本通过 | `readState` 用 `found` 区分 not found 与 query error（`TableStateStore.cpp:64`）。 |
| J-13 rebase buffer LRU | 部分通过 | insertion-order LRU 存在（`SyncWorker.cpp:394`）；但 buffer 淘汰后广播会发送未 rebased 原 blob，见 K-18。 |
| J-14 init precise error | 基本通过 | `E_SYNC_SESSION_UNAVAILABLE` / `E_SYNC_UNSUPPORTED_SCHEMA` 前缀映射到错误码（`SyncEngine.cpp:71`，`SyncEngine.cpp:90`）。 |
| J-15 ACK peer parse | 部分通过 | 从文件名解析 fromPeer 并 updateAcked（`SyncWorker.cpp:508`）；未校验 toPeer，见 K-09。 |
| J-16 table_state UPSERT | 未通过 | 存在性和 old hash 在 UpsertExecutor 之后读取，无法正确区分 insert/update，也无法正确 delta（`CapturedWriteTemplate.cpp:198`，`CapturedWriteTemplate.cpp:246`）。 |

## F. ETL DRY 验证

| 组件 | 同步引擎复用状态 | 评价 |
|---|---|---|
| UpsertExecutor | 部分复用 | 同步层有独立 `src/sync/apply/UpsertExecutor`，Branch B/C 和 SelectionPushApplier 使用它（`CapturedWriteTemplate.cpp:198`，`SelectionPushApplier.cpp:17`）。但 ETL `ImportService` 仍使用自己的 `SqlBuilder` + QSqlQuery loop（`ImportService.cpp:683`），没有复用同步 UpsertExecutor；DRY 未达成。 |
| FkClosureBuilder | 未复用 ETL FkInjector | 同步层有 `src/sync/selection/FkClosureBuilder`，直接基于 `SchemaCatalog.foreignKeys` BFS（`FkClosureBuilder.cpp:78`）。ETL 的 `FkInjector` 文件本身 no-op，真实逻辑在 ImportService 内嵌；两边不是同一实现。 |
| SchemaIntrospector / SchemaCatalog | 部分复用 | ETL 使用 `SchemaIntrospector` 构建 catalog（`DataBridge.cpp:85`）。同步 `SchemaEligibility` 自己用 SQL/PRAGMA introspect，没有直接复用 ETL `SchemaIntrospector`（`SchemaEligibility.cpp:73`）。FkClosureBuilder 复用 `SchemaCatalog` 输入。 |
| TopoSorter | ETL 使用，Selection closure 自实现 | ETL `TopoSorter` 用 Kahn（`TopoSorter.cpp:10`）。同步 `FkClosureBuilder::topoSort` 另写 Kahn 变体（`FkClosureBuilder.cpp:131`），符合行级闭包需求，但不是直接复用。 |

结论：DRY 目标只部分达成。同步层复用了概念和部分数据结构，但关键实现仍多处分叉，尤其 UpsertExecutor、FkInjector/FkClosure、SchemaEligibility。

## G. 亮点

- ETL Profile 建模已经比较完整，lookup、fkInject、time format、reverse lookup、columnOrder 等都不是空字段，且 loader/validator 对不少约束做了前置检查。
- ETL 导入的 lookup 预取设计正确避开逐行查库，按 identity 合并并按 SQLite 变量数分批。
- SchemaIntrospector 使用 `table_xinfo`，比单纯 `table_info` 更能识别 generated/hidden 列。
- 同步层坚持在 `SyncWorker::run()` 内创建写连接，方向正确，避免了直接把 `DataBridge.db_` 移给后台线程。
- v0.5 关键持久表已建：`__sync_changelog`、`__sync_applied_vector`、`__sync_row_winner`、`__sync_inbox_ledger`、`__sync_table_state` 等均有 DDL。
- `AppliedVectorStore` 的严格连续 check 是正确的基础形态，配合 ledger seen 已经具备 pending gap 的雏形。
- `DiffEngine` 的表级判等已按三元组实现，没有把 high_water 当一致性条件。
- Outbox 发布顺序有 fsync 意识，虽然还需要修 ACK 命名和 rename 语义，但方向比普通直接写 final 文件更可靠。
