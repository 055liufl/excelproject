# 同步引擎实现深度评审

评审范围：`src/sync/`、`src/batch/`、`include/dbridge/sync/`、`include/dbridge/IBatchTransfer.h`  
对照文档：`specs/SQLite-同步工具-设计文档.md` v0.5、`specs/SQLite-同步工具-plan.md` v0.5

## A. 总体结论

实现与设计吻合程度：**低**。

当前代码已经搭出不少类型、DDL、门面和模块名，但大量关键路径仍是占位或“形似实现”。最核心的问题是：`SyncContext` 没有实际创建/打开写连接，`QSqlDatabase/sqlite3*` 线程归属模型不成立，G-01 行级胜者状态没有按所有成功写入维护，批量门面仍直接跨线程调用旧 `DataBridge` 写连接，选择推送入口存在异步引用生命周期问题且未实现。基于这些问题，代码**不可进入阶段 0 硬验收**，也无法支撑 M1 DoD 的关键断言。

Critical 问题数：**5**。High 问题数：**11**。Medium 问题数：**4**。

## B. 发现清单

| 编号 | 严重级别 | 位置(文件:行) | 类别 | 问题 | 修复建议 |
|---|---|---|---|---|---|
| I-01 | Critical | `src/sync/SyncContext.cpp:68`; `src/sync/SyncEngine.cpp:62` | 线程/连接模型 | `SyncContextRegistry::getOrCreate()` 只创建 `SyncContext`，没有 `addDatabase`、`setDatabaseName`、`open` 写连接；`initialize()` 随后直接从 `ctx_->wconn` 取 handle、建 DDL、跑 eligibility。实际 `wconn` 是默认空连接，同步引擎入口即不可运行。 | 在 registry 或 worker 初始化中按 dev/inode 唯一上下文创建独立写连接，设置 `sqlitePath`、WAL/busy/foreign_keys，打开成功后再执行 handle/session/DDL/eligibility。 |
| I-02 | Critical | `src/sync/SyncEngine.cpp:62`; `src/sync/SyncEngine.cpp:105`; `src/sync/SyncWorker.h:75`; `src/sync/SyncWorker.cpp:64` | 线程安全 | 设计要求 `wconn` 在 `SyncWorker` 线程创建并由该线程独占；实现是在 `initialize()` 调用线程取 `QSqlDatabase&` 和 `sqlite3*`，再把引用/裸指针交给 `QThread::run()` 使用。即便补上 open，也会违反 Qt SQL 线程归属和 `sqlite3*` 连接归属。 | 改为 `SyncWorker::run()` 内部创建连接并取 `sqlite3*`；跨线程只传 `SyncConfig`、任务值对象和快照，不传 `QSqlDatabase`/`sqlite3*`。主线程只接收加锁快照。 |
| I-03 | Critical | `src/sync/apply/ChangesetApplier.cpp:63`; `src/sync/apply/ChangesetApplier.cpp:120`; `src/sync/apply/CapturedWriteTemplate.cpp:90` | G-01 冲突裁决 | `__sync_row_winner` 只在 apply_v2 冲突回调内读取/写入。一个高 rank changeset 若无 SQLite 冲突成功落库，不会写 winner；之后低 rank 跨批到达并触发冲突时读取不到 incumbent，会被当成胜者 `REPLACE`。这正破坏“高 rank 先到提交、低 rank 后到不覆盖”的核心夹具。 | 从 changeset 迭代中提取每行 mutation，在同事务内对所有实际成功写入的行更新 `RowWinnerStore`；冲突回调必须读取已持久化 winner，并且非冲突成功路径也要写 winner。 |
| I-04 | Critical | `src/batch/BatchTransfer.cpp:47`; `src/batch/BatchTransfer.cpp:104`; `src/DataBridge.cpp:205` | 线程安全/写边界 | `BatchTransfer` 在 `QtConcurrent` 后台线程直接调用 `bridge_.importExcel()`，而 `DataBridge::importExcel()` 使用主对象持有的 `db_` 写连接；这既跨线程使用 `QSqlDatabase`，也绕过 `SyncWorker`、`CapturedWriteTemplate`、session 捕获和 table_state 维护。 | 批量导入必须入 `SyncWorker` 写队列，在 worker 自有 `wconn` 上通过 `CapturedWriteTemplate` + `UpsertExecutor/ImportService` 执行；同步激活时旧 `DataBridge::importExcel` 对同步表要拒绝或改道。 |
| I-05 | Critical | `src/sync/SyncEngine.cpp:189`; `src/sync/SyncEngine.cpp:201` | API 正确性/线程安全 | `syncSelected()` 将 `selection` 和局部变量 `success` 以引用捕获进异步队列；函数返回后引用悬空。同时 lambda 内只是 `Q_UNUSED` 占位，没有 SelectionResolver/FK/ChunkStreamer/OutboxWriter。 | 捕获 `SyncSelection` 值对象；结果通过 future/队列回传或状态快照更新。实现完整选择推送流程，或在未实现前返回明确错误而不是投递空任务。 |
| I-06 | High | `src/sync/apply/ChangesetApplier.cpp:88`; `src/sync/apply/ChangesetApplier.cpp:110` | G-01 主键标识 | 冲突回调用所有 new values 拼接生成 `pkHash`，没有调用 `sqlite3changeset_pk()` 取得 PK mask，也没有按 `RowWinnerStore::pkHash(QVariantMap)` 的规范生成同一 key。不同路径会写入/查询不一致的 winner key。 | 使用 `sqlite3changeset_pk()` 识别 PK 列，按表 PK 列名和值构造规范 `QVariantMap`，统一调用 `RowWinnerStore::pkHash()`。DELETE/DATA 冲突要从 old/new 中取可用 PK 值。 |
| I-07 | High | `src/sync/apply/CapturedWriteTemplate.cpp:101`; `src/sync/apply/CapturedWriteTemplate.cpp:109` | G-03/G-06 | 分支 A apply 后只推进 applied_vector 和 appendForward 原 blob，没有调用 `RowWinner.applyMutations`、`TableState.applyMutations`。`WriteResult::tableMutations` 也从未填充。M1 要求业务、changelog、applied_vector、row_winner、table_state 同事务一致，这里缺两项。 | apply_v2 前后解析 changeset 或使用 session/preupdate 信息生成 `TableMutation` 和 `RowWinner` 更新列表，在同一 `WriteTxn` 内写入。失败必须整体 rollback。 |
| I-08 | High | `src/sync/apply/CapturedWriteTemplate.cpp:134`; `src/sync/apply/CapturedWriteTemplate.cpp:175`; `src/sync/apply/CapturedWriteTemplate.cpp:241`; `src/sync/apply/SelectionPushApplier.cpp:23` | G-03/DRY | 分支 B/C 没有使用 `UpsertExecutor`，而是在模板里手写 UPSERT SQL；也没有更新 table_state。`SelectionPushApplier` 持有 `UpsertExecutor` 但显式 `Q_UNUSED(upsert_)`。 | 将 B/C 的行写统一委托给 `UpsertExecutor`，模板只负责事务、session、state/changelog 包装；每条 mutation 产生 table_state 增量。 |
| I-09 | High | `src/sync/apply/AppliedVectorStore.cpp:20`; `src/sync/apply/CapturedWriteTemplate.cpp:75`; `src/sync/SyncWorker.cpp:247` | G-05 错误落点 | `AppliedVectorStore` 有 `seq==applied_seq+1` 检查，但缺口返回模板私有 `"SEQ_GAP"`，worker 只是按普通 apply 错误发出；没有 `E_SYNC_GAP`、gapTimeout、基线回退触发。ledger `seen` 可被保留，但缺口状态不可观测、不可收口。 | 将 Gap 映射为 `err::E_SYNC_GAP`，保留 ledger `seen` 且不 ACK；增加首次 seen 时间/重判次数或超时策略，超限触发 baseline。 |
| I-10 | High | `src/sync/transport/InboxWatcher.cpp:21`; `src/sync/SyncWorker.cpp:104`; `src/sync/SyncWorker.cpp:114` | G-08 三时机扫描 | `InboxWatcher` 依赖 `QFileSystemWatcher` 和 `QTimer`，但 `SyncWorker::run()` 没有启动 Qt 事件循环；`QTimer` timeout 和 watcher 信号不会正常驱动。实际只剩启动扫描和 worker wait 后的轮询，不满足 watcher/timer 三时机契约。 | 不要在无事件循环的 `QThread::run()` 里依赖 QObject 定时器；要么调用 `exec()` 并用 queued tasks，要么改成显式轮询扫描器，并把三时机语义落成可测逻辑。 |
| I-11 | High | `src/sync/transport/OutboxWriter.cpp:58`; `src/sync/transport/OutboxWriter.cpp:65`; `src/sync/transport/OutboxWriter.cpp:75` | G-08 原子发布 | `OutboxWriter` 在 rename 前 `remove(finalPath)`，会制造目标文件消失窗口；未检查 `fsync()` 返回值，未 fsync 目录，`.ready` 也未 flush/fsync。设计要求 tmp→fsync→rename→.ready 的可靠发布语义未完整落地。 | 用同目录临时文件，检查 write/flush/fsync 结果；POSIX 下 rename 直接覆盖或使用唯一 artifact 名避免覆盖；rename 后 fsync 目录；`.ready` 写入也要 flush/fsync 并再 fsync 目录。 |
| I-12 | High | `src/sync/SyncEngine.cpp:57`; `src/sync/SyncEngine.cpp:75`; `src/sync/SyncContext.cpp:21`; `src/sync/SyncContext.cpp:75` | G-07 Context key | registry 存储 key 是 `canonicalKey()` 的 dev/inode，但 `SyncEngine` 保存 release key 时直接用路径字符串，析构 `release()` 找不到 registry 项，refcount/连接生命周期泄漏。实现也没有按设计通过 `PRAGMA database_list` 的主库路径反查真实文件标识。 | `getOrCreate()` 返回 context 和 canonical key，engine 持有同一个 key；初始化后用打开连接的主库路径计算 OS 文件标识，并处理 URI/别名/未建库临时 key 到真实 key 的迁移。 |
| I-13 | High | `src/sync/diff/DiffEngine.cpp:25`; `src/sync/diff/DiffEngine.cpp:30`; `src/sync/diff/DiffEngine.cpp:39` | G-06 表级判等 | `TableStateStore::readState()` 即使没有状态行也返回 true，所以 `localFound` 不能表示存在；判等只比较 checksum，忽略 `schema_fingerprint` 和 `row_count`。设计要求 Identical = schema_fingerprint + row_count + content_checksum 三元组相等，且 high_water 不参与。 | 让 `readState()` 显式返回 found 或增加 exists API；判等条件改为三元组同时相等。 |
| I-14 | High | `src/sync/SyncEngine.cpp:118`; `src/sync/SyncEngine.cpp:130`; `src/sync/SyncEngine.cpp:139` | 状态机/ACK DoD | `sync()` 投递一个空 lambda 后立即释放 gate，并直接把状态/result 置 Completed；没有等待 scan/broadcast 结果，也没有 ACK 等待、ACK 超时或错误传播。 | `sync()` 应启动真实前台操作状态机：触发扫描/打包，进入等待 ACK 状态，按 ACK 前移 result，超时发 `E_SYNC_ACK_TIMEOUT`。 |
| I-15 | High | `src/sync/anchor/OutboundAckStore.cpp:58`; `src/sync/SyncWorker.cpp:308`; `src/sync/SyncWorker.cpp:365` | ACK/广播算法 | ACK 制品处理把 `peer` 和 `origin` 都写成 `csAck.origin`，没有从 artifact 或 header 得到 ACK 发送方；`computePeerAckedSeq(peer)` 完全忽略 peer，返回本地 origin 的全 peer 最小 ACK。广播 `readRange(wconn_, peer, peerAckedSeq)` 又按 `origin = peer` 查 changelog，导致本地变更不会正确发给 peer。 | ACK schema/文件名需包含 from/to；`__sync_outbound_ack(peer, origin, epoch)` 要按接收 ACK 的 peer 更新；广播按“目标 peer 的 acked_seq”读取本地/待转发 changelog，而不是按 origin=peer 读取。 |
| I-16 | High | `src/sync/conflict/RebaseEngine.cpp:7`; `src/sync/SyncWorker.cpp:34`; `src/sync/SyncWorker.cpp:322` | Rebase/G-09 | `RebaseEngine` 的 sqlite3rebaser API 调用形状基本存在，但没有被冲突仲裁/广播路径使用；`E_SYNC_REBASE_FAILED` 没有任何触发点。 | 在 ConflictArbiter/广播阶段接入 apply_v2 产生的 rebase buffer，失败时回滚本轮外发并记录 `E_SYNC_REBASE_FAILED`。 |
| I-17 | High | `src/sync/schema/SchemaEligibility.cpp:151` | G-04 eligibility | 单列 PK 被一律当成隐式 NOT NULL；但 SQLite rowid 表里只有 `INTEGER PRIMARY KEY` 是 rowid alias，普通 `TEXT PRIMARY KEY` 若未声明 NOT NULL 仍可能允许 NULL。实现会误放行不满足“显式非空 PK”的表。 | introspect 时读取列类型，只对单列 `INTEGER PRIMARY KEY` 例外放行；其他 PK 列必须 `notnull=1` 或表为 `WITHOUT ROWID` 等可证明非空的形式。 |
| I-18 | Medium | `include/dbridge/sync/IComparisonSession.h:56`; `src/sync/diff/ComparisonSession.cpp:15` | 接口契约 | `createComparisonSession()` 在头文件声明了工厂，但实现目录没有定义。链接公共 API 时会缺符号。 | 增加工厂实现，负责创建只读/写连接、TableStateStore、DiffEngine、Gate、UpsertExecutor，并返回初始化后的 session 或明确错误。 |
| I-19 | Medium | `include/dbridge/Errors.h:72`; `include/dbridge/Errors.h:73`; `include/dbridge/Errors.h:74`; `src/sync/SyncEngine.cpp:100` | 错误处理 | v0.5 新码中只有 `E_SYNC_UNSUPPORTED_SCHEMA` 被使用；`E_SYNC_ACK_TIMEOUT`、`E_SYNC_REBASE_FAILED`、`E_SYNC_BASELINE_FAILED`、`E_SYNC_GAP` 均只有常量定义，没有业务触发点。 | 按 plan §5 建立每个错误码的触发点和测试：ACK 状态机、rebase 接入、baseline apply/export 回滚、gap timeout。 |
| I-20 | Medium | `src/sync/baseline/BaselineManager.cpp:197`; `src/sync/baseline/BaselineManager.cpp:207`; `src/sync/baseline/BaselineManager.cpp:221` | Baseline/G-09 | BaselineManager 有 apply/reset 基本流程，但失败只返回 false/string，没有映射 `E_SYNC_BASELINE_FAILED`，且没有接入缺口/迁移回退路径。 | Baseline 调用方应统一记录 `E_SYNC_BASELINE_FAILED`，失败时保持旧锚点；从 gap/migration/schema moved 路径接入 baseline manager。 |

## C. 设计不变量核查表

| 不变量 | 状态 | 依据 |
|---|---|---|
| G-01 `__sync_row_winner` + `(rank,seq)` 极大元冲突裁决 | **部分落地** | DDL 和 `RowWinnerStore::beats()` 存在，冲突回调会查 winner；但 winner 只在冲突时写，非冲突成功 apply 不写，且 pkHash 生成不按 PK mask，不能保证跨批到达序无关。 |
| G-03 CapturedWriteTemplate 三分支 | **部分落地** | `WriteKind` 和 A/B/C 形状存在；A 不 fresh 捕获并 appendForward 原 blob是正确方向，但 A 缺 row_winner/table_state 更新；B/C 不使用 UpsertExecutor，缺 table_state，C/local seq 也未形成完整捕获契约。 |
| G-04 initialize eligibility 校验 | **部分落地** | `SyncEngine::initialize()` 调用了 `SchemaEligibility::verify()`，但调用发生在空/未正确归属的 `wconn` 上；eligibility 对单列非 INTEGER PK 的 NOT NULL 判断也会误放行。 |
| G-05 入站严格连续 + 缺口 pending 复用 inbox+ledger seen | **部分落地** | `AppliedVectorStore::check()` 实现了 seq 连续判断；worker 在失败时不会 consumed/ACK，ledger 可保持 seen。但错误码、gap timeout、baseline 回退和明确 pending 状态都未落地。 |
| G-06 表级判等三元组且 high_water 不参与 | **未落地** | `DiffEngine` 只比较 checksum，不比较 schema_fingerprint 和 row_count；`readState()` 也无法区分未找到。 |
| G-07 SyncContext key = OS dev+inode | **部分落地** | `canonicalKey()` 用 `stat()`/Windows file index 生成 key；但 context 不创建连接，release 使用路径字符串导致 registry 泄漏，也未从实际打开主库路径反查。 |
| G-08 TransportAdapter 原子发布 + inbox ledger + 三时机扫描 | **部分落地** | Outbox 有 tmp/fsync/rename/.ready 形状，ledger 有 seen/consumed/corrupt，watcher 有 startup/watcher/timer 形状；但 rename 前 remove、目录/.ready 未 fsync，watcher/timer 在无事件循环 worker 中不可靠。 |

## D. 亮点

- `ISyncEngine` 的 8+1 方法和 `IBatchTransfer` 的 8+3 方法在头文件和具体类中基本齐全，getter 使用互斥锁返回值快照。
- `AppliedVectorStore::check()` 的核心连续性判断写得清晰，`seq<=applied_seq`、`seq==applied_seq+1`、`seq>applied_seq+1` 三类区分正确，可作为 G-05 的基础。
- `RowWinnerStore::beats()` 按 `rank DESC, seq DESC` 判断，局部逻辑符合 G-01 的比较规则。
- `RebaseEngine` 对 `sqlite3rebaser_create/configure/rebase/delete` 的调用顺序基本正确，后续主要问题是接入和错误码落点。
- `FkClosureBuilder` 使用 BFS 扩展依赖并用 Kahn 拓扑排序检测环，算法骨架符合设计方向。
