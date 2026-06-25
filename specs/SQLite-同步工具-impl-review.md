# 同步引擎实现评审（第二轮）
评审版本：实现 commit 7f1475c（两轮修复后）
对照文档：设计 v0.5 / 计划 v0.5

## A. 总体结论

吻合程度：**中偏低**。

当前实现较第一轮有明显收敛：写连接已移入 `SyncWorker::run()`；`SchemaEligibility::verify()` 在 `wconn` 建立后执行；`ChangesetApplier` 在 `apply_v2` 成功后维护 `__sync_row_winner`；`CapturedWriteTemplate` A 分支不再 fresh capture，并保存原始 blob；`OutboxWriter` 的原子发布顺序基本正确；`InboxWatcher::scan()` + `InboxLedger` 已形成幂等消费骨架。

但仍有 **4 个 Critical** 阻塞问题：广播/ACK 路由错位、`sync()` 前台状态机与 ACK 等待语义失真、`submitImportSync()` promise 生命周期和跨线程 `DataBridge` 访问不安全、迁移 T5.4 三路径未实现。另有多项 High 问题会破坏 G-01/G-03/G-05/G-08 的可断言性。

结论：**不可进入阶段 0 硬验收/不可作为 M1-M2 收敛验收通过**。尤其是星型广播、ACK 锚点、批量导入改道、严格连续缺口收口、迁移撞推送语义仍不可依赖。

## B. 发现清单

| 编号 | 严重级别 | 位置(文件:函数) | 类别 | 问题 | 修复建议 |
|---|---|---|---|---|---|
| J-01 | Critical | `src/sync/SyncWorker.cpp:SyncWorker::broadcastTopeer`; `src/sync/capture/ChangelogStore.cpp:ChangelogStore::readRange`; `src/sync/transport/AckChannel.cpp:AckChannel::flush` | 传输/ACK 路由 | `broadcastTopeer()` 先按 `(peer, localOrigin)` 取 ACK，但 `readRange()` 用 `origin = peer` 查询 changelog，导致本地 origin 的变更不会发给 peer，反而可能把 peer 自己的 origin 发回 peer。ACK 文件名又写死 `fromPeer=self`，`processAckArtifact()` 解析后会把 ACK 记到 peer=`self`，无法推进真实发送端锚点。 | `readRange` 应按目标 peer 的 per-origin ACK 读取所有待发 origin（至少本地 origin 和需转发 origin），排除回声；ACK 生成需携带本节点 `nodeId` 和目标 peer，`AckChannel` 需要知道 from/to 或由 `SyncWorker` 传入。 |
| J-02 | Critical | `src/sync/SyncEngine.cpp:SyncEngine::sync`; `src/sync/SyncWorker.cpp:SyncWorker::startAckWait`; `src/sync/SyncWorker.cpp:SyncWorker::processChangesetArtifact` | 前台状态机/线程安全 | `sync()` 调 `startAckWait()` 后只入队一个释放 gate 的空任务，worker 执行该任务即把状态置 `Completed`，没有等待广播完成或 typed ACK。`ackWaiting_` 从调用线程写、worker 线程读写，未加锁；且任何入站 changeset apply 成功都会清除 ACK 等待，不是真实出站 ACK。 | 将 ACK 等待状态作为 worker 线程任务创建并保护；前台状态应为 `Exporting`/`percent=-1` 直到对应 peer 的 ACK 足额到达；只有 `processAckArtifact()` 更新锚点后才能完成，超时才置 `Failed + E_SYNC_ACK_TIMEOUT`。 |
| J-03 | Critical | `src/sync/SyncWorker.cpp:SyncWorker::submitImportSync`; `src/DataBridge.cpp:DataBridge::runImportOnDb` | 线程模型/生命周期 | `submitImportSync()` 以引用捕获栈上 `std::promise`；60 秒超时返回后 worker 任务仍可能执行并写已销毁 promise。`!wconnPtr_` 分支还解引用 `*wconnPtr_`。任务内调用 `bridge.runImportOnDb()`，而该函数会执行 `d_->refreshCatalog()`，使用 `DataBridge` 主线程 `db_` 做 introspection，违反 Qt `QSqlDatabase` 线程归属；`catalog_`/`profiles_` 也无快照锁。 | 使用 `shared_ptr<promise>` 或取消超时后保证任务不再访问；无 `wconnPtr_` 时直接返回错误。`DataBridge` 应提供只读元数据快照，worker 用自己的连接刷新/使用 catalog，禁止 worker 调主线程 `db_`。 |
| J-04 | Critical | `src/sync/baseline/BaselineManager.cpp`; `src/sync/SyncWorker.cpp`; 全局搜索 `E_SYNC_PUSH_SCHEMA_MOVED` | G-02/迁移 T5.4 | 仅有 baseline 序列化/应用基础类，未看到静默窗排空、`stop()` 取消后迁移后 re-baseline 收口、旧 schema push 迁移后整发拒收 `E_SYNC_PUSH_SCHEMA_MOVED` 的流程入口。 | 增加迁移状态机：DDL 前等待 push done；stop 取消记录待 baseline；schema 代际变化后按 push_id 整发拒收旧片并落 `E_SYNC_PUSH_SCHEMA_MOVED`，随后触发 baseline 收口。 |
| J-05 | High | `src/sync/apply/CapturedWriteTemplate.cpp:branchBC`; `src/sync/SyncWorker.cpp:processSelectionPushArtifact` | G-03/选择推送 | `processSelectionPushArtifact()` 构造 `RowMutation` 时未填 `pkColumns`，`UpsertExecutor` 的 `ON CONFLICT ()` 会无效；branchBC 的 chunk 幂等只查 `status='applied'`，未比较 checksum，重复 chunk 内容不同不会报 `E_SYNC_PAYLOAD_CORRUPT`。 | 从 `FrozenEntry`/schema catalog 填充每行 pkColumns；`push_chunk_progress` 存入并校验 checksum，同 chunk 异 checksum 直接 corrupt/quarantine。 |
| J-06 | High | `src/sync/SyncWorker.cpp:submitImportSync`; `src/sync/apply/CapturedWriteTemplate.cpp:branchBC` | G-03/写模板收口 | 批量导入路径没有调用 `CapturedWriteTemplate::execute(LocalWrite)`，而是手写 txn/session/seal；忽略 `rec_->begin()`、`sealInto()`、`txn.commit()` 结果，且不更新 `TableStateStore`、不维护 `RowWinnerStore`。branchBC 虽存在 fresh capture，但当前导入未走它。 | 导入/场景2 save/selection push 都应进入统一模板；去掉 submitImportSync 的手写事务。模板内任何 session/seal/commit 失败必须 rollback 并返回错误。 |
| J-07 | High | `src/sync/apply/CapturedWriteTemplate.cpp:branchA`; `src/sync/SyncWorker.cpp:processArtifact`; `src/sync/apply/AppliedVectorStore.cpp:check` | G-05/缺口收口 | 连续性判断存在，ledger `seen` 也会保留，但 `seq>applied+1` 立即返回 `E_SYNC_GAP` 并每轮重扫重复报错；没有 gapTimeout/阈值、没有首次 seen 时间驱动的延迟升级、没有 baseline fallback。 | 缺口先作为 pending seen 静默保留且不 ACK；记录 first_seen/retry，超过阈值才落 `E_SYNC_GAP` 并触发 BaselineManager。 |
| J-08 | High | `src/sync/SyncWorker.cpp:processArtifact`; `src/sync/transport/AckChannel.cpp`; `src/sync/schema/QuarantineStore.cpp` | G-08/传输幂等 | 损坏 payload 只 `markCorrupt`，没有移动到 `quarantineDir`；ACK 走同一个 outbox writer 但缺少真实 peer 路由；ledger 只记录 artifact_name，没有 checksum/size，无法检测同名替换或重复 ready 的内容漂移。 | corrupt 时写入/移动 quarantine；ledger 增加 checksum/byte_size 或至少重复 seen 时复核；ACK 使用 typed 目标 peer 路由。 |
| J-09 | High | `src/DataBridge.cpp:DataBridge::importExcel`; `src/sync/ForegroundGate.h`; `src/sync/SyncContext.cpp` | sync-aware 写边界 | 同步激活后，旧 `DataBridge::importExcel()` 仍可直接通过主线程 `db_` 写同步表，没有 `E_SYNC_WRITE_BLOCKED` 检查或改道；这绕过 session，违反 FR-1/G-03。 | 在 `DataBridge::importExcel()` 根据物理库 context 和 profile 目标表判断是否命中 syncTables，M1 返回 `E_SYNC_WRITE_BLOCKED`；M3 再改道到 worker。 |
| J-10 | High | `src/batch/BatchTransfer.cpp:BatchTransfer::runImport` | IBatchTransfer/路由契约 | `BatchTransfer` 为查找同步 context 调 `getOrCreate()`，会在未激活同步时创建空 context；同步导入路径把 `opts.profileName` 当 xlsxPath，破坏 profileName 语义，正常 profile 查找和文件路径无法同时正确。 | 增加 registry lookup-only API；`ImportOptions` 或 `startImport` 入参必须有独立 xlsxPath，不能复用 profileName。 |
| J-11 | High | `src/sync/diff/ComparisonSession.cpp:createComparisonSession`; `src/sync/diff/ComparisonSession.cpp:ComparisonSession::save` | 场景2/工厂契约 | factory 将同一个只读 `rconn` 同时传作 `wconn`，`save()` 直接 `StagingBuffer::save()` 到只读连接，且不走 `CapturedWriteTemplate + UpsertExecutor` 的同步写队列。 | ComparisonSession factory 只能创建读会话；save 必须经 SyncWorker 任务和 CapturedWriteTemplate 分支 C，或在未接入前显式返回 unsupported。 |
| J-12 | Medium | `src/sync/diff/DiffEngine.cpp:tableDiffs`; `src/sync/schema/TableStateStore.cpp:readState` | G-06/比对语义 | `DiffEngine` 注释认为 `readState()` 无记录返回 false，但 `readState()` 实际无记录返回 true 且输出空 fp/0 checksum/0 rowCount，导致无本地状态时被当成 `OnlyLocal` 或 `Different`，不是 `OnlyRemote`/`Identical`。 | 让 `readState()` 区分 found/error，或返回 `std::optional<TableState>`；修正 tableDiffs 分支。 |
| J-13 | Medium | `src/sync/SyncWorker.cpp:processChangesetArtifact`; `src/sync/SyncWorker.cpp:broadcastTopeer` | RebaseEngine 接入 | `sqlite3rebaser_*` 调用链完整，但 rebase buffer 以 `QHash<QString,QByteArray>` 缓存，`size()>500` 时 `erase(begin())` 不保证淘汰最旧；buffer 生命周期不可预测，可能丢掉仍需广播的 rebase 信息。 | 改为 `QCache`/`QLinkedHashMap` 或保存插入序队列，按时间或 changelog 锚点淘汰；淘汰前确认所有 peer 已 ACK。 |
| J-14 | Medium | `src/sync/SyncEngine.cpp:initialize`; `src/sync/SyncWorker.cpp:run` | 错误码触发点 | eligibility 失败时 worker `initError_` 字符串含 `E_SYNC_UNSUPPORTED_SCHEMA`，但 `SyncEngine` 记录到 errors ring 的 code 仍是 `E_SYNC_INIT`；调用方不能按 v0.5 错误码断言。 | init 失败应携带结构化 code，`E_SYNC_UNSUPPORTED_SCHEMA`、`E_SYNC_SESSION_UNAVAILABLE` 等不要折叠成 `E_SYNC_INIT`。 |
| J-15 | Medium | `src/sync/selection/FkClosureBuilder.cpp`; `src/sync/selection/SelectionResolver.cpp`; `src/sync/diff/DiffEngine.cpp:getPkColumn` | 复合 PK/选择闭包 | 多处只取 `pk==1` 的单列主键；eligibility 允许复合显式 PK，但选择解析、FK closure、row diff 都无法正确处理复合 PK。 | 公共化 PK 元数据，统一使用有序 PK 列组和规范序列化 key；SyncSelection 也需支持复合主键值。 |
| J-16 | Medium | `src/sync/apply/CapturedWriteTemplate.cpp:branchBC` | table_state 增量 | branchBC 将所有 `DoUpdate` 都当 insert：`tm.isInsert = (m.mode == DoUpdate)`，row_count 每次 +1；更新已有行会把 `row_count` 撑大，checksum 也缺少 beforeHash 扣减。 | UPSERT 后需要知道 insert/update/no-op 结果和旧行 hash，按 `+new`、`+new-old`、`0` 分别更新。 |
| J-17 | Low | `src/sync/transport/InboxWatcher.cpp:scan` | 扫描顺序 | 注释称 `.ready` oldest first，但 `QDir::Time` 默认不是明确 FIFO；乱序输入虽由 applied vector 拦住，但会增加 pending/重扫噪声。 | 显式按 mtime 升序或按 artifact 中 `(origin,seq)` 分组排序；不要依赖默认排序语义。 |

## C. 设计不变量核查表

| 不变量 | 状态(已落地/部分/未落地) | 代码依据 |
|---|---|---|
| G-01 RowWinnerStore：`apply_v2` 成功后更新 winner，PK hash 用 `sqlite3changeset_pk()` | **部分** | `ChangesetApplier::apply()` 在 `sqlite3changeset_apply_v2()` 成功后调用 `updateWinnersFromChangeset()`；冲突回调和 post-apply 都用 `sqlite3changeset_pk()`。但本地写/导入/selection push 没有维护 winner，完整多源到达序无关仍不可断言。 |
| G-02 T5.4 迁移三路径：排空、stop 取消后 re-baseline、旧片整发拒收 | **未落地** | `BaselineManager` 只有基础 export/apply；未发现迁移状态机、排空等待、`E_SYNC_PUSH_SCHEMA_MOVED` 触发或 stop 后 baseline 收口。 |
| G-03 CapturedWriteTemplate 三分支：A 挂起捕获存原 blob，B/C fresh 捕获 | **部分** | A 分支不调用 `SessionRecorder.begin()`，保存原 `changesetBlob` 到 `appendForward()`；branchBC 会 fresh capture。但实际导入走 `submitImportSync()` 手写路径，selection push 的 RowMutation/幂等不完整。 |
| G-04 initialize 调 `SchemaEligibility::verify`，且在 wconn 建立后调用 | **已落地** | `SyncWorker::run()` 打开 `wconn`、取 `sqlite3*`、建 DDL 后调用 `SchemaEligibility::verify(wconn, syncTables)`。观测层错误码仍被 `SyncEngine` 折叠为 `E_SYNC_INIT`。 |
| G-05 AppliedVector 严格连续；缺口复用 ledger seen + 三时机重判 | **部分** | `AppliedVectorStore::check()` 强制 `seq==applied_seq+1`；`processArtifact()` 失败不 consumed/ACK，`scanInbox()` 会合并 `pendingSeen()`。但缺口立即 `E_SYNC_GAP`，没有 gapTimeout/基线回退；启动/周期扫描有，文件系统 watcher 时机已简化为轮询。 |
| G-06 DiffEngine Identical = `schema_fp + row_count + checksum` | **部分** | `DiffEngine::tableDiffs()` 的 Identical 判断确实使用三元组且不含 high_water；但 `readState()` 无记录返回 true，使无状态表的 OnlyLocal/OnlyRemote 判断错误。 |
| G-07 SyncContextRegistry 使用 dev/inode key，release 同 key | **已落地** | `SyncContextRegistry::canonicalKey()` POSIX 用 `stat().st_dev/st_ino`，Windows 用 volume/file index；`SyncEngine` 保存 `canonicalKey_` 并在析构/失败路径 release。 |
| G-08 Outbox 原子发布、Inbox scan、ledger 幂等消费 | **部分** | `OutboxWriter` 基本实现 `tmp→fsync→rename→.ready→dir fsync`；`InboxWatcher::scan()` 和 `InboxLedger` 有 seen/consumed/corrupt。但 ACK 路由错误、corrupt 未进 quarantineDir、ledger 不校验同名内容漂移。 |

## D. 上轮 I-01~I-20 修复验收

| 编号 | 验收状态 | 一句依据 |
|---|---|---|
| I-01 | 部分收敛 | 写连接确实在 `SyncWorker::run()` 创建，但 `submitImportSync()` 又通过 `DataBridge::runImportOnDb()` 在 worker 线程触碰主线程 `db_`。 |
| I-02 | 已收敛 | `wconn` 和 `sqlite3* h` 都在 worker 线程内创建，`wconnPtr_/hPtr_` 仅指向 run 生命周期内对象。 |
| I-03 | 部分收敛 | changeset 路径 post-apply 更新 winner；但本地写/导入/selection push 不更新 winner。 |
| I-04 | 部分收敛 | `BatchTransfer` 有同步 context 路由，但 xlsxPath/profileName 复用、lookup 会创建空 context，且导入任务线程安全未收口。 |
| I-05 | 已收敛 | `syncSelected()` 捕获 selection by value，避免悬垂引用；但功能仍是占位。 |
| I-06 | 已收敛 | `ChangesetApplier` 冲突回调和 post-apply 均通过 `sqlite3changeset_pk()` 构造 PK 材料。 |
| I-07 | 部分收敛 | A 分支 `apply_v2 → applied_vector → table_state → appendForward` 顺序存在；但 table_state 失败被吞，winner 仅在 applier 内更新，非 changeset 写路径缺口仍在。 |
| I-08 | 部分收敛 | `UpsertExecutor` 存在并被 branchBC 调用；但 selection push 未填 pkColumns，导入路径未走 branchBC。 |
| I-09 | 部分收敛 | `E_SYNC_GAP` 已映射，但触发时机过早，缺少 pending 超时和 baseline fallback。 |
| I-10 | 已收敛 | `InboxWatcher` 改为同步 `scan()`，worker 主循环直接调用，无需 Qt event loop。 |
| I-11 | 已收敛 | `OutboxWriter::writeAtomic()` 已去掉 remove-final 窗口，包含 tmp fsync、rename、ready fsync、目录 fsync。 |
| I-12 | 部分收敛 | registry key/release 使用同一 canonicalKey；但 sync 激活后的旧写边界未拦截，仍可能绕过唯一写者。 |
| I-13 | 部分收敛 | Identical 三元组已实现；但 `readState()` found 语义错误影响缺状态判断。 |
| I-14 | 未收敛 | `sync()` 入队空任务后立即 `Completed`，没有等待广播和 ACK。 |
| I-15 | 未收敛 | ACK 文件名写死 `self`，广播读取 origin 错位，ACK 不能推进真实 peer 锚点。 |
| I-16 | 部分收敛 | `RebaseEngine` 调用链完整并在广播前接入；但缓存淘汰不按最旧，且广播路由错误会掩盖 rebase 效果。 |
| I-17 | 部分收敛 | eligibility 校验已在 worker 初始化执行；但错误 ring code 折叠为 `E_SYNC_INIT`，不利于 DoD 断言。 |
| I-18 | 部分收敛 | `createSyncEngine`、`createBatchTransfer`、`createComparisonSession` 都存在；但 comparison session save 使用只读连接且不走写队列。 |
| I-19 | 未收敛 | `E_SYNC_ACK_TIMEOUT` 有检测代码，但 ACK 等待数据竞争、完成条件错误、清除条件不是 typed ACK。 |
| I-20 | 部分收敛 | v0.5 错误码常量齐全，baseline/rebase/gap/ack 有部分触发；`E_SYNC_WRITE_BLOCKED`、`E_SYNC_PUSH_SCHEMA_MOVED`、结构化 `E_SYNC_UNSUPPORTED_SCHEMA` 仍未完整覆盖。 |

## E. 亮点

- `SyncWorker::run()` 将 `QSqlDatabase` 和 `sqlite3*` 的创建收口在 worker 线程，是线程模型修复的正确方向。
- `ChangesetApplier` 使用 `sqlite3changeset_apply_v2()` 并保存 rebase buffer，`RebaseEngine` 的 `create→configure→rebase→delete` 链路完整。
- `CapturedWriteTemplate` A 分支已经符合“入站 changeset 不 fresh capture、保存原 blob”的核心要求。
- `SchemaEligibility` 覆盖了表存在性、视图、虚表、shadow table、显式 PK、PK not null 等关键拒绝条件。
- `TableStateStore` 使用模加聚合维护 `content_checksum`，不是 XOR；`DiffEngine` 的 Identical 判定已从 high_water 转为三元组。
- `OutboxWriter` 的原子发布顺序比上一轮可靠，包含文件和目录 fsync。
- `InboxLedger` 的 `seen/consumed/corrupt` 状态为缺口 pending 和幂等消费提供了基础骨架。
