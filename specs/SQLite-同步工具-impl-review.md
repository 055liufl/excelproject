# SQLite 同步工具实现评审报告（第十一轮）

评审日期：2026-06-26

## 总体评分

总分：68/100。

各维度子分：

- 合规性：67/100。同步主干、Excel 导入导出、OpenSpec 五个特性都有实现，但 baseline 应用、ACK/进度语义、直接导入兼容性、ComparisonSession 语义仍与设计文档存在明显偏差。
- 正确性：63/100。向量时钟连续性、sqlite session 捕获、FK 闭包、反向查找、时间格式等路径可运行；但 row winner 内容序列化、baseline FK 关闭、snapshot pinning、stageCell 合并、行级/route 级错误粒度存在确认 bug。
- 安全性：74/100。大部分 SQL 值使用绑定参数，标识符 quoting 已普遍使用；仍有少量动态标识符拼接绕过 `quoteIdent()`，以及 profile raw SQL 的可信边界未明确。
- 架构质量：70/100。模块边界总体成形，store/apply/transport/profile/mapping 拆分清晰；`SyncWorker` 和 `ExportService` 仍过于集中，部分协议状态复用了不合适的数据表。
- 边界处理：61/100。已有 quarantine、gap timeout、schema guard、ACK timeout、chunk 顺序、单行超预算等处理；但复合主键、BLOB/大整数、baseline FK、stale ledger、跨平台 outbox durability 等边界仍不足。

本轮已按要求读取全部 15 份设计/OpenSpec 文档，并读取 `src/` 下 122 个 `.cpp/.h` 源文件（共 15,822 行）。评审结论以下列文档为基准：

- `specs/Qt-SQLite-Excel-批量导入导出-设计文档.md`
- `specs/MVP-Qt-SQLite-Excel-批量导入导出-实现设计.md`
- `specs/SQLite-同步工具-设计文档.md`
- `specs/SQLite-同步工具-plan.md`
- `openspec/specs/fk-injection/spec.md`
- `openspec/specs/row-lookup/spec.md`
- `openspec/specs/export-column-order/spec.md`
- `openspec/specs/export-reverse-lookup/spec.md`
- `openspec/specs/time-format/spec.md`
- `openspec/changes/archive/2026-05-18-add-row-lookup-and-multi-fk-inject/design.md`
- `openspec/changes/archive/2026-05-18-add-row-lookup-and-multi-fk-inject/proposal.md`
- `openspec/changes/archive/2026-05-21-add-export-column-order/design.md`
- `openspec/changes/archive/2026-05-21-add-export-reverse-lookup/design.md`
- `openspec/changes/archive/2026-05-21-add-time-explicit-type/design.md`
- `openspec/changes/archive/2026-05-21-add-time-format-profile/design.md`

## 第一部分：功能合规性评审

### 1.1 同步引擎核心（SyncEngine / SyncWorker）

- 已实现：单写线程和 worker 写入入口基本符合设计。`SyncEngine` 建立 `SyncWorker` 后将 import/export/capture write 绑定到 `SyncContext`，避免主线程直接跨线程使用 worker 连接，见 `src/sync/SyncEngine.cpp:123`、`src/sync/SyncEngine.cpp:128`、`src/sync/SyncEngine.cpp:134`。
- 已实现：本地导入经 worker 线程执行，使用 `BEGIN IMMEDIATE`、sqlite session、导入函数、`sealInto()`、commit 的顺序，见 `src/sync/SyncWorker.cpp:1255`、`src/sync/SyncWorker.cpp:1262`、`src/sync/SyncWorker.cpp:1277`、`src/sync/SyncWorker.cpp:1286`、`src/sync/SyncWorker.cpp:1300`。
- 已实现：`CapturedWriteTemplate` 区分 inbound changeset 与 local/import/selection push 写入路径，inbound changeset 走 `branchA()`，selection/local 走 `branchBC()`，见 `src/sync/apply/CapturedWriteTemplate.cpp:47`、`src/sync/apply/CapturedWriteTemplate.cpp:64`、`src/sync/apply/CapturedWriteTemplate.cpp:166`。
- 已实现：gap fallback 已能发送 `BaselineRequest`，并在 baseline response 后 apply baseline、移除 in-flight、重新扫描 inbox，见 `src/sync/SyncWorker.cpp:1192`、`src/sync/SyncWorker.cpp:1202`、`src/sync/SyncWorker.cpp:1223`、`src/sync/SyncWorker.cpp:1242`、`src/sync/SyncWorker.cpp:852`、`src/sync/SyncWorker.cpp:860`、`src/sync/SyncWorker.cpp:863`。
- 已实现：前台 ACK 超时路径存在，`ackWaiting_` 超时后发 `E_SYNC_ACK_TIMEOUT`，见 `src/sync/SyncWorker.cpp:397`、`src/sync/SyncWorker.cpp:399`。
- 偏差：`DataBridge::importExcel()` 在 `syncActive_` 时阻断所有直接导入，未按“同步表拒绝、非同步表允许”或“旧 API 重定向到队列”做兼容，见 `src/DataBridge.cpp:181`、`src/DataBridge.cpp:184`。影响是非同步表批量导入也被全局锁死。
- 偏差：普通 changeset ACK 只校验 `toPeer`，任意合法 ACK 到达都会满足当前 `ackWaiting_`，没有绑定本次 foreground sync 的 peer/range/batch，见 `src/sync/SyncWorker.cpp:891`、`src/sync/SyncWorker.cpp:896`、`src/sync/SyncWorker.cpp:907`、`src/sync/SyncWorker.cpp:908`。selection push ACK 已用 `pendingPushId_` 过滤，普通 changeset ACK 缺同等级保护。
- 偏差：gap timeout 硬编码为 30 秒，未使用 `SyncConfig` 中的 gap/peer 参数，见 `src/sync/SyncWorker.cpp:452`、`src/sync/SyncWorker.cpp:455`、`src/sync/SyncWorker.cpp:456`。
- 偏差：`InboxLedger::stalePending()` 对所有 `status='seen'` artifact 触发 stale 检测，不区分 changeset、baseline request/response、ACK、被 inbound gate defer 的 selection push，见 `src/sync/transport/InboxLedger.cpp:93`、`src/sync/transport/InboxLedger.cpp:98`。`processArtifact()` 又在 inbound gate 判断前先 `markSeen()`，见 `src/sync/SyncWorker.cpp:517`、`src/sync/SyncWorker.cpp:518`，可能产生误报 gap。

### 1.2 差量与冲突处理（DiffEngine / ConflictArbiter / RebaseEngine）

- 已实现：表级差量判定同时比较 schema fingerprint、checksum、row count，符合高水位不能作为判等依据的要求，见 `src/sync/diff/DiffEngine.cpp:41`、`src/sync/diff/DiffEngine.cpp:43`、`src/sync/diff/DiffEngine.cpp:44`。
- 已实现：`AppliedVectorStore` 强制连续应用，`seq != applied + 1` 时返回 gap，见 `src/sync/apply/AppliedVectorStore.cpp:20`、`src/sync/apply/AppliedVectorStore.cpp:31`、`src/sync/apply/AppliedVectorStore.cpp:41`。
- 已实现：`ConflictArbiter` 和 `ChangesetApplier` 都采用 rank 优先、rank 相同 originSeq 后写胜出的 LWW 规则，见 `src/sync/conflict/ConflictArbiter.cpp:13`、`src/sync/conflict/ConflictArbiter.cpp:18`、`src/sync/apply/ChangesetApplier.cpp:194`、`src/sync/apply/ChangesetApplier.cpp:196`。
- 已实现：`RebaseEngine` 使用 sqlite3 rebaser API，见 `src/sync/conflict/RebaseEngine.cpp:9`、`src/sync/conflict/RebaseEngine.cpp:17`、`src/sync/conflict/RebaseEngine.cpp:27`。
- 偏差：`DiffEngine::tableDiffs()` 在 row_count 不同时用 `localRowCount / 4` 或 `rm.rowCount / 4` 估算 modified rows，不是实际 diff 结果，见 `src/sync/diff/DiffEngine.cpp:48`、`src/sync/diff/DiffEngine.cpp:51`、`src/sync/diff/DiffEngine.cpp:54`。这会误导 UI 或自动决策。
- 偏差：`ComparisonSession::initialize()` 注释声称 pin read snapshot，但实际只执行一次 `SELECT * FROM sqlite_master LIMIT 0`，没有显式 `BEGIN` 持有读事务，查询结束后快照并不会被长期固定，见 `src/sync/diff/ComparisonSession.cpp:65`、`src/sync/diff/ComparisonSession.cpp:69`。
- 偏差：`ComparisonSession::fetchRemoteRows()` 把 `keysetPageToken` 当 offset hint，而不是按主键 keyset 分页，见 `src/sync/diff/ComparisonSession.cpp:191`、`src/sync/diff/ComparisonSession.cpp:200`、`src/sync/diff/ComparisonSession.cpp:204`。这与设计中的稳定 keyset pagination 不一致。
- 缺失：`ComparisonSession::stageCell()` 没有从 `StagingBuffer` 读取已有 staged row，连续编辑同一行不同列时后一次会基于 local row 覆盖前一次 staged 修改，见 `src/sync/diff/ComparisonSession.cpp:168`、`src/sync/diff/ComparisonSession.cpp:170`、`src/sync/diff/ComparisonSession.cpp:173`、`src/sync/diff/ComparisonSession.cpp:176`。

### 1.3 变更捕获（ChangelogStore / SessionRecorder）

- 已实现：`SessionRecorder` 创建 sqlite session、attach 同步表、收集 changeset、写入 changelog，见 `src/sync/capture/SessionRecorder.cpp:19`、`src/sync/capture/SessionRecorder.cpp:28`、`src/sync/capture/SessionRecorder.cpp:59`、`src/sync/capture/SessionRecorder.cpp:77`。
- 已实现：`ChangelogStore::append()` 使用普通 INSERT，不通过 replace 吞掉重复 `(origin, epoch, origin_seq)`，见 `src/sync/capture/ChangelogStore.cpp:134`、`src/sync/capture/ChangelogStore.cpp:138`、`src/sync/capture/ChangelogStore.cpp:158`。
- 已实现：读取出站范围时排除 peer echo，见 `src/sync/capture/ChangelogStore.cpp:71`、`src/sync/capture/ChangelogStore.cpp:83`。
- 偏差：session 可用性检查只看编译选项 `ENABLE_SESSION` 和 `ENABLE_PREUPDATE_HOOK`，没有执行最小 `sqlite3session_create` smoke test，也未记录 `sqlite3_sourceid()` 级诊断，见 `src/sync/capture/SqliteHandle.cpp:15`、`src/sync/capture/SqliteHandle.cpp:19`、`src/sync/capture/SqliteHandle.cpp:23`。Qt SQLite 插件链接库不一致时定位能力不足。

### 1.4 选择与传输（SelectionResolver / ChunkStreamer / OutboxWriter / InboxWatcher）

- 已实现：`SelectionResolver` 拒绝 raw SQL where，仅支持主键选择并使用绑定参数，见 `src/sync/selection/SelectionResolver.cpp:57`、`src/sync/selection/SelectionResolver.cpp:59`、`src/sync/selection/SelectionResolver.cpp:62`、`src/sync/selection/SelectionResolver.cpp:80`。
- 已实现：`FkClosureBuilder` 能按 child→parent FK 构建依赖闭包、处理缺失依赖和最大规模限制，见 `src/sync/selection/FkClosureBuilder.cpp:85`、`src/sync/selection/FkClosureBuilder.cpp:98`、`src/sync/selection/FkClosureBuilder.cpp:138`、`src/sync/selection/FkClosureBuilder.cpp:228`。
- 已实现：`ChunkStreamer` 支持按预算分块，单行超预算时失败，见 `src/sync/selection/ChunkStreamer.cpp:36`、`src/sync/selection/ChunkStreamer.cpp:64`、`src/sync/selection/ChunkStreamer.cpp:89`。
- 偏差：发送端处理 `PushChunkAck` 时把 ACK 状态写入 `__sync_push_chunk_progress`，接收端应用 chunk 时也写同一张表，见 `src/sync/SyncWorker.cpp:939`、`src/sync/SyncWorker.cpp:942`、`src/sync/SyncWorker.cpp:958`、`src/sync/apply/CapturedWriteTemplate.cpp:344`、`src/sync/apply/CapturedWriteTemplate.cpp:348`、`src/sync/SyncDDL.h:111`。这混淆“本地接收已应用”和“远端 ACK 已到达”两个协议状态。
- 偏差：`OutboxWriter` 无条件包含 POSIX `fcntl.h`/`unistd.h` 并调用 `open/fsync`，和 Qt 跨平台设计冲突，见 `src/sync/transport/OutboxWriter.cpp:8`、`src/sync/transport/OutboxWriter.cpp:9`、`src/sync/transport/OutboxWriter.cpp:113`、`src/sync/transport/OutboxWriter.cpp:114`。
- 偏差：`OutboxWriter` 注释按 POSIX rename replace 语义设计，但实际调用 `QFile::rename(tmpPath, finalPath)`；Qt 在目标存在时通常失败，见 `src/sync/transport/OutboxWriter.cpp:70`、`src/sync/transport/OutboxWriter.cpp:73`。同名重试/重复 artifact 时可能不能实现原子替换。

### 1.5 模式守卫（SchemaGuard / SchemaEligibility）

- 已实现：同步表 eligibility 排除 sqlite 内部表、`__sync_%`、view、virtual/shadow table、无显式 PK、nullable PK，见 `src/sync/schema/SchemaEligibility.cpp:21`、`src/sync/schema/SchemaEligibility.cpp:52`、`src/sync/schema/SchemaEligibility.cpp:56`、`src/sync/schema/SchemaEligibility.cpp:60`、`src/sync/schema/SchemaEligibility.cpp:68`、`src/sync/schema/SchemaEligibility.cpp:72`。
- 已实现：schema fingerprint 包含列名、类型、PK、NOT NULL、默认值、唯一索引名、FK 目标，见 `src/sync/schema/SchemaGuard.cpp:46`、`src/sync/schema/SchemaGuard.cpp:52`、`src/sync/schema/SchemaGuard.cpp:71`、`src/sync/schema/SchemaGuard.cpp:84`。
- 偏差：唯一索引 fingerprint 只记录 index name，未记录 index column list、排序、partial index predicate；同名索引内容变化可能漏检，见 `src/sync/schema/SchemaGuard.cpp:71`、`src/sync/schema/SchemaGuard.cpp:78`、`src/sync/schema/SchemaGuard.cpp:79`。
- 偏差：复合主键表被整体拒绝，见 `src/sync/schema/SchemaEligibility.cpp:76`、`src/sync/schema/SchemaEligibility.cpp:79`。这与当前 selection/apply 实现一致，但低于“有明确 conflict target 即可同步”的最终规格目标。

### 1.6 应用层（ChangesetApplier / UpsertExecutor / SelectionPushApplier）

- 已实现：inbound changeset 通过 `sqlite3changeset_apply_v2()` 应用，且拿到 rebase buffer，见 `src/sync/apply/ChangesetApplier.cpp:111`、`src/sync/apply/ChangesetApplier.cpp:116`、`src/sync/apply/ChangesetApplier.cpp:117`。
- 已实现：`ChangesetApplier` 已把低优先级 DELETE 后的 winner 恢复改为 `ON CONFLICT DO UPDATE` 或 `INSERT OR IGNORE`，避免旧版 `INSERT OR REPLACE` 的级联副作用，见 `src/sync/apply/ChangesetApplier.cpp:303`、`src/sync/apply/ChangesetApplier.cpp:326`、`src/sync/apply/ChangesetApplier.cpp:332`。
- 已实现：`UpsertExecutor` 构造 `INSERT ... ON CONFLICT DO UPDATE` 并统一用 `quoteIdent()`，见 `src/sync/apply/UpsertExecutor.cpp:85`、`src/sync/apply/UpsertExecutor.cpp:86`、`src/sync/apply/UpsertExecutor.cpp:127`。
- 偏差：`ChangesetApplier::updateWinnersFromChangeset()` 把 SQLite INTEGER 存为 JSON double，超过 2^53 的主键或业务整数会丢精度，见 `src/sync/apply/ChangesetApplier.cpp:393`、`src/sync/apply/ChangesetApplier.cpp:394`、`src/sync/apply/ChangesetApplier.cpp:409`。
- 偏差：同一路径把 BLOB 和其他未处理类型写成 JSON null，winner 恢复会丢失二进制列，见 `src/sync/apply/ChangesetApplier.cpp:399`、`src/sync/apply/ChangesetApplier.cpp:400`、`src/sync/apply/ChangesetApplier.cpp:409`。
- 偏差：`RowWinnerStore::pkHash()` 的 canonical material 是 `key=value\n`，而 changeset/selection 路径基于原始 PK 值串接，见 `src/sync/apply/RowWinnerStore.cpp:106`、`src/sync/apply/RowWinnerStore.cpp:111`、`src/sync/apply/ChangesetApplier.cpp:68`、`src/sync/apply/CapturedWriteTemplate.cpp:258`。如果不同入口混用，会查不到同一行 winner。
- 偏差：selection push 构造 `RowMutation` 时分别取 `rowMap.keys()` 和 `rowMap.values()`，依赖 Qt map 顺序一致，见 `src/sync/SyncWorker.cpp:713`、`src/sync/SyncWorker.cpp:716`、`src/sync/SyncWorker.cpp:717`。当前 `QVariantMap` 通常稳定，但应显式按 keys 取 value。

### 1.7 基线管理（BaselineManager）

- 已实现：baseline export 逐表序列化并记录 `sourceMaxSeq`，见 `src/sync/baseline/BaselineManager.cpp:18`、`src/sync/baseline/BaselineManager.cpp:44`、`src/sync/baseline/BaselineManager.cpp:78`、`src/sync/baseline/BaselineManager.cpp:203`。
- 已实现：baseline apply 后重置 applied vector、table state、row winner，并清理 consistency cache，见 `src/sync/baseline/BaselineManager.cpp:267`、`src/sync/baseline/BaselineManager.cpp:269`、`src/sync/baseline/BaselineManager.cpp:275`、`src/sync/baseline/BaselineManager.cpp:283`。
- 偏差：`applyBaseline()` 先 `BEGIN IMMEDIATE`，再执行 `PRAGMA foreign_keys=OFF`；SQLite 中在事务内切换 `foreign_keys` 无效，见 `src/sync/baseline/BaselineManager.cpp:222`、`src/sync/baseline/BaselineManager.cpp:233`、`src/sync/baseline/BaselineManager.cpp:234`。后续 `DELETE`/`INSERT` 仍可能受 FK 约束影响。
- 偏差：baseline 反序列化按 artifact 中表顺序执行 `DELETE` 和 `INSERT`，没有按 FK 拓扑排序，见 `src/sync/baseline/BaselineManager.cpp:115`、`src/sync/baseline/BaselineManager.cpp:128`、`src/sync/baseline/BaselineManager.cpp:141`。与事务内 FK 关闭无效叠加后，会在父子表顺序不利时失败。
- 偏差：`PRAGMA foreign_keys=ON` 也在同一事务内执行，不能作为“提交前验证 FK”的可靠手段，见 `src/sync/baseline/BaselineManager.cpp:253`、`src/sync/baseline/BaselineManager.cpp:256`、`src/sync/baseline/BaselineManager.cpp:257`。

## 第二部分：OpenSpec 特性合规性

### 2.1 FK 注入（FkInjector）

- 已实现：支持多对 `fkInject.pairs`，父 route 缺失时跳过，父值为 NULL 时按严格错误处理，见 `src/mapping/FkInjector.cpp:60`、`src/mapping/FkInjector.cpp:65`、`src/mapping/FkInjector.cpp:74`、`src/mapping/FkInjector.cpp:76`。
- 已实现：子列已有非 NULL 且与父值冲突时报 `E_VALIDATE_FK`，见 `src/mapping/FkInjector.cpp:87`、`src/mapping/FkInjector.cpp:90`、`src/mapping/FkInjector.cpp:92`。
- 偏差：FK/lookup 错误最终被 `ImportService` 汇总成 Excel 行级失败，写入阶段跳过整行，见 `src/service/ImportService.cpp:619`、`src/service/ImportService.cpp:630`。OpenSpec 要求 route 级失败不影响兄弟 route；当前粒度过粗。

### 2.2 行查找（Router / Mapper）

- 已实现：mixed profile discriminator 路由、重复 `matchEquals` 拒绝，见 `src/mapping/Router.cpp:7`、`src/mapping/Router.cpp:14`、`src/mapping/Router.cpp:25`。
- 已实现：lookup 预取按 identity 聚合，跳过 K=0，按 999 变量限制分块，见 `src/service/ImportService.cpp:272`、`src/service/ImportService.cpp:307`、`src/service/ImportService.cpp:323`、`src/service/ImportService.cpp:330`。
- 已实现：lookup key 已改为类型标记 tuple key，避免 `1`、`"1"`、`1.0` 碰撞，见 `src/service/ImportService.cpp:70`、`src/service/ImportService.cpp:73`、`src/service/ExportService.cpp:107`、`src/service/ExportService.cpp:110`。
- 偏差：`castToAffinity()` 对 NUMERIC/DECIMAL/BOOLEAN 等 SQLite NUMERIC affinity 未按 SQLite affinity 规则保留数值语义，最终落到 `raw.toString()`，见 `src/service/ImportService.cpp:43`、`src/service/ImportService.cpp:52`、`src/service/ImportService.cpp:60`、`src/service/ImportService.cpp:67`。reverse lookup 侧也有同样逻辑，见 `src/service/ExportService.cpp:141`、`src/service/ExportService.cpp:150`、`src/service/ExportService.cpp:165`。
- 偏差：`ProfileValidator` 校验 lookup `select.first` 属于 G 表，但没有校验 `select.second` 是否是当前 route 目标表的真实 dbColumn；后续 import upsert 或 export H-col select 才会失败，见 `src/profile/ProfileValidator.cpp:153`、`src/profile/ProfileValidator.cpp:156`、`src/profile/ProfileValidator.cpp:175`、`src/profile/ProfileValidator.cpp:185`。

### 2.3 导出列顺序（ExportService / ExportHelpers）

- 已实现：`columnOrder` 与 raw SQL 互斥，见 `src/profile/ProfileValidator.cpp:418`、`src/profile/ProfileValidator.cpp:420`、`src/profile/ProfileValidator.cpp:422`。
- 已实现：导出前按 columnOrder 重排 header，未列出的 header 保持相对顺序，见 `src/service/ExportService.cpp:747`、`src/service/ExportService.cpp:748`、`src/service/ExportService.cpp:930`、`src/service/ExportService.cpp:931`。
- 偏差：columnOrder 或 reverse lookup 路径需要先 `fetchAllRows()` 再写出，而不是流式写，见 `src/service/ExportService.cpp:819`、`src/service/ExportService.cpp:820`、`src/service/ExportService.cpp:898`、`src/service/ExportService.cpp:909`。大表导出内存峰值不符合批量导出“可控内存”的设计目标。

### 2.4 导出反向查找（ExportService）

- 已实现：反向查找会收集 H 值、批量查询 G 表、构建 cache、输出 A header，见 `src/service/ExportService.cpp:258`、`src/service/ExportService.cpp:310`、`src/service/ExportService.cpp:342`、`src/service/ExportService.cpp:382`、`src/service/ExportService.cpp:938`。
- 已实现：D5 规则“源列非 NULL 优先”已实现，见 `src/service/ExportService.cpp:951`、`src/service/ExportService.cpp:953`、`src/service/ExportService.cpp:956`。
- 偏差：rowSkip 时不递增 `rowCount`，后续错误定位使用 `rowCount + 1`，因此报错行号是输出行位置而非原始结果行位置，见 `src/service/ExportService.cpp:936`、`src/service/ExportService.cpp:939`、`src/service/ExportService.cpp:941`、`src/service/ExportService.cpp:960`。
- 偏差：`needReverseLookup` 在 `explicitSql` 时直接为 false，见 `src/service/ExportService.cpp:797`、`src/service/ExportService.cpp:816`、`src/service/ExportService.cpp:817`。如果 profile 同时配置 lookup 与 raw SQL，反向 lookup 不生效；当前 validator 需要明确禁止或文档化。

### 2.5 时间格式（TemporalConvert / ProfileSpec）

- 已实现：导入时 `Mapper` 只在 effective temporal slot declared 时剥离 legacy `date:*` validator，避免重复解析，见 `src/mapping/Mapper.cpp:23`、`src/mapping/Mapper.cpp:26`、`src/mapping/Mapper.cpp:28`。
- 已实现：导出 DB 值解析失败使用实际输出行号，不再固定为 0，见 `src/service/ExportService.cpp:76`、`src/service/ExportService.cpp:79`、`src/service/ExportService.cpp:80`、`src/service/ExportService.cpp:580`。
- 已实现：profile/column temporal side 采用 side-level overwrite，见 `src/profile/ProfileSpec.h:135`、`src/profile/ProfileSpec.h:168`、`src/profile/ProfileSpec.h:170`、`src/profile/ProfileSpec.h:171`。
- 偏差：`readTemporalSlot()` 对显式 JSON null 直接当未声明处理，见 `src/profile/ProfileLoader.cpp:179`、`src/profile/ProfileLoader.cpp:181`。OpenSpec 要求 format/fallback 等显式 null 被拒绝；如果 slot 本身出现 null，也应明确为非法或在规格中说明。
- 偏差：profile 级 `dateFormat` 不会覆盖仅声明 legacy `date:fmt` validator 的列，见 `src/profile/ProfileSpec.h:160`、`src/profile/ProfileSpec.h:162`、`src/profile/ProfileSpec.h:164`。如果按 time-format profile 设计应允许 profile 默认统一接管 legacy 日期列，则当前实现偏保守。

## 第三部分：Excel 批量导入导出合规性

### 3.1 ImportService

- 已实现：MVP 默认 `abortOnError=true` 时，任何 row-level 错误都会在写库前返回，见 `src/service/ImportService.cpp:592`、`src/service/ImportService.cpp:594`、`src/service/ImportService.cpp:605`。
- 已实现：`abortOnError=false` 支持行级错误跳过并继续写其他行，服务 time-format OpenSpec 的“错误不终止全表”模式，见 `src/service/ImportService.cpp:594`、`src/service/ImportService.cpp:595`、`src/service/ImportService.cpp:619`、`src/service/ImportService.cpp:630`。
- 已实现：写入阶段在单事务内准备 upsert SQL 并绑定参数，见 `src/service/ImportService.cpp:611`、`src/service/ImportService.cpp:612`、`src/service/ImportService.cpp:645`、`src/service/ImportService.cpp:662`。
- 偏差：`Mapper::map()` 即使 route 内校验失败仍 append payload，最终由 `ImportService` 按 Excel row 跳过整行，见 `src/mapping/Mapper.cpp:58`、`src/mapping/Mapper.cpp:123`、`src/service/ImportService.cpp:619`、`src/service/ImportService.cpp:630`。这不满足 OpenSpec 的 route-level 失败隔离。

### 3.2 ExportService

- 已实现：普通流式路径在没有 columnOrder/reverse lookup 时零额外行缓存，见 `src/service/ExportService.cpp:819`、`src/service/ExportService.cpp:820`、`src/service/ExportService.cpp:822`。
- 已实现：mixed/classColumn、columnOrder、reverse lookup、temporal formatting 已集成在导出路径，见 `src/service/ExportService.cpp:740`、`src/service/ExportService.cpp:747`、`src/service/ExportService.cpp:760`、`src/service/ExportService.cpp:779`。
- 偏差：`explicitSql` 直接执行 profile 配置，见 `src/service/ExportService.cpp:797`、`src/service/ExportService.cpp:798`。如果 profile 文件不可信，这是 SQL 执行入口；若 profile 是可信配置，应在 API/文档明确可信边界，并限制为只读单 SELECT。
- 偏差：ExportService 已接近千行，raw SQL、streaming、mixed、reverse lookup、columnOrder、temporal 多路径交织，见 `src/service/ExportService.cpp:622`、`src/service/ExportService.cpp:692`、`src/service/ExportService.cpp:793`、`src/service/ExportService.cpp:898`。回归风险较高。

### 3.3 DataBridge 公共 API

- 已实现：`DataBridge::runImportOnDb()`、`runExportOnDb()` 可供 worker 使用指定连接执行导入导出，见 `src/DataBridge.cpp:225`、`src/DataBridge.cpp:245`。
- 偏差：`DataBridge::setSyncActive()` 只是一个 bool，`importExcel()` 不知道哪些表为 sync table，见 `src/DataBridge.cpp:177`、`src/DataBridge.cpp:181`、`src/DataBridge.cpp:184`。这导致公共 API 兼容性被同步开关全局影响。
- 偏差：同步激活后的旧 `importExcel()` 没有自动转发到 `IBatchTransfer` 或 worker 队列，调用者必须切换 API，见 `src/DataBridge.cpp:183`、`src/DataBridge.cpp:187`。与设计文档中的分阶段兼容目标不完全一致。

## 第四部分：安全与数据完整性

### 4.1 SQL 注入风险

- 风险：`TableStateStore::resetFromBaseline()` 用 `QStringLiteral("SELECT * FROM \"%1\"").arg(tbl)` 拼表名，未统一 `quoteIdent()`，嵌入双引号的表名会破坏 SQL，见 `src/sync/schema/TableStateStore.cpp:115`、`src/sync/schema/TableStateStore.cpp:117`。建议改为 `SELECT * FROM %1`.arg(SqlBuilder::quoteIdent(tbl))。
- 风险：`DiffEngine::getPkColumn()` 用 `PRAGMA table_info(\"%1\")` 拼表名，未转义内嵌双引号，见 `src/sync/diff/DiffEngine.cpp:176`、`src/sync/diff/DiffEngine.cpp:181`。建议使用 `PRAGMA table_info(` + `quoteIdent(table)` + `)`。
- 风险：`ProfileLoader` 以 `isSimpleIdentifier()` 拒绝复杂标识符，见 `src/profile/ProfileLoader.cpp:338`、`src/profile/ProfileLoader.cpp:378`。这降低注入风险，但与已经实现的 `quoteIdent()` 能力不一致；如果设计支持任意 SQLite 合法标识符，应改为加载保留、执行时 quote。
- 风险：`ExportService` 的 `explicitSql` 是 profile 直通 SQL，见 `src/service/ExportService.cpp:797`、`src/service/ExportService.cpp:798`。该入口不是标识符拼接注入，而是配置级 SQL 执行风险；需把 profile 明确定义为可信边界。

### 4.2 事务安全性

- 严重问题：baseline apply 事务内切换 `PRAGMA foreign_keys=OFF/ON` 无效，见 `src/sync/baseline/BaselineManager.cpp:222`、`src/sync/baseline/BaselineManager.cpp:234`、`src/sync/baseline/BaselineManager.cpp:257`。建议在事务外调整，或使用 FK 拓扑排序、deferred constraints、临时禁用策略并用 `PRAGMA foreign_key_check` 验证。
- 风险：baseline 删除/插入不按 FK 拓扑排序，见 `src/sync/baseline/BaselineManager.cpp:115`、`src/sync/baseline/BaselineManager.cpp:128`、`src/sync/baseline/BaselineManager.cpp:141`。即使源 baseline 一致，目标 apply 也可能因父子顺序失败。
- 良好：local import capture 和 changelog 写入在同一 worker transaction 内完成，见 `src/sync/SyncWorker.cpp:1262`、`src/sync/SyncWorker.cpp:1277`、`src/sync/SyncWorker.cpp:1286`、`src/sync/SyncWorker.cpp:1300`。

### 4.3 竞态与线程安全

- 良好：`SyncContext` 以物理数据库 identity 做共享上下文，见 `src/sync/SyncContext.cpp:52`、`src/sync/SyncContext.cpp:61`。
- 风险：普通 changeset ACK 不绑定当前等待批次，可能由旧 ACK/无关 ACK 提前完成前台同步，见 `src/sync/SyncWorker.cpp:903`、`src/sync/SyncWorker.cpp:907`、`src/sync/SyncWorker.cpp:908`。
- 风险：`InboxLedger` 对 deferred artifact 先标 seen，再由 stale scan 当 gap 处理，见 `src/sync/SyncWorker.cpp:517`、`src/sync/SyncWorker.cpp:518`、`src/sync/transport/InboxLedger.cpp:98`。这会把应用层暂停误判为传输缺口。

### 4.4 资源泄漏

- 风险：`OutboxWriter` 在 POSIX 路径上自行 `open/fsync/close`，异常路径目前简单 return；代码短小但跨平台分支缺失，见 `src/sync/transport/OutboxWriter.cpp:113`、`src/sync/transport/OutboxWriter.cpp:114`、`src/sync/transport/OutboxWriter.cpp:117`。
- 良好：`ChangesetApplier` 对 `pRebase` 使用后释放，见 `src/sync/apply/ChangesetApplier.cpp:116`、`src/sync/apply/ChangesetApplier.cpp:119`、`src/sync/apply/ChangesetApplier.cpp:120`。
- 良好：`SessionRecorder::collectChangeset()` 释放 sqlite changeset buffer，见 `src/sync/capture/SessionRecorder.cpp:108`、`src/sync/capture/SessionRecorder.cpp:116`。

## 第五部分：正确性与边界处理

### 5.1 已发现的 Bug

- `src/sync/baseline/BaselineManager.cpp:234` — 在事务内执行 `PRAGMA foreign_keys=OFF` 无效，baseline apply 可能仍受 FK 约束失败 — 将 FK 策略移到事务外，或按 FK 拓扑排序并在提交后执行 `PRAGMA foreign_key_check`。
- `src/sync/apply/ChangesetApplier.cpp:394` — row winner JSON 把 INTEGER 存为 double，大整数恢复会丢精度 — 使用类型标记二进制/CBOR/QDataStream 序列化 row content。
- `src/sync/apply/ChangesetApplier.cpp:400` — row winner JSON 把 BLOB 写成 null，DELETE 恢复会丢列值 — 为 BLOB 使用 base64/hex 类型标记。
- `src/sync/diff/ComparisonSession.cpp:176` — `stageCell()` 不读取已有 staged row，多列编辑会互相覆盖 — 给 `StagingBuffer` 增加 getter 或在 session 维护 staged map。
- `src/sync/diff/ComparisonSession.cpp:69` — read snapshot 未真正 pin 住 — 初始化时显式 `BEGIN` 只读事务，session 结束时 commit/rollback。
- `src/sync/SyncWorker.cpp:908` — changeset ACK 未绑定当前等待批次 — ACK wait tracker 应记录 peer/origin/epoch/fromSeq/toSeq，并按 ACK payload 匹配。
- `src/sync/SyncWorker.cpp:942` — sender-side chunk ACK 写入 receiver-side `__sync_push_chunk_progress` — 拆分 `__sync_push_chunk_ack` 或在表中增加 direction/peer 字段。
- `src/sync/schema/TableStateStore.cpp:117` — 表名拼接未 `quoteIdent()` — 使用统一 SQL builder。
- `src/sync/diff/DiffEngine.cpp:181` — PRAGMA table name 拼接未 `quoteIdent()` — 使用统一 schema helper。
- `src/profile/ProfileValidator.cpp:175` — lookup `select.second` 未校验为 route 表列 — 在 profile validate 阶段查询 route table schema。
- `src/service/ImportService.cpp:630` — route-level 错误变成整行跳过 — 改为按 payload/route 标记失败，仅跳过受影响 route 及其子 route。
- `src/sync/transport/OutboxWriter.cpp:73` — `QFile::rename()` 不保证替换已有目标 — 使用平台原子 replace API，或先生成绝不冲突的 artifact 文件名并禁止覆盖。

### 5.2 边界用例覆盖

- 未充分覆盖：父子表 baseline apply，artifact 表顺序为 parent-first 和 child-first 两种情况，涉及 `src/sync/baseline/BaselineManager.cpp:128`、`src/sync/baseline/BaselineManager.cpp:141`。
- 未充分覆盖：row winner 中含 `qint64` 最大值、BLOB、NULL、REAL、TEXT 混合列，涉及 `src/sync/apply/ChangesetApplier.cpp:393`、`src/sync/apply/ChangesetApplier.cpp:399`。
- 未充分覆盖：同一 `ComparisonSession` 连续 stage 同一行不同列，涉及 `src/sync/diff/ComparisonSession.cpp:168`、`src/sync/diff/ComparisonSession.cpp:176`。
- 未充分覆盖：ACK 重放、旧 ACK、错误 peer ACK 与当前 foreground sync 等待并发，涉及 `src/sync/SyncWorker.cpp:891`、`src/sync/SyncWorker.cpp:907`。
- 未充分覆盖：route 级 lookup/FK 错误只影响该 route，不影响兄弟 route，涉及 `src/mapping/Mapper.cpp:123`、`src/service/ImportService.cpp:630`。
- 未充分覆盖：NUMERIC affinity lookup/reverse lookup，例如 SQLite `NUMERIC` 列中 `1` 与 `"1"` 的等价/不等价策略，涉及 `src/service/ImportService.cpp:67`、`src/service/ExportService.cpp:165`。
- 未充分覆盖：表名/列名包含双引号、关键字、空格时的 sync diff/table state 路径，涉及 `src/sync/schema/TableStateStore.cpp:117`、`src/sync/diff/DiffEngine.cpp:181`。
- 未充分覆盖：Windows 构建下 outbox durability 路径，涉及 `src/sync/transport/OutboxWriter.cpp:8`、`src/sync/transport/OutboxWriter.cpp:113`。

### 5.3 错误传播路径

- 良好：baseline 内部错误统一 prefix 到 `E_SYNC_BASELINE_FAILED`，见 `src/sync/baseline/BaselineManager.cpp:216`、`src/sync/baseline/BaselineManager.cpp:218`。
- 良好：selection push chunk 应用失败会 rollback 并返回错误，不会 ACK 部分成功，见 `src/sync/apply/CapturedWriteTemplate.cpp:296`、`src/sync/apply/CapturedWriteTemplate.cpp:303`、`src/sync/apply/CapturedWriteTemplate.cpp:306`。
- 偏差：sender-side `processAckArtifact()` 写 `__sync_push_chunk_progress` 时忽略 `markQ.exec()` 返回值，见 `src/sync/SyncWorker.cpp:940`、`src/sync/SyncWorker.cpp:952`。ACK 进度写失败时仍可能继续判断完成。
- 偏差：`InboxWatcher::scan()` 的 ledger 操作失败风险未向上冒泡；扫描结果可能和账本状态不一致，相关处理入口见 `src/sync/SyncWorker.cpp:437`、`src/sync/SyncWorker.cpp:439`、`src/sync/SyncWorker.cpp:449`。
- 偏差：`UpsertExecutor::apply()` 对单行 exec 失败只是记录 `rowErrors` 并继续，返回值仍可为 true，见 `src/sync/apply/UpsertExecutor.cpp:56`、`src/sync/apply/UpsertExecutor.cpp:67`、`src/sync/apply/UpsertExecutor.cpp:72`。当前 `CapturedWriteTemplate` 会二次检查，但直接复用该类的调用方容易误判成功。

## 第六部分：架构质量

### 6.1 模块职责与耦合

- 优点：同步代码已经分为 capture/apply/baseline/diff/schema/selection/transport/anchor 等子模块，store 类职责较清晰，例如 `AppliedVectorStore`、`RowWinnerStore`、`InboxLedger`，见 `src/sync/apply/AppliedVectorStore.cpp:12`、`src/sync/apply/RowWinnerStore.cpp:14`、`src/sync/transport/InboxLedger.cpp:20`。
- 问题：`SyncWorker` 同时承担生命周期、任务队列、inbox scan、payload decode、changeset apply、selection push、ACK、baseline、peer eviction，职责过重，相关集中区间见 `src/sync/SyncWorker.cpp:433`、`src/sync/SyncWorker.cpp:469`、`src/sync/SyncWorker.cpp:621`、`src/sync/SyncWorker.cpp:773`、`src/sync/SyncWorker.cpp:831`、`src/sync/SyncWorker.cpp:874`、`src/sync/SyncWorker.cpp:1171`。
- 问题：`ExportService` 聚合了 explicit SQL、auto join、mixed、columnOrder、reverse lookup、temporal，接近单文件“导出总控”，见 `src/service/ExportService.cpp:622`、`src/service/ExportService.cpp:692`、`src/service/ExportService.cpp:793`、`src/service/ExportService.cpp:898`。
- 问题：ACK 状态复用 push chunk progress 表，说明 transport/progress/apply 领域边界没有完全分离，见 `src/sync/SyncWorker.cpp:939`、`src/sync/apply/CapturedWriteTemplate.cpp:344`、`src/sync/SyncDDL.h:111`。

### 6.2 SOLID 原则

- 单一职责：`OutboxWriter`、`AckChannel`、`AppliedVectorStore`、`QuarantineStore` 等小类基本符合，见 `src/sync/transport/OutboxWriter.cpp:13`、`src/sync/transport/AckChannel.cpp:12`、`src/sync/apply/AppliedVectorStore.cpp:12`、`src/sync/schema/QuarantineStore.cpp:12`。
- 开闭原则：OpenSpec 特性通过 `ProfileSpec`、`ProfileLoader`、`ProfileValidator`、`Mapper`、`ExportService` 加入，但 `ExportService` 的分支式扩展已接近上限，见 `src/profile/ProfileSpec.h:88`、`src/profile/ProfileLoader.cpp:570`、`src/service/ExportService.cpp:760`。
- 依赖倒置：`SyncEngine` 直接 new/持有具体 `SyncWorker`，测试时难替换 transport/apply/baseline 协作者，见 `src/sync/SyncEngine.cpp:62`、`src/sync/SyncEngine.cpp:69`。
- 接口隔离：`DataBridge` 暴露 `runImportOnDb()`/`runExportOnDb()` 给内部 worker 使用，公共 API 面偏宽，见 `src/DataBridge.cpp:225`、`src/DataBridge.cpp:245`。

### 6.3 可测试性

- 优点：profile、mapping、SQL builder、store 类大多可用临时 SQLite DB 做单元测试，见 `src/sql/SqlBuilder.cpp:13`、`src/profile/ProfileValidator.cpp:39`、`src/mapping/FkInjector.cpp:18`。
- 缺口：`SyncWorker` 强依赖 QThread/文件系统/SQLite session/transport 目录，端到端测试成本高，见 `src/sync/SyncWorker.cpp:350`、`src/sync/SyncWorker.cpp:433`、`src/sync/SyncWorker.cpp:469`。
- 缺口：baseline FK、ACK replay、row winner typed serialization、ComparisonSession snapshot/staging、route-level import errors 需要专门回归测试，相关问题见 `src/sync/baseline/BaselineManager.cpp:234`、`src/sync/SyncWorker.cpp:908`、`src/sync/apply/ChangesetApplier.cpp:394`、`src/sync/diff/ComparisonSession.cpp:176`、`src/service/ImportService.cpp:630`。

### 6.4 代码重复

- 重复：导入/导出各自实现 lookup identity、tuple key、affinity cast，见 `src/service/ImportService.cpp:31`、`src/service/ImportService.cpp:43`、`src/service/ImportService.cpp:73`、`src/service/ExportService.cpp:96`、`src/service/ExportService.cpp:110`、`src/service/ExportService.cpp:141`。建议抽到 `LookupKeyCodec`/`AffinityCaster`。
- 重复：多处自行 PRAGMA table_info 发现 PK，包括 `src/sync/diff/DiffEngine.cpp:176`、`src/sync/selection/SelectionResolver.cpp:14`、`src/sync/selection/FkClosureBuilder.cpp:31`、`src/sync/SyncWorker.cpp:693`。建议统一到 sync schema helper。
- 重复：temporal export 调用分散在 streaming、mixed、columnOrder/reverse 路径，见 `src/service/ExportService.cpp:580`、`src/service/ExportService.cpp:779`、`src/service/ExportService.cpp:852`、`src/service/ExportService.cpp:961`。建议抽出 row projection pipeline。

## 第七部分：总结与优先级修复建议

### Critical（必须修复）

- [C-1] `src/sync/baseline/BaselineManager.cpp:234` — baseline apply 在事务内关闭 FK，无效且会导致父子表快照应用失败 — 将 FK 策略移出事务，或实现 FK 拓扑删除/插入并用 `PRAGMA foreign_key_check` 验证。
- [C-2] `src/sync/apply/ChangesetApplier.cpp:394` — row winner 把 INTEGER 序列化为 JSON double，大整数恢复会损坏数据 — 改为类型保真序列化格式。
- [C-3] `src/sync/apply/ChangesetApplier.cpp:400` — row winner 把 BLOB/未处理类型序列化为 null，DELETE 恢复会丢数据 — 对所有 SQLite value 类型做带类型标签编码。
- [C-4] `src/sync/diff/ComparisonSession.cpp:176` — 多次 `stageCell()` 会覆盖同一行已有 staged 修改 — 增加 staged row 读取/合并能力。

### High（强烈建议）

- [H-1] `src/sync/SyncWorker.cpp:908` — 普通 changeset ACK 未绑定当前 foreground sync 批次 — 按 peer/origin/epoch/range 关联 ACK wait。
- [H-2] `src/sync/SyncWorker.cpp:942` — 发送端 ACK 进度与接收端 chunk apply 复用 `__sync_push_chunk_progress` — 拆分 ACK 表或增加 direction/peer 维度。
- [H-3] `src/sync/schema/TableStateStore.cpp:117` — 动态表名未 `quoteIdent()` — 改用统一 quoting。
- [H-4] `src/sync/diff/DiffEngine.cpp:181` — PRAGMA 表名未 `quoteIdent()` — 改用统一 schema helper。
- [H-5] `src/profile/ProfileValidator.cpp:175` — lookup 输出目标未校验为 route 表列 — validate 阶段提前失败。
- [H-6] `src/service/ImportService.cpp:630` — route 级错误被提升为整行跳过 — 改为 payload/route 级失败传播。
- [H-7] `src/sync/diff/ComparisonSession.cpp:69` — 没有真正 pin read snapshot — 显式开启并保持只读事务。
- [H-8] `src/sync/transport/OutboxWriter.cpp:8` — POSIX API 无条件使用，Windows 构建不合规 — 增加平台分支或使用 Qt/native abstraction。
- [H-9] `src/DataBridge.cpp:184` — sync active 时阻断所有 direct import — 改为按 sync table 判定或自动转发 worker 队列。

### Medium（建议修复）

- [M-1] `src/sync/SyncWorker.cpp:455` — gap timeout 硬编码 30 秒 — 移入 `SyncConfig`。
- [M-2] `src/sync/transport/InboxLedger.cpp:98` — stale seen 不区分 artifact 类型 — ledger 记录 kind/origin/seq，只对 changeset gap 触发 baseline。
- [M-3] `src/sync/schema/SchemaGuard.cpp:78` — unique index fingerprint 只记录名字 — 纳入 index columns、collation、where predicate。
- [M-4] `src/sync/schema/SchemaEligibility.cpp:79` — 复合 PK 全拒绝 — 文档化为 MVP 限制，或实现复合 PK selection/diff/apply。
- [M-5] `src/service/ImportService.cpp:67` — NUMERIC affinity 被转字符串 — 抽出 SQLite affinity caster 并补 NUMERIC 测试。
- [M-6] `src/service/ExportService.cpp:819` — columnOrder/reverse lookup 路径全量缓存 — 设计可流式投影或分页导出方案。
- [M-7] `src/profile/ProfileLoader.cpp:181` — temporal slot 显式 null 被吞掉 — 按 OpenSpec 明确拒绝或补充规格说明。
- [M-8] `src/sync/apply/RowWinnerStore.cpp:106` — pkHash canonical 与 changeset 路径不一致 — 统一 PK hash codec。
- [M-9] `src/sync/apply/UpsertExecutor.cpp:72` — row exec 失败后 `apply()` 仍可能返回 true — 将错误策略显式化，或失败即返回 false。

### Low（可选优化）

- [L-1] `src/sync/selection/ChunkStreamer.cpp:39` — `PayloadCodec` 参数未使用 — 删除参数或用真实编码大小预算。
- [L-2] `src/sync/selection/ChunkStreamer.cpp:40` — `targetPeer` 参数未使用 — 写入元数据或从接口移除。
- [L-3] `src/sync/SyncWorker.cpp:716` — `keys()`/`values()` 依赖 map 顺序 — 显式按 keys 读取 value。
- [L-4] `src/profile/ProfileLoader.cpp:338` — profile 标识符限制和 `quoteIdent()` 能力不一致 — 明确只支持 simple identifier，或放宽并统一 quote。
- [L-5] `src/service/ExportService.cpp:797` — raw `explicitSql` 可信边界未在实现层限制 — 增加只读 SELECT 校验或文档化 profile 必须可信。

总体结论：当前实现已经具备同步系统的主要骨架，特别是 sqlite session 捕获、changelog、严格 applied vector、baseline request/response、selection push、FK 闭包、OpenSpec 导入导出特性都不是空实现。但仍不能判定为完全合规：baseline FK 事务语义、row winner 类型保真、ComparisonSession 快照/暂存、ACK 与进度状态建模、route 级错误隔离是当前最需要优先修复的核心问题。
