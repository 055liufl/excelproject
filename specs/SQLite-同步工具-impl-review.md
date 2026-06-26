# SQLite 同步工具实现评审报告（第十一轮）

评审日期：2026-06-26

## 总体评分

总分：72/100。

各维度子分：

- 合规性：70/100。同步、导入导出和 OpenSpec 主体能力覆盖面较高，但基线 gap 闭环、MVP 导入原子性、部分时间格式继承语义存在明确偏差。
- 正确性：72/100。向量时钟连续性、ACK、FK 闭包、反向查找等核心路径有实现；但类型严格相等、主键/tuple key 字符串化、导出时间错误定位、低优先级 DELETE 恢复语义有实质风险。
- 安全性：68/100。大部分业务 SQL 使用绑定参数，`SqlBuilder::quoteIdent()` 已建立；但同步若干路径仍手工拼接/手工双引号，未统一通过 `quoteIdent()`。
- 架构质量：76/100。模块拆分较完整，SyncWorker 单写线程模型清晰；但 SyncWorker 仍承担传输、ACK、baseline、selection push、apply 调度等过多职责。
- 边界处理：69/100。已有 stale gap、schema guard、quarantine、ACK timeout、FK cycle/too large 等处理；但 baseline fallback 未闭环，复合主键整体拒绝，若干空字符串/NULL/类型边界与规格不一致。

本轮已按要求读取全部 15 份规格/设计文档，并读取 `src/` 下 122 个 `.cpp/.h` 源文件。

## 第一部分：功能合规性评审

### 1.1 同步引擎核心（SyncEngine / SyncWorker）

- 已实现：单写线程模型。`SyncEngine::initialize()` 创建 `SyncWorker` 并把 `importFn`、`workerWriteFn`、`workerCaptureWriteFn` 绑定到共享 `SyncContext`，避免主线程直接跨线程使用数据库连接，见 `src/sync/SyncEngine.cpp:62`、`src/sync/SyncEngine.cpp:123`、`src/sync/SyncEngine.cpp:128`、`src/sync/SyncEngine.cpp:134`。
- 已实现：同步激活后阻断 `DataBridge::importExcel()` 直接写入，要求通过 `IBatchTransfer` 进入 worker，见 `src/DataBridge.cpp:177`、`src/DataBridge.cpp:181`、`src/DataBridge.cpp:184`。
- 已实现：物理数据库上下文以 POSIX dev/inode 或 Windows file index 作为 key，符合单库单上下文要求，见 `src/sync/SyncContext.cpp:20`、`src/sync/SyncContext.cpp:52`、`src/sync/SyncContext.cpp:61`。
- 已实现：ChangeLog 捕获与本地导入同事务封装。`SyncWorker::submitImportSync()` 在 worker 线程 `BEGIN IMMEDIATE`、启动 sqlite session、运行导入、`sealInto()` 写入 changelog 后提交，见 `src/sync/SyncWorker.cpp:1246`、`src/sync/SyncWorker.cpp:1262`、`src/sync/SyncWorker.cpp:1277`、`src/sync/SyncWorker.cpp:1286`、`src/sync/SyncWorker.cpp:1300`。
- 已实现：前台同步门闩。`ForegroundGate` 确保前台 import/export/syncSelected 不并行，见 `src/sync/ForegroundGate.h:12`、`src/batch/BatchTransfer.cpp:63`、`src/batch/BatchTransfer.cpp:112`、`src/sync/SyncEngine.cpp:159`。
- 已实现：出站/入站通道。Outbox 采用 tmp 写入、fsync、rename、ready marker、目录 fsync，见 `src/sync/transport/OutboxWriter.cpp:24`、`src/sync/transport/OutboxWriter.cpp:45`、`src/sync/transport/OutboxWriter.cpp:59`、`src/sync/transport/OutboxWriter.cpp:73`、`src/sync/transport/OutboxWriter.cpp:95`、`src/sync/transport/OutboxWriter.cpp:113`。Inbox ledger 有 seen/consumed/corrupt，见 `src/sync/transport/InboxLedger.cpp:20`、`src/sync/transport/InboxLedger.cpp:36`、`src/sync/transport/InboxLedger.cpp:52`。
- 已实现：ACK timeout 与前台状态回写。见 `src/sync/SyncWorker.cpp:394`、`src/sync/SyncWorker.cpp:904`、`src/sync/SyncWorker.cpp:974`。
- 偏差：changeset ACK 到达时只校验 `toPeer`，没有绑定当前前台 `sync()` 发出的批次或目标 peer；任意合法 changeset ACK 都会清空 `ackWaiting_` 并置 Completed，见 `src/sync/SyncWorker.cpp:888`、`src/sync/SyncWorker.cpp:900`、`src/sync/SyncWorker.cpp:904`。selection push ACK 已通过 `pendingPushId_` 过滤，见 `src/sync/SyncWorker.cpp:927`，普通 changeset ACK 缺同等级关联。
- 缺失：gap 后的 baseline fallback 未形成协议闭环。stale pending 会调用 `runBaselineFallbackFor()`，见 `src/sync/SyncWorker.cpp:451`、`src/sync/SyncWorker.cpp:463`；但该函数只报错、隔离文件、mark consumed，并明确说明需要 source-authoritative baseline 但尚未实现请求发送，见 `src/sync/SyncWorker.cpp:1189`、`src/sync/SyncWorker.cpp:1194`、`src/sync/SyncWorker.cpp:1202`、`src/sync/SyncWorker.cpp:1210`。虽然 baseline request/response handler 已存在，见 `src/sync/SyncWorker.cpp:773`、`src/sync/SyncWorker.cpp:828`，但 gap 触发点未发出 `BaselineRequest`。

### 1.2 差量与冲突处理（DiffEngine / ConflictArbiter / RebaseEngine）

- 已实现：表级差量比较包含 schema fingerprint、checksum、row count 三要素，见 `src/sync/diff/DiffEngine.cpp:41`、`src/sync/diff/DiffEngine.cpp:43`。
- 已实现：行级 diff 可比较 added/deleted/modified/same，见 `src/sync/diff/DiffEngine.cpp:68`、`src/sync/diff/DiffEngine.cpp:97`、`src/sync/diff/DiffEngine.cpp:108`、`src/sync/diff/DiffEngine.cpp:119`。
- 已实现：冲突仲裁按 rank 优先、rank 相同时 originSeq 后写胜出，见 `src/sync/conflict/ConflictArbiter.cpp:13`、`src/sync/conflict/ConflictArbiter.cpp:18`、`src/sync/conflict/ConflictArbiter.cpp:22`。`ChangesetApplier` 的 sqlite conflict callback 也按同样语义更新 winner，见 `src/sync/apply/ChangesetApplier.cpp:194`、`src/sync/apply/ChangesetApplier.cpp:196`、`src/sync/apply/ChangesetApplier.cpp:199`。
- 已实现：RebaseEngine 使用 sqlite3 rebaser API，见 `src/sync/conflict/RebaseEngine.cpp:9`、`src/sync/conflict/RebaseEngine.cpp:17`、`src/sync/conflict/RebaseEngine.cpp:27`。
- 偏差：row diff 和 comparison session 的主键索引用 `toString()`，不同 SQLite 类型可能碰撞或排序/匹配错误，见 `src/sync/diff/DiffEngine.cpp:79`、`src/sync/diff/DiffEngine.cpp:82`、`src/sync/diff/DiffEngine.cpp:89`、`src/sync/diff/ComparisonSession.cpp:363`。
- 偏差：低优先级 DELETE 恢复路径用 `INSERT OR REPLACE`，SQLite 会执行 DELETE+INSERT 语义，可能触发级联删除或破坏 rowid/触发器语义，见 `src/sync/apply/ChangesetApplier.cpp:302`、`src/sync/apply/ChangesetApplier.cpp:304`。这与设计文档中避免 `INSERT OR REPLACE` 的数据完整性原则不一致。

### 1.3 变更捕获（ChangelogStore / SessionRecorder）

- 已实现：sqlite session 创建、attach 同步表、收集 changeset、写 changelog 都有实现，见 `src/sync/capture/SessionRecorder.cpp:19`、`src/sync/capture/SessionRecorder.cpp:28`、`src/sync/capture/SessionRecorder.cpp:59`、`src/sync/capture/SessionRecorder.cpp:77`。
- 已实现：changelog append 使用普通 INSERT，不吞掉重复 `(origin, epoch, origin_seq)` 错误，见 `src/sync/capture/ChangelogStore.cpp:134`、`src/sync/capture/ChangelogStore.cpp:138`、`src/sync/capture/ChangelogStore.cpp:158`。
- 已实现：session 捕获硬依赖 sqlite session/preupdate 编译选项，见 `src/sync/capture/SqliteHandle.cpp:15`、`src/sync/capture/SqliteHandle.cpp:19`。
- 偏差：session hard gate 只检查编译选项和 handle，未记录/比对 SQLite source id，也未验证 Qt 驱动实际链接库与 session API 的一致性；`SqliteHandle::libVersion()` 只暴露版本字符串，见 `src/sync/capture/SqliteHandle.cpp:23`。若运行环境存在 Qt SQLite 插件与头文件/库不一致，错误定位能力不足。
- 已实现：无变更时 `collectChangeset()` 返回非 null 空 QByteArray，避免把无变更误判为错误，见 `src/sync/capture/SessionRecorder.cpp:108`、`src/sync/capture/SessionRecorder.cpp:112`。

### 1.4 选择与传输（SelectionResolver / ChunkStreamer / OutboxWriter / InboxWatcher）

- 已实现：`SelectionResolver` 拒绝 raw SQL where，避免 selection API 注入风险，见 `src/sync/selection/SelectionResolver.cpp:80`、`src/sync/selection/SelectionResolver.cpp:83`、`src/sync/selection/SelectionResolver.cpp:91`。
- 已实现：按主键解析记录并使用绑定参数，见 `src/sync/selection/SelectionResolver.cpp:57`、`src/sync/selection/SelectionResolver.cpp:59`、`src/sync/selection/SelectionResolver.cpp:62`。
- 已实现：FK 闭包 BFS 扩展依赖、缺失依赖报错、拓扑排序、循环拒绝，见 `src/sync/selection/FkClosureBuilder.cpp:85`、`src/sync/selection/FkClosureBuilder.cpp:98`、`src/sync/selection/FkClosureBuilder.cpp:114`、`src/sync/selection/FkClosureBuilder.cpp:138`、`src/sync/selection/FkClosureBuilder.cpp:187`。
- 已实现：chunk streamer 按预算分块，单行超预算直接失败，见 `src/sync/selection/ChunkStreamer.cpp:36`、`src/sync/selection/ChunkStreamer.cpp:64`、`src/sync/selection/ChunkStreamer.cpp:89`。
- 已实现：selection push 接收侧强制 chunk 顺序，避免子表先于父表落地，见 `src/sync/SyncWorker.cpp:669`、`src/sync/SyncWorker.cpp:674`、`src/sync/SyncWorker.cpp:683`。
- 偏差：`FkClosureBuilder` 与 selection key 使用字符串化主键，见 `src/sync/selection/FkClosureBuilder.cpp:14`、`src/sync/selection/FkClosureBuilder.cpp:104`、`src/sync/selection/SelectionResolver.cpp:40`、`src/sync/selection/SelectionResolver.cpp:45`。由于同步 schema 当前拒绝复合主键，影响范围主要是单列非 TEXT 主键的类型保真。
- 偏差：`ChunkStreamer::stream()` 参数中 `targetPeer` 与 `PayloadCodec` 当前未使用，见 `src/sync/selection/ChunkStreamer.cpp:39`、`src/sync/selection/ChunkStreamer.cpp:40`。功能可运行，但设计上目标 peer/编码预算未真正参与分块策略。

### 1.5 模式守卫（SchemaGuard / SchemaEligibility）

- 已实现：空 syncTables 展开为所有用户表，排除 sqlite 内部表和 `__sync_%` 表，见 `src/sync/schema/SchemaEligibility.cpp:14`、`src/sync/schema/SchemaEligibility.cpp:21`。
- 已实现：拒绝不存在表、view、virtual table、shadow table、无显式 PK、nullable PK，见 `src/sync/schema/SchemaEligibility.cpp:52`、`src/sync/schema/SchemaEligibility.cpp:56`、`src/sync/schema/SchemaEligibility.cpp:60`、`src/sync/schema/SchemaEligibility.cpp:64`、`src/sync/schema/SchemaEligibility.cpp:68`、`src/sync/schema/SchemaEligibility.cpp:72`。
- 已实现但偏严格：复合主键表全部拒绝，见 `src/sync/schema/SchemaEligibility.cpp:76`、`src/sync/schema/SchemaEligibility.cpp:79`。这与当前 SelectionResolver/FkClosureBuilder/ChangesetApplier 的单列 PK 假设一致，但比“有可用 conflict target”的规格目标更窄。
- 已实现：schema fingerprint 包含表名、列名、类型、PK 序，见 `src/sync/schema/SchemaGuard.cpp:36`、`src/sync/schema/SchemaGuard.cpp:41`、`src/sync/schema/SchemaGuard.cpp:51`。
- 偏差：fingerprint 未纳入 NOT NULL、默认值、唯一索引、外键等约束信息，`SchemaGuard.cpp:52` 注释列出 `notnull` 和 `dflt_value`，但实际 material 只追加 name/type/pk，见 `src/sync/schema/SchemaGuard.cpp:52`、`src/sync/schema/SchemaGuard.cpp:56`、`src/sync/schema/SchemaGuard.cpp:59`。如果约束变化但列名/类型/PK 不变，payload 可能未被 guard 拦截。

### 1.6 应用层（ChangesetApplier / UpsertExecutor / SelectionPushApplier）

- 已实现：`UpsertExecutor` 使用 `INSERT ... ON CONFLICT DO UPDATE` 或 `INSERT OR IGNORE`，并统一 `quoteIdent()`，见 `src/sync/apply/UpsertExecutor.cpp:83`、`src/sync/apply/UpsertExecutor.cpp:86`、`src/sync/apply/UpsertExecutor.cpp:126`。
- 已实现：selection push 通过 `CapturedWriteTemplate` 进入捕获模板，而不是绕过 changelog，见 `src/sync/apply/SelectionPushApplier.cpp:28`、`src/sync/apply/SelectionPushApplier.cpp:41`。
- 已实现：CapturedWriteTemplate 对 inbound selection push 的行级 upsert 错误会整体回滚 chunk，不会 ACK 部分成功，见 `src/sync/apply/CapturedWriteTemplate.cpp:296`、`src/sync/apply/CapturedWriteTemplate.cpp:303`、`src/sync/apply/CapturedWriteTemplate.cpp:306`。
- 偏差：`UpsertExecutor::apply()` 单独使用时对每行错误继续执行并返回 true，见 `src/sync/apply/UpsertExecutor.cpp:56`、`src/sync/apply/UpsertExecutor.cpp:67`、`src/sync/apply/UpsertExecutor.cpp:72`。当前 CapturedWriteTemplate 会检查 `rowErrors` 并回滚，但其他调用方若直接使用该类需自行保证事务和错误策略。
- 偏差：selection push 构造 `RowMutation` 时 `m.columns = rowMap.keys()`、`m.values = rowMap.values()` 依赖同一个 `QVariantMap/QMap` 的键序一致，见 `src/sync/SyncWorker.cpp:708`、`src/sync/SyncWorker.cpp:713`、`src/sync/SyncWorker.cpp:714`。在 Qt 当前容器语义下通常一致，但这是隐式约束，建议显式按 keys 取值。

### 1.7 基线管理（BaselineManager）

- 已实现：baseline 序列化表、压缩、记录 sourceMaxSeq，见 `src/sync/baseline/BaselineManager.cpp:18`、`src/sync/baseline/BaselineManager.cpp:26`、`src/sync/baseline/BaselineManager.cpp:75`、`src/sync/baseline/BaselineManager.cpp:86`。
- 已实现：baseline apply 先删除目标表再 plain INSERT，不使用 `OR REPLACE`，见 `src/sync/baseline/BaselineManager.cpp:128`、`src/sync/baseline/BaselineManager.cpp:155`、`src/sync/baseline/BaselineManager.cpp:165`。
- 已实现：applyBaseline 同事务重置 applied vector、table state、row winner，见 `src/sync/baseline/BaselineManager.cpp:222`、`src/sync/baseline/BaselineManager.cpp:237`、`src/sync/baseline/BaselineManager.cpp:245`、`src/sync/baseline/BaselineManager.cpp:252`。
- 缺失：baseline fallback 的发起侧未把 gap 转成 baseline request artifact，详见 1.1 的 `SyncWorker.cpp:1168` 至 `SyncWorker.cpp:1211`。
- 风险：baseline deserialize 按传输顺序逐表 DELETE/INSERT，未显式按 FK 拓扑排序；若 SQLite foreign_keys 打开且 baseline 表顺序为子表先父表，删除/插入可能受约束影响，见 `src/sync/baseline/BaselineManager.cpp:115`、`src/sync/baseline/BaselineManager.cpp:128`、`src/sync/baseline/BaselineManager.cpp:141`。

## 第二部分：OpenSpec 特性合规性

### 2.1 FK 注入（FkInjector）

- 已实现：多组 `fkInject`、多 pair、按父 route 输出向子 payload 注入，见 `src/mapping/FkInjector.cpp:21`、`src/mapping/FkInjector.cpp:52`、`src/mapping/FkInjector.cpp:80`、`src/mapping/FkInjector.cpp:116`。
- 已实现：FK preflight 在写库前检查依赖，见 `src/validation/ForeignKeyPreflight.cpp:14`、`src/validation/ForeignKeyPreflight.cpp:81`、`src/service/ImportService.cpp:541`、`src/service/ImportService.cpp:550`。
- 偏差：导入服务对 row-level FK/validator/time 错误只跳过失败行而继续写其他行，见 `src/service/ImportService.cpp:562`、`src/service/ImportService.cpp:587`。这符合 time-format OpenSpec 的“行级错误不终止整 sheet”方向，但违反 MVP 文档的全量预校验失败则整体终止要求。

### 2.2 行查找（Router / Mapper）

- 已实现：ImportService 在映射前预取 lookup，构建缓存并按 0/1/N 命中处理，主逻辑位于 `src/service/ImportService.cpp:84`、`src/service/ImportService.cpp:218`。
- 已实现：Mapper 在有效 temporal 声明存在时剥离 legacy `date:*` validator，并走 temporal convert，见 `src/mapping/Mapper.cpp:23`、`src/mapping/Mapper.cpp:75`、`src/mapping/Mapper.cpp:80`。
- 偏差：lookup tuple key 用 `QVariant::toString()`，无法满足规格中“按 QVariant/SQLite 亲和后的严格相等、不 trim、不大小写折叠”的类型保真要求；`1`、`"1"`、`1.0` 等可能被合并，见 `src/service/ImportService.cpp:63`、`src/service/ImportService.cpp:65`、`src/service/ExportService.cpp:109`、`src/service/ExportService.cpp:110`。
- 偏差：`castToAffinity()` 把 TEXT/BLOB/NONE/NUMERIC 以外情况统一转字符串；OpenSpec 要求 BLOB、NUMERIC、无声明 affinity 不应无条件字符串化，见 `src/service/ImportService.cpp:43`、`src/service/ImportService.cpp:60`、`src/service/ExportService.cpp:117`、`src/service/ExportService.cpp:131`。

### 2.3 导出列顺序（ExportService / ExportHelpers）

- 已实现：columnOrder 通过 `reorderHeaders()` 应用于普通导出、混合导出和反向查找导出，见 `src/service/ExportService.cpp:716`、`src/service/ExportService.cpp:823`、`src/service/ExportService.cpp:893`。
- 已实现：无 columnOrder 且无 reverse lookup 时保持 streaming path，见 `src/service/ExportService.cpp:785`、`src/service/ExportService.cpp:788`。
- 偏差：columnOrder-only path 会全量加载 rows 再写，见 `src/service/ExportService.cpp:809`、`src/service/ExportService.cpp:820`。这是实现简化，不违反功能规格，但大表内存边界弱于 streaming 设计目标。

### 2.4 导出反向查找（ExportService）

- 已实现：reverse lookup 预取 H 值、构建 cache、按 A header 投影，见 `src/service/ExportService.cpp:367`、`src/service/ExportService.cpp:410`、`src/service/ExportService.cpp:669`、`src/service/ExportService.cpp:872`、`src/service/ExportService.cpp:898`。
- 已实现：混合模式按 class 分别解析 route 并合并 header，见 `src/service/ExportService.cpp:590`、`src/service/ExportService.cpp:693`、`src/service/ExportService.cpp:728`。
- 偏差：H 值收集把非 NULL 空字符串当 miss，见 `src/service/ExportService.cpp:381`、`src/service/ExportService.cpp:383`、`src/service/ExportService.cpp:430`、`src/service/ExportService.cpp:432`。规格只允许 SQL NULL 作为 miss；空字符串应参与严格匹配。
- 偏差：反向查找同样受 tuple key 字符串化和 affinity 字符串化影响，见 `src/service/ExportService.cpp:109`、`src/service/ExportService.cpp:117`。

### 2.5 时间格式（TemporalConvert / ProfileSpec）

- 已实现：导入 Mapper 支持 U→structured→V，空值静默为 NULL，解析失败按行记录错误，见 `src/mapping/Mapper.cpp:75`、`src/mapping/Mapper.cpp:82`、`src/mapping/Mapper.cpp:91`、`src/mapping/Mapper.cpp:95`。
- 已实现：导出支持 DB 值 V→structured→U，DB NULL 静默为空，非 NULL 空白继续解析并报错，见 `src/service/ExportService.cpp:61`、`src/service/ExportService.cpp:66`、`src/service/ExportService.cpp:72`、`src/service/ExportService.cpp:80`。
- 偏差：导出时间解析错误固定报 row 0，丢失实际 Excel 输出行号，见 `src/service/ExportService.cpp:63`、`src/service/ExportService.cpp:82`。调用方虽知道 rowCount，但未传入 `convertTemporalForExport()`，见 `src/service/ExportService.cpp:746`、`src/service/ExportService.cpp:816`、`src/service/ExportService.cpp:923`。
- 偏差：profile 级 `dateFormat` 对仅声明 legacy `date:fmt` validator 的列被强制忽略，见 `src/profile/ProfileSpec.h:160`、`src/profile/ProfileSpec.h:162`、`src/profile/ProfileSpec.h:164`。若规格期望 profile root temporal default 与列 validator 共存时由新 `dateFormat` 优先，该实现会偏离；`Mapper` 也只在 effective temporal declared 时剥离 date validator，见 `src/mapping/Mapper.cpp:23`、`src/mapping/Mapper.cpp:27`。

## 第三部分：Excel 批量导入导出合规性

### 3.1 ImportService

- 已实现：读取 Excel、编译 validator、拓扑排序、lookup 预取、FK preflight、upsert 写入形成完整导入链路，关键入口见 `src/service/ImportService.cpp:540`、`src/service/ImportService.cpp:541`、`src/service/ImportService.cpp:593`、`src/service/ImportService.cpp:605`。
- 已实现：写入阶段使用单事务，见 `src/service/ImportService.cpp:579`、`src/service/ImportService.cpp:648`、`src/service/ImportService.cpp:659`。
- 严重偏差：MVP 文档要求导入前全量校验，任一错误终止本次导入且不写入任何数据；当前实现只让 row 0 表级错误整体终止，row-level 错误跳过失败行继续写入成功行，见 `src/service/ImportService.cpp:562`、`src/service/ImportService.cpp:587`、`src/service/ImportService.cpp:598`、`src/service/ImportService.cpp:648`、`src/service/ImportService.cpp:656`。
- 规格冲突说明：OpenSpec time-format 要求时间解析行级错误不终止整 sheet，因此当前实现更接近 time-format 的行级容错；但主 MVP 设计仍要求全量原子导入。建议在产品层明确 profile/option 级策略，否则合规口径不可判定。

### 3.2 ExportService

- 已实现：普通流式导出、columnOrder、mixed、reverse lookup、temporal formatting 都有覆盖，见 `src/service/ExportService.cpp:580`、`src/service/ExportService.cpp:785`、`src/service/ExportService.cpp:793`、`src/service/ExportService.cpp:839`。
- 已实现：SQL 构造通过 `SqlBuilder::buildAutoJoinSelect()` 统一 quote 表/列/orderBy，见 `src/sql/SqlBuilder.cpp:57`、`src/sql/SqlBuilder.cpp:72`、`src/sql/SqlBuilder.cpp:87`、`src/sql/SqlBuilder.cpp:121`。
- 偏差：`explicitSql` 直接执行 profile 提供的 SQL，见 `src/service/ExportService.cpp:763`、`src/service/ExportService.cpp:796`。如果 profile 来自不可信来源，这是功能性逃逸点；若设计假定 profile 可信，则需要在文档/API 中明确。
- 偏差：reverse lookup 与 columnOrder 路径需要全量加载，见 `src/service/ExportService.cpp:809`、`src/service/ExportService.cpp:862`。对大表导出可能违背批量导出设计中的内存可控目标。

### 3.3 DataBridge 公共 API

- 已实现：open 时检查 SQLite >= 3.24.0、设置 busy timeout/WAL/foreign_keys，见 `src/DataBridge.cpp:31`、`src/DataBridge.cpp:44`、`src/DataBridge.cpp:53`、`src/DataBridge.cpp:57`。
- 已实现：导入/导出前刷新 catalog、检查 profile 存在，见 `src/DataBridge.cpp:199`、`src/DataBridge.cpp:208`、`src/DataBridge.cpp:328`、`src/DataBridge.cpp:337`。
- 已实现：同步 active 时 direct import 被拒绝，见 `src/DataBridge.cpp:181`、`src/DataBridge.cpp:184`。
- 已实现：BatchTransfer 在同步上下文存在时把 import 路由到 SyncWorker，见 `src/batch/BatchTransfer.cpp:140`、`src/batch/BatchTransfer.cpp:146`、`src/batch/BatchTransfer.cpp:148`；无同步时使用独立线程连接，见 `src/batch/BatchTransfer.cpp:197`、`src/batch/BatchTransfer.cpp:209`。
- 偏差：`DataBridge::runImportOnDb()` 是 public/internal bridge 绕过 `syncActive_` 阻断的后门，见 `src/DataBridge.cpp:225`、`src/DataBridge.cpp:251`。目前 SyncWorker 需要它的等价能力，但 API 边界应限制为 internal 或明确只给 worker/test 使用。

## 第四部分：安全与数据完整性

### 4.1 SQL 注入风险

已确认的标识符拼接/未统一使用 `quoteIdent()` 位置：

- `src/sync/apply/CapturedWriteTemplate.cpp:237`、`src/sync/apply/CapturedWriteTemplate.cpp:240`：PK where 子句手工 `"%1"=?`，未转义内嵌双引号。
- `src/sync/apply/CapturedWriteTemplate.cpp:275`：`SELECT * FROM "%1"` 手工拼表名，未转义。
- `src/sync/SyncWorker.cpp:695`、`src/sync/SyncWorker.cpp:696`：selection push 接收侧 `PRAGMA table_info("%1")` 手工拼表名，未转义。
- `src/sync/diff/DiffEngine.cpp:181`：`PRAGMA table_info("%1")` 手工拼表名，未转义。
- `src/sync/diff/ComparisonSession.cpp:375`：`SELECT * FROM "%1" WHERE "%2"` 手工拼表名/列名，未转义。
- `src/sync/diff/ComparisonSession.cpp:392`：`PRAGMA table_info("%1")` 手工拼表名，未转义。
- `src/sync/apply/ChangesetApplier.cpp:333`、`src/sync/apply/ChangesetApplier.cpp:334`：已手工 replace 双引号，功能上较安全，但仍应统一走 `SqlBuilder::quoteIdent()`。

正向例子：

- `SqlBuilder::quoteIdent()` 正确转义内嵌双引号，见 `src/sql/SqlBuilder.cpp:8`。
- `UpsertExecutor`、`SelectionResolver`、`FkClosureBuilder`、`BaselineManager` 多数路径已使用 `quoteIdent()`，见 `src/sync/apply/UpsertExecutor.cpp:86`、`src/sync/selection/SelectionResolver.cpp:59`、`src/sync/selection/FkClosureBuilder.cpp:64`、`src/sync/baseline/BaselineManager.cpp:28`。

### 4.2 事务安全性

- 已实现：同步写路径使用 `BEGIN IMMEDIATE`，见 `src/sync/WriteTxn.cpp:8`、`src/sync/WriteTxn.cpp:10`。
- 已实现：CapturedWriteTemplate 出错时 abort session 并 rollback，见 `src/sync/apply/CapturedWriteTemplate.cpp:296`、`src/sync/apply/CapturedWriteTemplate.cpp:306`、`src/sync/apply/CapturedWriteTemplate.cpp:387`。
- 已实现：baseline apply 事务包裹，失败回滚，见 `src/sync/baseline/BaselineManager.cpp:222`、`src/sync/baseline/BaselineManager.cpp:229`、`src/sync/baseline/BaselineManager.cpp:258`。
- 风险：普通 ImportService 对 row-level 错误跳过并提交，见 `src/service/ImportService.cpp:587`、`src/service/ImportService.cpp:648`。这不是事务破坏，但与 MVP 原子导入要求冲突。

### 4.3 竞态与线程安全

- 已实现：DataBridge sync active 标志为 atomic，见 `src/DataBridgePrivate.h:36`。
- 已实现：BatchTransfer 状态读写加 mutex，停止标志为 atomic，见 `src/batch/BatchTransfer.h:44`、`src/batch/BatchTransfer.h:58`。
- 已实现：SyncEngine 快照状态加 mutex，见 `src/sync/SyncEngine.h:51`、`src/sync/SyncEngine.cpp:210`、`src/sync/SyncEngine.cpp:220`。
- 风险：`BatchTransfer::startImport()` 先 snapshot profile/catalog，再检查/使用 SyncContext gate；若 snapshot 与 worker schema 状态之间发生 DDL 变化，后续 worker 导入依赖 SyncWorker 的 schema guard 才能兜底。相关路径见 `src/batch/BatchTransfer.cpp:54`、`src/batch/BatchTransfer.cpp:63`。
- 风险：普通 changeset ACK 未绑定当前 foreground batch，可能被旧 ACK 或其他 peer ACK 提前完成前台 sync，见 `src/sync/SyncWorker.cpp:904`。

### 4.4 资源泄漏

- 已实现：DataBridge close 先清空 `QSqlDatabase` 引用再 removeDatabase，见 `src/DataBridge.cpp:63`、`src/DataBridge.cpp:71`。
- 已实现：SyncEngine 析构停止 worker、清理 callbacks、release context，见 `src/sync/SyncEngine.cpp:14`、`src/sync/SyncEngine.cpp:23`、`src/sync/SyncEngine.cpp:28`。
- 已实现：SessionRecorder abort/delete session，见 `src/sync/capture/SessionRecorder.cpp:85`。
- 风险：`ComparisonSession` 析构中 `QSqlDatabase::removeDatabase()` 时 `deps_->rconn` 仍作为成员存在，虽然先 close 了，但 Qt 通常要求所有 `QSqlDatabase` 句柄销毁/置空后再 remove；当前见 `src/sync/diff/ComparisonSession.cpp:439`、`src/sync/diff/ComparisonSession.cpp:441`、`src/sync/diff/ComparisonSession.cpp:442`。建议将 `deps_->rconn = QSqlDatabase()` 后再 remove。

## 第五部分：正确性与边界处理

### 5.1 已发现的 Bug

- `src/sync/SyncWorker.cpp:1168` — gap baseline fallback 不发送 `BaselineRequest`，只隔离 pending artifact 并返回 false；持久 gap 无法自动恢复 — 建议在 stale gap 处生成并写出 baseline request artifact，记录 in-flight，等待 response 后恢复 pending changeset。
- `src/service/ImportService.cpp:562` — row-level 校验错误不终止整批导入，违反 MVP 全量预校验/全有全无要求 — 建议引入明确导入策略；MVP 默认 any error rollback，time-format 可通过 profile/option 选择 row-resilient。
- `src/service/ImportService.cpp:587` — 失败行被跳过且成功行继续写入 — 同上，若按 MVP 应在写入前发现任何 errors 后直接返回。
- `src/sync/apply/ChangesetApplier.cpp:304` — 低优先级 DELETE 恢复使用 `INSERT OR REPLACE`，可能触发 DELETE+INSERT 级联副作用 — 建议改为 `INSERT ... ON CONFLICT(pk) DO UPDATE`，或用 sqlite changeset 反向补偿策略。
- `src/sync/apply/CapturedWriteTemplate.cpp:240` — PK 标识符未转义 — 建议统一改为 `SqlBuilder::quoteIdent(pk) + " = ?"`。
- `src/sync/apply/CapturedWriteTemplate.cpp:275` — 表名未通过 `quoteIdent()` — 建议统一 quote。
- `src/sync/SyncWorker.cpp:696` — PRAGMA table_info 表名未转义 — 建议统一 quote。
- `src/sync/diff/DiffEngine.cpp:181` — PRAGMA table_info 表名未转义 — 建议统一 quote。
- `src/sync/diff/ComparisonSession.cpp:375` — SELECT 表名/列名未转义 — 建议统一 quote。
- `src/service/ImportService.cpp:60` — BLOB/NUMERIC/NONE affinity 被转字符串 — 建议按 SQLite affinity 规则保留原 QVariant 或只对 TEXT 转字符串。
- `src/service/ImportService.cpp:65` — lookup tuple key 字符串化导致类型碰撞 — 建议构造带类型 tag/二进制规范编码的 key。
- `src/service/ExportService.cpp:110` — reverse lookup tuple key 字符串化导致类型碰撞 — 建议与导入侧共用 typed key helper。
- `src/service/ExportService.cpp:383` — reverse lookup 把空字符串当 missing — 建议只把 `isNull()` 当 missing。
- `src/service/ExportService.cpp:82` — 导出时间解析错误 row 固定为 0 — 建议传入实际输出行号。
- `src/profile/ProfileSpec.h:162` — profile 级 `dateFormat` 被 legacy `date:fmt` validator 屏蔽 — 建议按 OpenSpec 最终决议调整优先级，并补回归测试。
- `src/sync/schema/SchemaGuard.cpp:52` — fingerprint 注释包含 notnull/default，但实际未纳入 material — 建议把 NOT NULL、默认值、唯一索引、FK 参与 fingerprint，或修正文档化边界。

### 5.2 边界用例覆盖

- 已覆盖：SQLite session 不可用时初始化失败路径，见 `src/sync/capture/SqliteHandle.cpp:15` 和 `src/sync/SyncEngine.cpp:79`。
- 已覆盖：selection 空结果、FK 缺失、FK cycle、selection too large，见 `src/sync/SyncWorker.cpp:1449`、`src/sync/selection/FkClosureBuilder.cpp:114`、`src/sync/selection/FkClosureBuilder.cpp:187`、`src/sync/selection/FkClosureBuilder.cpp:226`。
- 已覆盖：chunk ACK 乱序/过期 pushId 过滤，见 `src/sync/SyncWorker.cpp:927`、`src/sync/SyncWorker.cpp:930`。
- 未充分覆盖：lookup/reverse lookup 的 QVariant 类型严格等价、空字符串 H 值、BLOB/NUMERIC affinity。
- 未充分覆盖：包含双引号的表名/列名在 sync diff、captured write、selection push 接收路径。
- 未充分覆盖：baseline apply 的 FK 拓扑顺序、父子表删除/插入顺序。
- 未充分覆盖：普通 changeset ACK 与当前前台 sync batch 的关联。

### 5.3 错误传播路径

- 良好：SyncWorker init error 会映射到更精确错误码，见 `src/sync/SyncEngine.cpp:77`、`src/sync/SyncEngine.cpp:100`。
- 良好：baseline 内部错误会 prefix 为 `E_SYNC_BASELINE_FAILED`，见 `src/sync/baseline/BaselineManager.cpp:216`、`src/sync/baseline/BaselineManager.cpp:218`。
- 良好：Outbox 写入失败会向 SyncWorker/ACK channel 返回错误，见 `src/sync/transport/OutboxWriter.cpp:41`、`src/sync/transport/AckChannel.cpp:36`、`src/sync/transport/AckChannel.cpp:60`。
- 偏差：`InboxWatcher::scan()` 忽略 `ledger_.markSeen()` 失败，见 `src/sync/transport/InboxWatcher.cpp:42`、`src/sync/transport/InboxWatcher.cpp:43`。如果 ledger 写失败，artifact 仍会被处理，幂等账本可能不完整。
- 偏差：`OutboundAckStore::setPendingBaseline()` 对不存在 peer 行只是 UPDATE 0 行且返回 true，见 `src/sync/anchor/OutboundAckStore.cpp:112`、`src/sync/anchor/OutboundAckStore.cpp:115`。如果后续实现 baseline in-flight 依赖该字段，需要确认 row 预创建。

## 第六部分：架构质量

### 6.1 模块职责与耦合

- 优点：ImportService/ExportService、Mapper/FkInjector/Profile、SyncWorker/transport/apply/baseline/diff 分层基本清晰。
- 优点：`SqlBuilder` 承担 upsert 与 auto join SQL 构造，减少业务层直接拼接，见 `src/sql/SqlBuilder.cpp:13`、`src/sql/SqlBuilder.cpp:57`。
- 问题：`SyncWorker` 同时负责 worker 生命周期、任务队列、inbox scan、payload decode、changeset apply、selection push、ACK、baseline request/response、peer 评估，职责过重。相关集中区间见 `src/sync/SyncWorker.cpp:432`、`src/sync/SyncWorker.cpp:468`、`src/sync/SyncWorker.cpp:621`、`src/sync/SyncWorker.cpp:773`、`src/sync/SyncWorker.cpp:828`、`src/sync/SyncWorker.cpp:888`、`src/sync/SyncWorker.cpp:1413`。
- 问题：导入和导出分别定义 `castToAffinity()`、`makeTupleKey()`，实现重复且同样有类型语义缺陷，见 `src/service/ImportService.cpp:43`、`src/service/ImportService.cpp:65`、`src/service/ExportService.cpp:117`、`src/service/ExportService.cpp:110`。

### 6.2 SOLID 原则

- 单一职责：大多数小类如 `OutboxWriter`、`InboxLedger`、`AckChannel`、`AppliedVectorStore` 职责明确。
- 开闭原则：Temporal/Profile/OpenSpec 特性被加入到 ProfileSpec/Mapper/ExportService 中，扩展点存在，但 `ExportService.cpp` 已承担过多路径分支，继续增加导出特性会显著提高回归风险。
- 依赖倒置：SyncEngine 直接持有具体 `SyncWorker`，对测试不利；BatchTransfer 通过 `SyncContext` 间接调 worker，方向较好。
- 接口隔离：`DataBridge` 暴露的 `runImportOnDb()`、`runExportOnDb()` 边界偏宽，应收敛到 internal friend 或 detail API。

### 6.3 可测试性

- 优点：多数 store/helper 是无 UI 纯 C++/Qt SQL 类，适合以临时 SQLite DB 做单元测试。
- 缺口：SyncWorker 逻辑庞大且依赖文件系统 outbox/inbox、QThread、SQLite session，端到端测试成本高。建议抽出 `BaselineRequestCoordinator`、`AckWaitTracker`、`SelectionPushReceiver` 以便独立测试。
- 缺口：OpenSpec 类型严格相等、时间 profile 继承、空字符串 reverse lookup、双引号标识符等应补针对性回归测试。

### 6.4 代码重复

- 重复：导入/导出各自实现 tuple key 和 affinity cast，见 `src/service/ImportService.cpp:43`、`src/service/ImportService.cpp:65`、`src/service/ExportService.cpp:110`、`src/service/ExportService.cpp:117`。
- 重复：多处手工 PRAGMA table_info/pk 发现逻辑，分别在 `src/sync/diff/DiffEngine.cpp:176`、`src/sync/diff/ComparisonSession.cpp:387`、`src/sync/selection/SelectionResolver.cpp:14`、`src/sync/selection/FkClosureBuilder.cpp:31`、`src/sync/SyncWorker.cpp:691`。建议抽成 `SchemaIntrospector` 或 sync schema utility，并统一 quote。
- 重复：temporal export 调用散落在 mixed/columnOrder/reverse path，且都未传 row 号，见 `src/service/ExportService.cpp:746`、`src/service/ExportService.cpp:816`、`src/service/ExportService.cpp:923`。

## 第七部分：总结与优先级修复建议

### Critical（必须修复）

- [C-1] `src/sync/SyncWorker.cpp:1168` — baseline fallback 不发起 `BaselineRequest`，gap 无法自动闭环 — 实现 gap→request artifact→in-flight→response apply→pending rescan 的完整协议。
- [C-2] `src/service/ImportService.cpp:562` — MVP 全有全无导入语义缺失，row-level 错误仍写入其他行 — 明确并实现导入原子性策略；MVP 默认任何错误不写库。
- [C-3] `src/sync/apply/ChangesetApplier.cpp:304` — `INSERT OR REPLACE` 恢复 winner 可能触发 DELETE+INSERT 副作用 — 改为 `ON CONFLICT DO UPDATE` 或 changeset 级补偿。

### High（强烈建议）

- [H-1] `src/sync/apply/CapturedWriteTemplate.cpp:240` — PK 标识符未转义 — 使用 `SqlBuilder::quoteIdent()`。
- [H-2] `src/sync/apply/CapturedWriteTemplate.cpp:275` — 表名未转义 — 使用 `SqlBuilder::quoteIdent()`。
- [H-3] `src/sync/SyncWorker.cpp:696` — PRAGMA 表名未转义 — 使用统一 schema helper。
- [H-4] `src/sync/diff/ComparisonSession.cpp:375` — SELECT 表名/列名未转义 — 使用 `SqlBuilder::quoteIdent()`。
- [H-5] `src/service/ImportService.cpp:65` — lookup tuple key 字符串化破坏严格相等 — 引入 typed tuple key。
- [H-6] `src/service/ExportService.cpp:110` — reverse lookup tuple key 字符串化破坏严格相等 — 与导入共用 typed tuple key。
- [H-7] `src/service/ImportService.cpp:60` — BLOB/NUMERIC/NONE 被转字符串 — 按 OpenSpec affinity 规则修正。
- [H-8] `src/service/ExportService.cpp:383` — 空字符串被当作 reverse lookup miss — 仅 SQL NULL 视为 missing。
- [H-9] `src/profile/ProfileSpec.h:162` — profile `dateFormat` 被 legacy validator 屏蔽 — 按规格决议调整优先级并补测试。
- [H-10] `src/sync/SyncWorker.cpp:904` — 普通 changeset ACK 可完成任意前台 sync wait — ACK wait 需绑定 peer/origin/batch 或 last sent range。

### Medium（建议修复）

- [M-1] `src/service/ExportService.cpp:82` — 导出时间错误 row 固定 0 — 给 `convertTemporalForExport()` 传实际输出行号。
- [M-2] `src/sync/schema/SchemaGuard.cpp:52` — fingerprint 未覆盖 notnull/default/unique/FK — 扩展 fingerprint material。
- [M-3] `src/sync/capture/SqliteHandle.cpp:15` — session hard gate 缺 SQLite source id/动态库一致性诊断 — 初始化时记录并校验。
- [M-4] `src/sync/baseline/BaselineManager.cpp:128` — baseline apply 未按 FK 拓扑删除/插入 — 使用 schema catalog 排序或临时 defer foreign_keys 的受控策略。
- [M-5] `src/sync/transport/InboxWatcher.cpp:43` — `markSeen()` 失败被忽略 — 失败时不要继续处理 artifact，并上报 transport error。
- [M-6] `src/sync/diff/ComparisonSession.cpp:442` — removeDatabase 前仍持有 QSqlDatabase 成员 — close 后置空再 remove。
- [M-7] `src/sync/schema/SchemaEligibility.cpp:79` — 复合 PK 全拒绝限制较强 — 文档化为 MVP 限制，或实现复合 PK selection/diff/apply。
- [M-8] `src/service/ExportService.cpp:809` — columnOrder-only 全量加载 — 可用 header index 映射后流式写出，降低内存峰值。

### Low（可选优化）

- [L-1] `src/sync/selection/ChunkStreamer.cpp:39` — `PayloadCodec` 参数未使用 — 删除参数或把实际编码大小纳入预算。
- [L-2] `src/sync/selection/ChunkStreamer.cpp:40` — `targetPeer` 参数未使用 — 写入 chunk 元数据或从接口移除。
- [L-3] `src/sync/SyncWorker.cpp:713` — columns/values 依赖 QVariantMap 顺序一致 — 显式遍历 keys 并 `rowMap.value(k)`。
- [L-4] `src/sync/anchor/OutboundAckStore.cpp:112` — `setPendingBaseline()` 对不存在 peer 行无效果 — 改 UPSERT 或检查 affected rows。
- [L-5] `src/service/ExportService.cpp:763` — `explicitSql` 信任 profile — 在 API 文档声明 profile 可信边界，或增加只读 SQL/单 SELECT 校验。

总体结论：实现已经覆盖了大部分设计面，尤其是 SyncWorker 单写线程、session 捕获、changelog、ACK、selection push、FK 闭包、导出列顺序/反向查找/时间格式等能力。但当前仍不应视为完全合规：baseline gap 自动恢复、MVP 导入原子性、`INSERT OR REPLACE` 数据完整性、typed lookup equality、以及未统一 quote 的同步 SQL 路径是优先级最高的修复项。
