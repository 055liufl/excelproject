# SQLite 同步工具实现评审报告

评审日期：2026-06-26

评审范围：已按要求读取 15 份设计/规格文档，并读取 `src/` 下全部 `.cpp` / `.h` 源码，共 15,265 行。本文以 `specs/SQLite-同步工具-设计文档.md`、`specs/SQLite-同步工具-plan.md`、Excel 批量导入导出设计，以及 5 项 OpenSpec 规格为合规基准。

## 总体评分

总分：**59/100**

各维度子分：

- 合规性：57/100。Excel 导入导出与 OpenSpec 大部分落地，但同步 baseline、ACK 关联、comparison save、selection push 序列语义存在明显偏差。
- 正确性：54/100。存在会导致成功误报、误 ACK、漏同步、分片冲突、差量漏报的确认问题。
- 安全性：66/100。值绑定总体较好，但 schema/同步路径仍有多处动态标识符拼接未统一 `quoteIdent()`。
- 架构质量：61/100。同步核心拆分清晰，但部分路径绕过统一写模板；Baseline/transport 协议未闭环。
- 边界处理：52/100。首次同步、缺口恢复、乱序分片、超大单行、复合主键选择推送等边界不足。

## 第一部分：功能合规性评审

### 1.1 同步引擎核心（SyncEngine / SyncWorker）

- ChangeLog 捕获：**偏差**。`CapturedWriteTemplate::branchA()` 对入站 changeset 会在事务内 apply、advance applied vector、append raw changelog（`src/sync/apply/CapturedWriteTemplate.cpp:52`、`src/sync/apply/CapturedWriteTemplate.cpp:111`、`src/sync/apply/CapturedWriteTemplate.cpp:128`），本地/selection 写也通过 sqlite session seal 到 changelog（`src/sync/apply/CapturedWriteTemplate.cpp:206`、`src/sync/apply/CapturedWriteTemplate.cpp:317`）。但 sync-routed Excel import 自己开启 session 后忽略 `rec_->begin()`、`rec_->sealInto()`、`txn.commit()` 的返回值（`src/sync/SyncWorker.cpp:1144`、`src/sync/SyncWorker.cpp:1160`、`src/sync/SyncWorker.cpp:1162`），会把捕获/提交失败报告为成功。
- 向量时钟：**已实现但局部不适用**。changeset 路径通过 `AppliedVectorStore::check()` 强制首次 seq=1、连续 `applied+1`、旧 seq no-op（`src/sync/apply/AppliedVectorStore.cpp:20`、`src/sync/apply/AppliedVectorStore.cpp:35`、`src/sync/apply/AppliedVectorStore.cpp:37`）。selection push 发送端固定 `originSeq=0`（`src/sync/SyncWorker.cpp:1321`、`src/sync/SyncWorker.cpp:1322`），接收端用这个 seq seal changelog（`src/sync/apply/CapturedWriteTemplate.cpp:303`、`src/sync/apply/CapturedWriteTemplate.cpp:317`），不满足严格 origin 序列语义。
- 差量计算：**偏差**。表级 diff 依赖 `__sync_table_state` checksum/row_count（`src/sync/diff/DiffEngine.cpp:12`、`src/sync/diff/DiffEngine.cpp:43`），但行级 diff 将本地和远端都按相同 offset/limit 分页后比较（`src/sync/diff/DiffEngine.cpp:68`、`src/sync/diff/DiffEngine.cpp:73`），本地查询没有 `ORDER BY`（`src/sync/diff/DiffEngine.cpp:138`），插入/删除导致页边界错位时会漏报或误报。
- 冲突仲裁：**部分实现**。rank + originSeq LWW 比较存在（`src/sync/conflict/ConflictArbiter.cpp:13`、`src/sync/conflict/ConflictArbiter.cpp:18`、`src/sync/conflict/ConflictArbiter.cpp:22`），changeset apply 也在冲突回调中参考 RowWinner（`src/sync/apply/ChangesetApplier.cpp:156`、`src/sync/apply/ChangesetApplier.cpp:194`、`src/sync/apply/ChangesetApplier.cpp:195`）。但非冲突路径的 INSERT/UPDATE 是 post-apply 更新 winner（`src/sync/apply/ChangesetApplier.cpp:126`、`src/sync/apply/ChangesetApplier.cpp:377`），需要更多回归测试证明到达序无关。
- 出站/入站通道：**部分实现**。Outbox 原子写 payload、`.ready` 和 fsync（`src/sync/transport/OutboxWriter.cpp:183`、`src/sync/transport/OutboxWriter.cpp:232`、`src/sync/transport/OutboxWriter.cpp:252`），Inbox 通过 `.ready` 扫描和 ledger 去重（`src/sync/transport/InboxWatcher.cpp:26`、`src/sync/transport/InboxLedger.cpp:71`、`src/sync/SyncWorker.cpp:468`）。但 ACK wait 是全局布尔，任意发给本节点的合法 ACK 都会完成当前前台同步（`src/sync/SyncWorker.cpp:798`、`src/sync/SyncWorker.cpp:859`），没有绑定 payload、peer、pushId 或 originSeq。
- 基线管理：**严重缺失闭环**。`PayloadCodec` 已有 BaselineRequest/BaselineResponse 编解码（`src/sync/payload/PayloadCodec.cpp:84`、`src/sync/payload/PayloadCodec.cpp:107`、`src/sync/payload/PayloadCodec.cpp:207`、`src/sync/payload/PayloadCodec.cpp:226`），`SyncWorker` 也声明并调用 baseline artifact handler（`src/sync/SyncWorker.h:111`、`src/sync/SyncWorker.cpp:529`、`src/sync/SyncWorker.cpp:531`），但仓库内没有 `SyncWorker::processBaselineRequestArtifact()` / `processBaselineResponseArtifact()` 定义。当前缺口超时只隔离并提示需要源端 baseline（`src/sync/SyncWorker.cpp:1074`、`src/sync/SyncWorker.cpp:1079`、`src/sync/SyncWorker.cpp:1095`）。

### 1.2 差量与冲突处理（DiffEngine / ConflictArbiter / RebaseEngine）

- 向量时钟比较逻辑：**已实现**。`AppliedVectorStore::check()` 明确区分 Apply/NoOp/Gap（`src/sync/apply/AppliedVectorStore.cpp:20`、`src/sync/apply/AppliedVectorStore.cpp:31`、`src/sync/apply/AppliedVectorStore.cpp:41`）。
- Last-Write-Wins 语义：**部分实现**。`ConflictArbiter::beats()` 按 rank 优先、seq 次之（`src/sync/conflict/ConflictArbiter.cpp:13`、`src/sync/conflict/ConflictArbiter.cpp:18`、`src/sync/conflict/ConflictArbiter.cpp:22`）；`ChangesetApplier::conflictCb()` 也使用相同规则（`src/sync/apply/ChangesetApplier.cpp:188`、`src/sync/apply/ChangesetApplier.cpp:195`、`src/sync/apply/ChangesetApplier.cpp:201`）。偏差是 `ConflictArbiter` 自身未被主 apply 路径直接注入，实际规则复制在 `ChangesetApplier` 中。
- Rebase 流程：**部分实现**。`sqlite3changeset_apply_v2()` 收集 rebase buffer（`src/sync/apply/ChangesetApplier.cpp:109`、`src/sync/apply/ChangesetApplier.cpp:114`），worker 保存并 LRU 淘汰（`src/sync/SyncWorker.cpp:596`、`src/sync/SyncWorker.cpp:604`），广播前调用 `RebaseEngine::rebase()`（`src/sync/SyncWorker.cpp:920`、`src/sync/conflict/RebaseEngine.cpp:32`）。缺口是 rebase key 仅为 `origin/originSeq`（`src/sync/SyncWorker.cpp:916`），未包含 epoch，跨 epoch 残留风险需要清理策略。
- 行级 diff：**偏差**。本地 `SELECT * FROM "%1"` 未转义标识符且无排序（`src/sync/diff/DiffEngine.cpp:138`），远端行按数组 offset 切片（`src/sync/diff/DiffEngine.cpp:73`），不符合 keyset/PK 对齐的差量语义。

### 1.3 变更捕获（ChangelogStore / SessionRecorder）

- changelog 表结构：**已实现**。`__sync_changelog` 包含 `local_seq`、`kind`、`origin`、`origin_seq`、`parent_seq`、`stream_epoch`、schema 信息、checksum、byte_size，并强制 `UNIQUE(origin, stream_epoch, origin_seq)`（`src/sync/SyncDDL.h:13`、`src/sync/SyncDDL.h:28`）。
- sqlite session 捕获：**已实现但调用不一致**。`SessionRecorder` 通过 `sqlite3session_create`/`attach`/`changeset` 捕获（`src/sync/capture/SessionRecorder.cpp:23`、`src/sync/capture/SessionRecorder.cpp:35`、`src/sync/capture/SessionRecorder.cpp:104`）；`CapturedWriteTemplate` 正确处理失败回滚（`src/sync/apply/CapturedWriteTemplate.cpp:206`、`src/sync/apply/CapturedWriteTemplate.cpp:281`）。`SyncWorker::submitImportSync()` 没有检查 session/seal/commit 失败（`src/sync/SyncWorker.cpp:1144`、`src/sync/SyncWorker.cpp:1160`、`src/sync/SyncWorker.cpp:1162`）。
- session 语义：**偏差**。Comparison save 直接调用 `StagingBuffer::save()` 和 `UpsertExecutor`，没有通过 `CapturedWriteTemplate`，因此接受远端行不会生成 changelog、table_state 和 local origin seq（`src/sync/diff/ComparisonSession.cpp:278`、`src/sync/diff/ComparisonSession.cpp:280`、`src/sync/diff/StagingBuffer.cpp:45`、`src/sync/diff/StagingBuffer.cpp:54`）。

### 1.4 选择与传输（SelectionResolver / ChunkStreamer / OutboxWriter / InboxWatcher）

- FK 闭包构建：**部分实现**。选择行通过 PK resolve（`src/sync/selection/SelectionResolver.cpp:53`、`src/sync/selection/SelectionResolver.cpp:59`），闭包沿外键父依赖 BFS 展开（`src/sync/selection/FkClosureBuilder.cpp:85`、`src/sync/selection/FkClosureBuilder.cpp:98`、`src/sync/selection/FkClosureBuilder.cpp:127`），并拓扑排序（`src/sync/selection/FkClosureBuilder.cpp:138`、`src/sync/selection/FkClosureBuilder.cpp:187`）。但 `getPkColumn()` 只返回 `pk==1` 的单列主键（`src/sync/selection/FkClosureBuilder.cpp:31`、`src/sync/selection/FkClosureBuilder.cpp:43`），复合主键同步表会选择推送失败或闭包错误。
- 分块传输：**偏差**。`ChunkStreamer` 按估算字节拆 chunk（`src/sync/selection/ChunkStreamer.cpp:36`、`src/sync/selection/ChunkStreamer.cpp:58`、`src/sync/selection/ChunkStreamer.cpp:63`），但单行超过 `chunkBudgetBytes` 时仍会进入空 chunk 并成功返回（`src/sync/selection/ChunkStreamer.cpp:61`、`src/sync/selection/ChunkStreamer.cpp:68`），不符合超大选择应报 `E_SYNC_SELECTION_TOO_LARGE`。
- ACK 机制：**偏差**。ACK payload 带 `toPeer`（`src/sync/payload/PayloadCodec.cpp:258`、`src/sync/payload/PayloadCodec.cpp:268`），worker 也过滤非本节点 ACK（`src/sync/SyncWorker.cpp:787`、`src/sync/SyncWorker.cpp:814`），但前台完成条件仍只看 `ackWaiting_`（`src/sync/SyncWorker.cpp:798`、`src/sync/SyncWorker.cpp:859`），没有确认 ACK 对应本次同步。
- FIFO 扫描：**偏差**。`InboxWatcher` 注释称 oldest first（`src/sync/transport/InboxWatcher.cpp:26`），实际使用 `QDir::Time`（`src/sync/transport/InboxWatcher.cpp:28`），Qt 默认通常为新到旧；严格向量会 pending gap，但会增加乱序和误超时概率。

### 1.5 模式守卫（SchemaGuard / SchemaEligibility）

- 表级可同步性：**已实现**。空 syncTables 会扩展为用户表并排除 SQLite/internal sync 表（`src/sync/schema/SchemaEligibility.cpp:14`、`src/sync/schema/SchemaEligibility.cpp:21`）；校验拒绝 view、virtual、shadow、无 PK、nullable PK、无可用 upsert target（`src/sync/schema/SchemaEligibility.cpp:52`、`src/sync/schema/SchemaEligibility.cpp:56`、`src/sync/schema/SchemaEligibility.cpp:60`、`src/sync/schema/SchemaEligibility.cpp:64`、`src/sync/schema/SchemaEligibility.cpp:68`、`src/sync/schema/SchemaEligibility.cpp:72`、`src/sync/schema/SchemaEligibility.cpp:76`）。
- 隔离表处理：**部分实现**。内部 `__sync_*` 表不会进入空表扩展（`src/sync/schema/SchemaEligibility.cpp:21`、`src/sync/schema/SchemaEligibility.cpp:22`），changeset filter 也拒绝 `__sync_` 表（`src/sync/apply/ChangesetApplier.cpp:142`、`src/sync/apply/ChangesetApplier.cpp:147`）。但 `SchemaIntrospector::readTables()` 仍会读入 `__sync_*` 到普通 catalog（`src/schema/SchemaIntrospector.cpp:11`、`src/schema/SchemaIntrospector.cpp:12`），可能影响 profile/导出辅助逻辑。
- SchemaGuard：**已实现**。payload schema version 和 fingerprint 都被验证（`src/sync/schema/SchemaGuard.cpp:21`、`src/sync/schema/SchemaGuard.cpp:28`），worker 初始化时计算本地 fingerprint（`src/sync/SyncWorker.cpp:312`、`src/sync/SyncWorker.cpp:313`）。

### 1.6 应用层（ChangesetApplier / UpsertExecutor / SelectionPushApplier）

- Upsert 语义：**部分实现**。`UpsertExecutor` 能生成 `INSERT OR IGNORE` 或 `ON CONFLICT DO UPDATE`（`src/sync/apply/UpsertExecutor.cpp:115`、`src/sync/apply/UpsertExecutor.cpp:139`），标识符使用 `SqlBuilder::quoteIdent()`（`src/sync/apply/UpsertExecutor.cpp:98`）。风险是 prepared cache key 只有 `(table, mode)`（`src/sync/apply/UpsertExecutor.cpp:33`、`src/sync/apply/UpsertExecutor.cpp:145`），同表不同列集依赖 `lastQuery()` 判定（`src/sync/apply/UpsertExecutor.cpp:47`、`src/sync/apply/UpsertExecutor.cpp:48`），较脆弱。
- 幂等性：**偏差**。selection push 对已 applied chunk 直接 no-op（`src/sync/apply/CapturedWriteTemplate.cpp:178`、`src/sync/apply/CapturedWriteTemplate.cpp:188`），但没有比较重复 chunk 的 checksum；规格要求相同 checksum no-op、不同 checksum 报 `E_SYNC_PAYLOAD_CORRUPT`。
- 事务边界：**已实现但有绕行**。`CapturedWriteTemplate` 的入站和本地路径都使用 `WriteTxn`（`src/sync/apply/CapturedWriteTemplate.cpp:52`、`src/sync/apply/CapturedWriteTemplate.cpp:163`），错误路径 rollback。绕行点是 `ComparisonSession::save()` 和 `SyncWorker::submitImportSync()`（`src/sync/diff/ComparisonSession.cpp:280`、`src/sync/SyncWorker.cpp:1144`）。
- SelectionPushApplier：**基本空壳**。`SelectionPushApplier::apply()` 只是逐行封装并调用 `UpsertExecutor`（`src/sync/apply/SelectionPushApplier.cpp:9`、`src/sync/apply/SelectionPushApplier.cpp:25`、`src/sync/apply/SelectionPushApplier.cpp:45`），主路径实际走 `CapturedWriteTemplate`（`src/sync/SyncWorker.cpp:725`、`src/sync/SyncWorker.cpp:739`），模块职责重复。

### 1.7 基线管理（BaselineManager）

- 基线快照：**部分实现**。可序列化指定表数据并记录 max local_seq（`src/sync/baseline/BaselineManager.cpp:18`、`src/sync/baseline/BaselineManager.cpp:72`、`src/sync/baseline/BaselineManager.cpp:190`）。
- 基线应用：**部分实现**。`applyBaseline()` 在事务中 delete/insert、reset applied_vector、reset table_state、reset row_winner（`src/sync/baseline/BaselineManager.cpp:219`、`src/sync/baseline/BaselineManager.cpp:226`、`src/sync/baseline/BaselineManager.cpp:234`、`src/sync/baseline/BaselineManager.cpp:242`、`src/sync/baseline/BaselineManager.cpp:249`）。
- 增量追踪：**偏差**。`resetFromBaseline()` 传入空 schema fingerprint（`src/sync/baseline/BaselineManager.cpp:240`、`src/sync/baseline/BaselineManager.cpp:242`），后续 `DiffEngine::tableDiffs()` 会把本地 fingerprint 与远端 fingerprint 比较（`src/sync/diff/DiffEngine.cpp:43`），可能制造伪差异。
- 协议闭环：**缺失**。baseline artifact handler 只有声明和调用没有定义（`src/sync/SyncWorker.h:111`、`src/sync/SyncWorker.cpp:529`），当前实现无法完成源端 baseline request/response。

## 第二部分：OpenSpec 特性合规性

### 2.1 FK 注入（FkInjector）

- array-of-groups：**已实现**。loader 明确拒绝旧对象形式，仅接受数组（`src/profile/ProfileLoader.cpp:399`、`src/profile/ProfileLoader.cpp:403`、`src/profile/ProfileLoader.cpp:412`），并解析 `from` 与 `pairs`（`src/profile/ProfileLoader.cpp:418`、`src/profile/ProfileLoader.cpp:435`、`src/profile/ProfileLoader.cpp:451`）。
- 多父/多 pair：**已实现**。`FkInjector::inject()` 遍历每个 `FkInjectSpec` 和每个 pair（`src/mapping/FkInjector.cpp:59`、`src/mapping/FkInjector.cpp:65`），可注入已有列或追加新列（`src/mapping/FkInjector.cpp:87`、`src/mapping/FkInjector.cpp:103`、`src/mapping/FkInjector.cpp:105`）。
- 错误处理：**已实现**。父值 NULL 时添加行级 `E_VALIDATE_FK`（`src/mapping/FkInjector.cpp:74`、`src/mapping/FkInjector.cpp:76`），child 预填值冲突时添加错误（`src/mapping/FkInjector.cpp:90`、`src/mapping/FkInjector.cpp:92`）。
- conflictVals 更新：**已实现**。按 childCol 名称更新 conflictVals（`src/mapping/FkInjector.cpp:109`、`src/mapping/FkInjector.cpp:112`）。
- 级联抑制：**已实现**。lookup 失败集合作为 initialFailed 传入，祖先失败时跳过子注入（`src/service/ImportService.cpp:520`、`src/service/ImportService.cpp:526`、`src/mapping/FkInjector.cpp:37`、`src/mapping/FkInjector.cpp:44`）。
- 风险：同一 profile 中多个 route 指向同一 table 时，`tableToPayloadIdx[payloads[i].table] = i` 会覆盖前者（`src/mapping/FkInjector.cpp:24`、`src/mapping/FkInjector.cpp:26`），多 route 同表场景下 FK 注入会找错 payload。

### 2.2 行查找（Router / Mapper）

- lookup 语法：**已实现**。loader 拒绝 object 形式，要求 `match`/`select` 为数组 pair（`src/profile/ProfileLoader.cpp:511`、`src/profile/ProfileLoader.cpp:521`、`src/profile/ProfileLoader.cpp:540`、`src/profile/ProfileLoader.cpp:550`）。
- batch prefetch：**已实现**。ImportService 构建 lookup cache，单列使用 `IN`，多列使用 OR-of-AND，值全部绑定（`src/service/ImportService.cpp:300`、`src/service/ImportService.cpp:313`、`src/service/ImportService.cpp:319`、`src/service/ImportService.cpp:336`、`src/service/ImportService.cpp:338`）。
- 严格基数：**已实现**。cache hitCount 用于判定 miss/ambiguous（`src/service/ImportService.cpp:363`、`src/service/ImportService.cpp:366`），applyLookups 对 miss/ambiguous 产生日志并失败该行（`src/service/ImportService.cpp:520`）。
- type affinity：**部分实现**。反向 export 有 `castToAffinity()`（`src/service/ExportService.cpp:117`、`src/service/ExportService.cpp:120`、`src/service/ExportService.cpp:125`），import lookup prefetch 主要依赖 SQLite 比较和 QVariant 绑定（`src/service/ImportService.cpp:336`、`src/service/ImportService.cpp:340`），没有显式按 G 列 affinity 归一化 Excel 值。
- 错误聚合：**偏差**。Mapper 遇到一个 validator 错误后 `rowHasError=true`，同 route 后续 temporal conversion 被跳过（`src/mapping/Mapper.cpp:58`、`src/mapping/Mapper.cpp:66`、`src/mapping/Mapper.cpp:78`），可能隐藏同一行其他列的时间解析错误；错误 sheet 传空字符串（`src/mapping/Mapper.cpp:67`、`src/mapping/Mapper.cpp:94`），上下文不完整。

### 2.3 导出列顺序（ExportService / ExportHelpers）

- 稳定排序：**已实现**。`reorderHeaders()` 将 `columnOrder` 中列放前面，未列出的 natural headers 保持相对顺序（`src/service/ExportHelpers.h:7`、`src/service/ExportHelpers.h:13`、`src/service/ExportHelpers.h:15`）。
- raw SQL 互斥：**已实现**。validator 拒绝 `export.columnOrder` 与 `export.sql` 共存（`src/profile/ProfileValidator.cpp:417`、`src/profile/ProfileValidator.cpp:420`、`src/profile/ProfileValidator.cpp:422`）。
- unknown/duplicate：**已实现**。validator 检查重复和未知 header（`src/profile/ProfileValidator.cpp:470`、`src/profile/ProfileValidator.cpp:473`、`src/profile/ProfileValidator.cpp:481`）。
- 反向 lookup 交互：**基本实现**。knownHeaders 会根据 `exportRoundtrip` 加 A header、移除/保留 H 列（`src/profile/ProfileValidator.cpp:428`、`src/profile/ProfileValidator.cpp:439`、`src/profile/ProfileValidator.cpp:443`、`src/profile/ProfileValidator.cpp:445`）。

### 2.4 导出反向查找（ExportService）

- H-cols 扩展：**实现但有性能偏差**。`buildHColSelectSuffix()` 为 lookup.select 加 H 列并 alias 到 dbColumn（`src/service/ExportService.cpp:168`、`src/service/ExportService.cpp:178`、`src/service/ExportService.cpp:179`），当前已避免旧报告中的 alias 缺失问题。
- 预取和替换：**已实现**。收集 H tuple，buildReverseCache，按 `exportOnMissing` 处理 miss/ambiguous（`src/service/ExportService.cpp:855`、`src/service/ExportService.cpp:866`、`src/service/ExportService.cpp:895`、`src/service/ExportService.cpp:901`）。
- exportRoundtrip=false：**偏差**。`needReverseLookup` 使用 `hasAnyLookupHCols()`，即使全部 lookup 都 `exportRoundtrip=false` 也进入 full-load/reverse path（`src/service/ExportService.cpp:152`、`src/service/ExportService.cpp:776`、`src/service/ExportService.cpp:833`），而 `buildReverseCache()` 只处理 roundtrip=true。功能结果通常正确，但不符合“false 跳过反查、H 列原样输出”的轻量路径。
- 行级错误结果：**偏差需产品确认**。export 结束无论 `errors` 是否含 DB 时间解析/反向 lookup 行错误都设置 `ok=true`（`src/service/ExportService.cpp:927`、`src/service/ExportService.cpp:933`、`src/service/ExportService.cpp:935`）。如果规格要求 error 策略阻断导出，这里应返回失败。

### 2.5 时间格式（TemporalConvert / ProfileSpec）

- 显式槽：**已实现**。ProfileSpec 支持 date/datetime/time 三个 slot，side 支持 `string|epochSec`（`src/profile/ProfileSpec.h:13`、`src/profile/ProfileSpec.h:26`、`src/profile/ProfileSpec.h:33`、`src/profile/ProfileSpec.h:47`）。
- profile/column 合并：**已实现**。`effectiveTemporalFor()` 实现列级 side 覆盖 profile side（`src/profile/ProfileSpec.h:137`、`src/profile/ProfileSpec.h:168`、`src/profile/ProfileSpec.h:170`）。
- 解析/格式化：**已实现**。字符串按 primary+fallback 解析（`src/mapping/TemporalConvert.cpp:33`、`src/mapping/TemporalConvert.cpp:40`），epochSec 只对 DateTime 格式化（`src/mapping/TemporalConvert.cpp:71`、`src/mapping/TemporalConvert.cpp:72`），导入失败记录行级错误（`src/mapping/Mapper.cpp:90`、`src/mapping/Mapper.cpp:94`）。
- 时区处理：**需明确**。`QDateTime::fromSecsSinceEpoch()` 使用本地时区默认行为（`src/mapping/TemporalConvert.cpp:117`），如果规格意图“无时区语义/按 UTC 原样秒数”，这里应显式设定 `Qt::UTC` 或文档化本地时区行为。
- 导出 DB parse failure：**已实现**。非 NULL DB 值解析失败写空并记录 `E_TIME_PARSE_DB`（`src/service/ExportService.cpp:63`、`src/service/ExportService.cpp:80`、`src/service/ExportService.cpp:82`、`src/service/ExportService.cpp:86`）。

## 第三部分：Excel 批量导入导出合规性

### 3.1 ImportService

- 行级错误收集：**已实现**。打开文件、sheet、header、profile 校验错误进入 ErrorCollector（`src/service/ImportService.cpp:397`、`src/service/ImportService.cpp:404`、`src/service/ImportService.cpp:410`、`src/service/ImportService.cpp:418`）。行级错误行在写入前收集并跳过（`src/service/ImportService.cpp:586`、`src/service/ImportService.cpp:597`）。
- 事务边界：**已实现**。普通导入用单事务包裹写入（`src/service/ImportService.cpp:578`、`src/service/ImportService.cpp:579`、`src/service/ImportService.cpp:642`、`src/service/ImportService.cpp:653`）。sync routed import 外层 `WriteTxn` 包裹，但错误检查缺失（`src/sync/SyncWorker.cpp:1129`、`src/sync/SyncWorker.cpp:1144`、`src/sync/SyncWorker.cpp:1162`）。
- FK 预检：**已实现**。非 Mixed 和 Mixed 分 class 调用 `ForeignKeyPreflight`（`src/service/ImportService.cpp:538`、`src/service/ImportService.cpp:541`、`src/service/ImportService.cpp:549`）。
- 默认 abort 策略：**偏差但可能是后续规格覆盖**。当前只有 table-level 错误终止整批，row-level 错误仅跳过该行（`src/service/ImportService.cpp:561`、`src/service/ImportService.cpp:572`、`src/service/ImportService.cpp:586`）。这偏离早期 MVP “prevalidate error terminates batch”的默认描述，但更符合 time/lookup OpenSpec 行级错误要求。
- writtenRows 计数：**偏差**。`result.writtenRows++` 每个 Excel 行只加 1（`src/service/ImportService.cpp:596`、`src/service/ImportService.cpp:600`、`src/service/ImportService.cpp:639`），多表 route 写多张表时不是实际 upsert 行数。

### 3.2 ExportService

- 批量导出：**已实现**。无 columnOrder/reverse 时流式写出（`src/service/ExportService.cpp:519`、`src/service/ExportService.cpp:543`、`src/service/ExportService.cpp:779`），有 columnOrder/reverse 时加载并重排/投影（`src/service/ExportService.cpp:787`、`src/service/ExportService.cpp:833`）。
- 列顺序：**已实现**。导出最终 header 使用 `reorderHeaders()`（`src/service/ExportService.cpp:817`、`src/service/ExportService.cpp:887`）。
- 反向 FK/lookup 展开：**已实现但性能偏差**。H 值收集、预取、A header 投影都存在（`src/service/ExportService.cpp:855`、`src/service/ExportService.cpp:866`、`src/service/ExportService.cpp:881`、`src/service/ExportService.cpp:895`）。
- 格式化输出：**已实现**。时间字段在 streaming、columnOrder、reverse 路径均调用 `convertTemporalForExport()`（`src/service/ExportService.cpp:548`、`src/service/ExportService.cpp:809`、`src/service/ExportService.cpp:917`）。

### 3.3 DataBridge 公共 API

- 接口签名合规性：**基本实现**。`open()`、`loadProfile()`、`importExcel()`、`exportExcel()`、`runImportOnDb()`、`runExportOnDb()` 路径存在（`src/DataBridge.cpp:99`、`src/DataBridge.cpp:120`、`src/DataBridge.cpp:181`、`src/DataBridge.cpp:318`、`src/DataBridge.cpp:225`、`src/DataBridge.cpp:254`）。
- 错误返回语义：**基本实现**。未打开 DB、profile 缺失、schema refresh 失败会返回 RowError（`src/DataBridge.cpp:191`、`src/DataBridge.cpp:199`、`src/DataBridge.cpp:208`、`src/DataBridge.cpp:320`、`src/DataBridge.cpp:328`）。
- sync active direct import：**已实现**。同步激活时 `DataBridge::importExcel()` 阻断，要求走 batch/sync worker（`src/DataBridge.cpp:177`、`src/DataBridge.cpp:183`、`src/DataBridge.cpp:186`）。
- BatchTransfer：**部分实现**。sync active 时通过 `SyncContext::importFn` 路由到 worker（`src/batch/BatchTransfer.cpp:140`、`src/batch/BatchTransfer.cpp:147`、`src/batch/BatchTransfer.cpp:148`），但如果 sync context 存在而 `importFn` 缺失则回落 direct import（`src/batch/BatchTransfer.cpp:167`、`src/batch/BatchTransfer.cpp:170`），需要确认是否违反“同步期间所有写必须捕获”的约束。

## 第四部分：安全与数据完整性

### 4.1 SQL 注入风险

确认值参数大多使用绑定，例如 import upsert binds（`src/service/ImportService.cpp:626`）、lookup prefetch binds（`src/service/ImportService.cpp:338`）、selection resolver binds（`src/sync/selection/SelectionResolver.cpp:59`）。主要风险集中在标识符拼接：

- `src/schema/SchemaIntrospector.cpp:26` — `PRAGMA table_xinfo('%1')` 直接拼表名；schema 表名来自数据库 catalog，带 `'` 时会破坏语句 — 使用 `SqlBuilder::quoteIdent()` 或 SQLite PRAGMA table-valued function。
- `src/schema/SchemaIntrospector.cpp:76` — `PRAGMA index_list('%1')` 直接拼表名 — 同上。
- `src/schema/SchemaIntrospector.cpp:96` — `PRAGMA index_info('%1')` 直接拼 index 名 — 同上。
- `src/schema/SchemaIntrospector.cpp:108` — `PRAGMA foreign_key_list('%1')` 直接拼表名 — 同上。
- `src/sync/baseline/BaselineManager.cpp:29` — `SELECT COUNT(*) FROM "%1"` 未转义表名中的 `"` — 应使用 `SqlBuilder::quoteIdent(table)`。
- `src/sync/baseline/BaselineManager.cpp:41` — `SELECT * FROM "%1"` 未转义表名 — 应使用 `quoteIdent()`。
- `src/sync/schema/TableStateStore.cpp:117` — `SELECT * FROM "%1"` 未转义表名 — 应使用 `quoteIdent()`。
- `src/sync/schema/SchemaEligibility.cpp:148` — `PRAGMA table_info("%1")` 未转义表名 — 应使用 `quoteIdent()`。
- `src/sync/schema/SchemaEligibility.cpp:191` — 同类 PRAGMA 未转义 — 应使用 `quoteIdent()`。
- `src/sync/SyncWorker.cpp:696` — selection push 入站读取 PK 的 PRAGMA 未转义表名 — payload 表名若异常会破坏语句。
- `src/sync/diff/DiffEngine.cpp:138` — `SELECT * FROM "%1"` 未转义且无排序 — 同时是 SQL 安全与正确性问题。
- `src/sync/diff/DiffEngine.cpp:168` — `PRAGMA table_info("%1")` 未转义表名 — 应统一 quote。
- `src/sync/diff/ComparisonSession.cpp:357` — `SELECT * FROM "%1" WHERE "%2"` 未转义 table/pkCol — 应统一 quote。
- `src/sync/diff/ComparisonSession.cpp:374` — `PRAGMA table_info("%1")` 未转义表名 — 应统一 quote。
- `src/sync/apply/CapturedWriteTemplate.cpp:225` — WHERE PK 列拼接 `"%1"` 未转义 — 应用 `quoteIdent(pk)`。
- `src/sync/apply/CapturedWriteTemplate.cpp:260` — `SELECT * FROM "%1"` 未转义 table — 应用 `quoteIdent(m.table)`。
- `src/sync/apply/CapturedWriteTemplate.cpp:514`、`src/sync/apply/CapturedWriteTemplate.cpp:525`、`src/sync/apply/CapturedWriteTemplate.cpp:530` — `execMutation()` 保留未 quote SQL 构造；虽当前注释称保留不用（`src/sync/apply/CapturedWriteTemplate.cpp:500`），建议删除或修正，避免未来误用。
- `src/sql/SqlBuilder.cpp:84`、`src/sql/SqlBuilder.cpp:89` — JOIN ON 使用手写双引号和 `.arg()`，不走 `quoteIdent()`；多父同 child 表还会重复 `LEFT JOIN "child"`，有 alias 冲突风险。

### 4.2 事务安全性

- `WriteTxn` RAII 在析构时 rollback 未提交事务（`src/sync/WriteTxn.cpp:10`、`src/sync/WriteTxn.cpp:21`、`src/sync/WriteTxn.cpp:35`），核心路径较安全。
- `CapturedWriteTemplate::branchA()` 错误路径基本 rollback（`src/sync/apply/CapturedWriteTemplate.cpp:52`、`src/sync/apply/CapturedWriteTemplate.cpp:75`、`src/sync/apply/CapturedWriteTemplate.cpp:101`、`src/sync/apply/CapturedWriteTemplate.cpp:124`）。
- `submitImportSync()` 是严重例外：`rec_->begin()`、`rec_->sealInto()`、`txn.commit()` 均忽略失败（`src/sync/SyncWorker.cpp:1144`、`src/sync/SyncWorker.cpp:1160`、`src/sync/SyncWorker.cpp:1162`），导致 ImportResult 与真实事务/变更捕获状态不一致。
- `processAckArtifact()` 写 ACK 状态时忽略 `markQ.exec()` 和 `doneQ.exec()` 失败（`src/sync/SyncWorker.cpp:837`、`src/sync/SyncWorker.cpp:856`），会把进度完成建立在未持久化状态上。

### 4.3 竞态与线程安全

- worker 单写线程设计基本成立：worker 自建写连接（`src/sync/SyncWorker.cpp:160`、`src/sync/SyncWorker.cpp:164`），外部写通过 queue/promise 提交（`src/sync/SyncWorker.cpp:100`、`src/sync/SyncWorker.cpp:117`、`src/sync/SyncWorker.cpp:119`）。
- 前台 gate 保护 sync 和 batch 操作（`src/sync/SyncEngine.cpp:149`、`src/sync/SyncEngine.cpp:231`、`src/batch/BatchTransfer.cpp:63`、`src/batch/BatchTransfer.cpp:114`）。
- `ackWaiting_` 是全局状态，不携带 operation identity（`src/sync/SyncWorker.cpp:1188`、`src/sync/SyncWorker.cpp:798`、`src/sync/SyncWorker.cpp:859`），存在并发/迟到 ACK 误完成当前操作的竞态。
- ComparisonSession read snapshot 未真正开启显式 read transaction，只执行 `SELECT * FROM sqlite_master LIMIT 0`（`src/sync/diff/ComparisonSession.cpp:64`、`src/sync/diff/ComparisonSession.cpp:68`），SQLite snapshot 是否保持到后续多次读取依赖连接事务状态，不够严格。

### 4.4 资源泄漏

- QSqlDatabase worker 连接 teardown 清理（`src/sync/SyncWorker.cpp:412`、`src/sync/SyncWorker.cpp:417`、`src/sync/SyncWorker.cpp:418`），DataBridge close 先清空 db_ 再 removeDatabase（`src/DataBridge.cpp:63`、`src/DataBridge.cpp:71`、`src/DataBridge.cpp:73`），总体合理。
- OutboxWriter 使用 QFile 局部对象，并在失败时删除 tmp/final/ready（`src/sync/transport/OutboxWriter.cpp:198`、`src/sync/transport/OutboxWriter.cpp:223`、`src/sync/transport/OutboxWriter.cpp:247`）。
- `RebaseEngine::rebase()` 在成功和失败路径释放 rebaser/free buffer（`src/sync/conflict/RebaseEngine.cpp:44`、`src/sync/conflict/RebaseEngine.cpp:53`、`src/sync/conflict/RebaseEngine.cpp:61`），无明显泄漏。

## 第五部分：正确性与边界处理

### 5.1 已发现的 Bug

- `src/sync/SyncWorker.h:111` — 声明 baseline request/response handler，但仓库没有定义；`src/sync/SyncWorker.cpp:529` 和 `src/sync/SyncWorker.cpp:531` 调用它们会导致链接失败或 baseline 协议不可用 — 补齐两个 handler，并接入 `BaselineManager::exportBaseline()` / `applyBaseline()`。
- `src/sync/SyncWorker.cpp:1144` — 忽略 `rec_->begin()` 返回值；session attach 失败仍继续写库 — 失败时 rollback 并返回 `E_SYNC_SESSION_UNAVAILABLE` 或 `E_SYNC_INIT`。
- `src/sync/SyncWorker.cpp:1160` — 忽略 `rec_->sealInto()` 返回值；ImportResult 可成功但 changelog 未写入 — 失败时 rollback、清空 `result.ok` 并返回错误。
- `src/sync/SyncWorker.cpp:1162` — 忽略 commit 返回值；事务提交失败仍报告成功 — 检查并传播 commit 错误。
- `src/sync/SyncWorker.cpp:798` — 任意 changeset ACK 都可清空当前 `ackWaiting_` — ACK wait 应绑定 peer/origin/epoch/seq 或 operation id。
- `src/sync/SyncWorker.cpp:859` — 任意 push 完成 ACK 都可完成当前前台操作 — 应绑定本次 `pushId`。
- `src/sync/SyncWorker.cpp:1322` — selection push 所有 chunk `originSeq=0`；接收端 sealInto 会写相同 `(origin, epoch, origin_seq)` — 为每个 chunk 或整个 push 分配合法 origin_seq，或 selection push 不写入 changelog unique namespace。
- `src/sync/apply/CapturedWriteTemplate.cpp:178` — 已 applied chunk 不校验 checksum — 重复 chunk checksum 不同应报 `E_SYNC_PAYLOAD_CORRUPT`。
- `src/sync/diff/ComparisonSession.cpp:280` — comparison save 绕过捕获模板 — 应通过 worker 的 CapturedWriteTemplate branch C 保存 staged mutations。
- `src/sync/diff/DiffEngine.cpp:68` — 行级 diff offset 分页两边后比较 — 应按 PK keyset/全量 PK set 对齐。
- `src/sync/diff/DiffEngine.cpp:138` — 本地 diff 查询无 `ORDER BY` — 至少按 PK 排序。
- `src/sync/selection/FkClosureBuilder.cpp:31` — 仅支持单列 PK — selection push 应支持复合 PK 或 SchemaEligibility 明确拒绝复合 PK。
- `src/sync/selection/ChunkStreamer.cpp:61` — 单行超过 chunk budget 不报错 — 应返回 `E_SYNC_SELECTION_TOO_LARGE`。
- `src/sync/baseline/BaselineManager.cpp:242` — baseline reset table_state 使用空 schema fingerprint — 应传入 payload/local schema fingerprint。
- `src/service/ImportService.cpp:639` — `writtenRows` 按 Excel 行计数，不按实际 upsert payload 计数 — 重命名字段或改为实际写入行数。
- `src/mapping/Mapper.cpp:78` — 同 route 任一 validator 错误会阻止后续列 temporal 错误收集 — 应按列独立收集错误。
- `src/mapping/Mapper.cpp:67` — Mapper 记录错误时 sheet 为空 — 应传入 sheetName，避免错误定位缺失。
- `src/sql/SqlBuilder.cpp:84` — auto join 未 quoteIdent 且无 alias，多父同 child 表会 JOIN 冲突 — 使用别名和 `quoteIdent()`。

### 5.2 边界用例覆盖

- 空表同步：表级 checksum/row_count 可表达空表（`src/sync/schema/TableStateStore.cpp:135`、`src/sync/schema/TableStateStore.cpp:145`），但 baseline serialize 读 count/select 仍有未转义标识符风险（`src/sync/baseline/BaselineManager.cpp:29`、`src/sync/baseline/BaselineManager.cpp:41`）。
- 首次同步（无基线）：changeset 首次 seq 必须为 1（`src/sync/apply/AppliedVectorStore.cpp:24`、`src/sync/apply/AppliedVectorStore.cpp:26`）；若源端已经截断 changelog，会进入 gap，但 baseline 请求闭环缺失（`src/sync/SyncWorker.cpp:1074`）。
- 网络中断后恢复：outbox/ready/ledger/ACK store 有基础设施（`src/sync/transport/OutboxWriter.cpp:183`、`src/sync/transport/InboxLedger.cpp:71`、`src/sync/anchor/OutboundAckStore.cpp:92`），但 ACK wait 没有关联 operation identity（`src/sync/SyncWorker.cpp:1188`）。
- 循环 FK：TopoSorter/closure 会报 cycle（`src/sync/selection/FkClosureBuilder.cpp:187`、`src/sync/selection/FkClosureBuilder.cpp:189`），import route topo 也会拒绝（`src/service/ImportService.cpp:470`、`src/service/ImportService.cpp:472`）。
- 重复导入同一文件：普通 import 用 `ON CONFLICT DO UPDATE`（`src/sql/SqlBuilder.cpp:41`、`src/sql/SqlBuilder.cpp:50`），batch uniqueness 防同批冲突（`src/service/ImportService.cpp:528`、`src/mapping/BatchUniqueness.cpp:14`），但 sync routed import 的 changelog 捕获失败可能漏广播（`src/sync/SyncWorker.cpp:1160`）。

### 5.3 错误传播路径

- ErrorCollector 用于 Import/Export/Profile 主要路径（`src/service/ImportService.cpp:392`、`src/service/ExportService.cpp:562`、`src/profile/ProfileValidator.cpp:330`）。
- Mapper 错误 sheet 缺失（`src/mapping/Mapper.cpp:67`、`src/mapping/Mapper.cpp:94`）。
- UpsertExecutor 对单行失败只 append row error 并继续（`src/sync/apply/UpsertExecutor.cpp:69`、`src/sync/apply/UpsertExecutor.cpp:80`）；CapturedWriteTemplate 会把 rowErrors 提升为 chunk 失败（`src/sync/apply/CapturedWriteTemplate.cpp:290`、`src/sync/apply/CapturedWriteTemplate.cpp:293`）。
- ExportService 最后无条件 `result.ok=true`（`src/service/ExportService.cpp:933`），即使已有非 fatal errors（`src/service/ExportService.cpp:935`）；需要明确哪些 export errors 应阻断。
- ACK 状态写失败未传播（`src/sync/SyncWorker.cpp:837`、`src/sync/SyncWorker.cpp:856`）。

## 第六部分：架构质量

### 6.1 模块职责与耦合

- 正向点：同步模块按 capture/apply/diff/transport/selection/schema/baseline 拆分，核心对象在 `SyncWorker` 初始化集中组装（`src/sync/SyncWorker.cpp:68`、`src/sync/SyncWorker.cpp:70`、`src/sync/SyncWorker.cpp:86`）。
- 职责混乱点：`SyncWorker` 同时处理线程、DDL、inbox/outbox、ACK、baseline fallback、selection push、import routing，文件达到 1359 行（`src/sync/SyncWorker.cpp:1`、`src/sync/SyncWorker.cpp:1359`），是最大复杂度中心。
- 绕过统一写路径：`ComparisonSession::save()` 和 `StagingBuffer::save()` 自己做事务/upsert（`src/sync/diff/ComparisonSession.cpp:280`、`src/sync/diff/StagingBuffer.cpp:45`），破坏 CapturedWriteTemplate 的单一写入口。
- 重复职责：`SelectionPushApplier` 与 CapturedWriteTemplate/UpsertExecutor 叠加（`src/sync/apply/SelectionPushApplier.cpp:9`、`src/sync/SyncWorker.cpp:739`）。

### 6.2 SOLID 原则评估

- 单一职责：`SyncWorker` 违反较明显，建议拆出 `AckProcessor`、`BaselineProtocolHandler`、`SelectionPushReceiver`、`BroadcastPump`。
- 开放封闭：payload kind 增加 baseline 后，`SyncWorker::processArtifact()` 的 if/else 链需要修改（`src/sync/SyncWorker.cpp:521`、`src/sync/SyncWorker.cpp:528`）；可用 handler registry 降低扩展成本。
- 依赖倒置：`ComparisonSession` 依赖 `std::function workerWriteFn` 是好接缝（`src/sync/diff/ComparisonSession.cpp:271`、`src/sync/diff/ComparisonSession.cpp:280`），但该接缝传入的是裸 `QSqlDatabase` task，导致调用方可绕过捕获模板。

### 6.3 可测试性

- 正向点：纯逻辑类较容易测，如 `ConflictArbiter`（`src/sync/conflict/ConflictArbiter.cpp:13`）、`ChunkStreamer`（`src/sync/selection/ChunkStreamer.cpp:36`）、`TemporalConvert`（`src/mapping/TemporalConvert.cpp:33`）。
- 难测点：`SyncWorker` 大量直接文件系统、QSqlDatabase、线程、信号交织（`src/sync/SyncWorker.cpp:160`、`src/sync/SyncWorker.cpp:432`、`src/sync/SyncWorker.cpp:468`），需要端到端 harness 才能覆盖 ACK/gap/baseline。
- 硬编码依赖：gap timeout 写死 30s（`src/sync/SyncWorker.cpp:454`），submitWrite/Import 等待写死 60s（`src/sync/SyncWorker.cpp:129`、`src/sync/SyncWorker.cpp:1172`），测试难以快速验证超时分支。

### 6.4 代码重复与抽象质量

- 标识符 quote 规则重复且不一致：已有 `SqlBuilder::quoteIdent()`（`src/sql/SqlBuilder.cpp:8`），但 schema/diff/baseline 多处手写双引号或单引号 PRAGMA（`src/schema/SchemaIntrospector.cpp:26`、`src/sync/diff/DiffEngine.cpp:138`、`src/sync/baseline/BaselineManager.cpp:29`）。
- PK 获取逻辑重复：`DiffEngine::getPkColumn()`（`src/sync/diff/DiffEngine.cpp:163`）、`ComparisonSession::getPkColumn()`（`src/sync/diff/ComparisonSession.cpp:369`）、`FkClosureBuilder::getPkColumn()`（`src/sync/selection/FkClosureBuilder.cpp:31`）、`SyncWorker` lambda（`src/sync/SyncWorker.cpp:691`）均各自实现，且复合 PK 支持不一致。
- Upsert 生成重复：Import 使用 `SqlBuilder::buildUpsert()`（`src/service/ImportService.cpp:604`），Sync 使用 `UpsertExecutor::buildUpsertSql()`（`src/sync/apply/UpsertExecutor.cpp:96`），CapturedWriteTemplate 还保留未 quote 的 `execMutation()`（`src/sync/apply/CapturedWriteTemplate.cpp:500`）。

## 第七部分：总结与优先级修复建议

### Critical（必须修复）

- [C-1] `src/sync/SyncWorker.h:111` / `src/sync/SyncWorker.cpp:529` — baseline request/response handler 声明和调用存在但无定义，baseline 协议无法链接/闭环 — 实现 `processBaselineRequestArtifact()` 和 `processBaselineResponseArtifact()`，接入 `PayloadCodec` 与 `BaselineManager`。
- [C-2] `src/sync/SyncWorker.cpp:1144` / `src/sync/SyncWorker.cpp:1160` / `src/sync/SyncWorker.cpp:1162` — sync-routed import 忽略 session begin/seal/commit 失败，可能成功返回但不写 changelog — 所有失败必须 rollback 并返回 RowError。
- [C-3] `src/sync/SyncWorker.cpp:1322` / `src/sync/apply/CapturedWriteTemplate.cpp:317` / `src/sync/SyncDDL.h:28` — selection push chunk 使用相同 `originSeq=0`，会撞 changelog unique 约束或破坏序列语义 — 为 push/chunk 分配合法 origin seq，或设计独立 selection-push changelog namespace。
- [C-4] `src/sync/SyncWorker.cpp:798` / `src/sync/SyncWorker.cpp:859` / `src/sync/SyncWorker.cpp:1188` — ACK wait 全局化，迟到/无关 ACK 可完成当前前台操作 — 引入 operation id，并校验 peer、origin、epoch、seq、pushId。
- [C-5] `src/sync/diff/ComparisonSession.cpp:280` / `src/sync/diff/StagingBuffer.cpp:45` — comparison save 绕过 CapturedWriteTemplate，接受远端数据不会进入 changelog/table_state — save 应提交 RowMutation 给 worker 捕获模板。

### High（强烈建议）

- [H-1] `src/sync/diff/DiffEngine.cpp:68` / `src/sync/diff/DiffEngine.cpp:138` — 行级 diff offset 分页且无排序，增删行会导致漏报/误报 — 改为按 PK keyset 排序和比较。
- [H-2] `src/sync/selection/FkClosureBuilder.cpp:31` / `src/sync/selection/FkClosureBuilder.cpp:43` — selection push 不支持复合 PK — 实现复合 PK key 编码，或在 schema eligibility 中拒绝复合 PK 同步。
- [H-3] `src/sync/apply/CapturedWriteTemplate.cpp:178` / `src/sync/apply/CapturedWriteTemplate.cpp:188` — 重复 chunk 不校验 checksum — 相同 checksum no-op，不同 checksum quarantine 并报 `E_SYNC_PAYLOAD_CORRUPT`。
- [H-4] `src/sync/selection/ChunkStreamer.cpp:61` / `src/sync/selection/ChunkStreamer.cpp:68` — 单行超过 chunk budget 不报错 — 检测并返回 `E_SYNC_SELECTION_TOO_LARGE`。
- [H-5] `src/sync/baseline/BaselineManager.cpp:242` / `src/sync/diff/DiffEngine.cpp:43` — baseline table_state 使用空 schema fingerprint 导致伪差异 — applyBaseline 时写入可信 schema fingerprint。
- [H-6] `src/sql/SqlBuilder.cpp:84` / `src/sql/SqlBuilder.cpp:89` — auto join 未 quoteIdent 且缺表别名，多父同 child join 会冲突 — 使用稳定 alias 并统一 quote。

### Medium（建议修复）

- [M-1] `src/schema/SchemaIntrospector.cpp:26` / `src/schema/SchemaIntrospector.cpp:76` / `src/schema/SchemaIntrospector.cpp:96` / `src/schema/SchemaIntrospector.cpp:108` — PRAGMA 直接拼标识符 — 改用 quote 或 table-valued PRAGMA。
- [M-2] `src/sync/baseline/BaselineManager.cpp:29` / `src/sync/baseline/BaselineManager.cpp:41` / `src/sync/schema/TableStateStore.cpp:117` — baseline/table_state 扫描表名未转义 — 使用 `SqlBuilder::quoteIdent()`。
- [M-3] `src/sync/schema/SchemaEligibility.cpp:148` / `src/sync/schema/SchemaEligibility.cpp:191` / `src/sync/SyncWorker.cpp:696` — PRAGMA table_info 表名未转义 — 统一 quote。
- [M-4] `src/sync/apply/UpsertExecutor.cpp:33` / `src/sync/apply/UpsertExecutor.cpp:47` — prepared cache key 不含列集/PK 集，依赖 `lastQuery()` — cache key 应包含 SQL 或列签名。
- [M-5] `src/service/ImportService.cpp:639` — `writtenRows` 语义是 Excel 行不是实际写入 payload — 调整字段语义或计数。
- [M-6] `src/mapping/Mapper.cpp:67` / `src/mapping/Mapper.cpp:94` — Mapper 错误 sheet 为空 — 将 sheetName 传入 Mapper。
- [M-7] `src/service/ExportService.cpp:776` / `src/service/ExportService.cpp:833` — `exportRoundtrip=false` 仍触发 reverse full-load path — 用 `hasActiveReverseLookup()` 决定反查，仅 H 列原样输出时走轻量路径。
- [M-8] `src/sync/transport/InboxWatcher.cpp:26` / `src/sync/transport/InboxWatcher.cpp:28` — 注释 oldest first 与 `QDir::Time` 默认排序不匹配 — 明确 `QDir::Reversed` 或按文件 mtime 升序排序。

### Low（可选优化）

- [L-1] `src/sync/apply/SelectionPushApplier.cpp:9` / `src/sync/SyncWorker.cpp:739` — SelectionPushApplier 与主路径职责重复 — 删除空壳或改造成唯一 selection apply 入口。
- [L-2] `src/sync/apply/CapturedWriteTemplate.cpp:500` / `src/sync/apply/CapturedWriteTemplate.cpp:525` — 保留未使用且未 quote 的 `execMutation()` — 删除死代码或修正后加测试。
- [L-3] `src/sync/SyncWorker.cpp:454` / `src/sync/SyncWorker.cpp:1172` — gap timeout 和同步等待时间硬编码 — 移入 SyncConfig 以便测试和部署调优。
- [L-4] `src/schema/SchemaIntrospector.cpp:11` — catalog 读取包含 `__sync_*` 表 — 如果 profile 不应绑定内部表，应在 introspector 排除。
- [L-5] `src/service/ExportService.cpp:933` / `src/service/ExportService.cpp:935` — export 有 errors 仍 ok=true 的策略不够清晰 — 明确 error/warning 分层，或让 `exportOnMissing=error` 阻断结果。
