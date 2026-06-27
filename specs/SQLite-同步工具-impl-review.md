# 代码审查报告（第三十三轮）

## 总览
- 总体评分：72/100
- 审查范围：`src/` 下全部 `.h/.cpp` 源码，共 122 个源文件；对照用户列出的 9 份规范文档逐项审查。
- Critical/High/Medium/Low 问题数量统计：
  - Critical：1
  - High：4
  - Medium：3
  - Low：2
- 特别说明：
  - 未将 `exportOnMissing:"error"` 跳过整行的行为列为问题。`openspec/specs/export-reverse-lookup/spec.md:42` 及 `157-160` 明确该行为符合当前规范和用户说明。
  - 未将测试二进制需要 `LD_LIBRARY_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/lib` 才能运行列为问题。该运行环境约束已按用户说明排除。

整体判断：导入/导出主链路实现完成度较高，`columnOrder`、反向 lookup、`fkInject`、row lookup、time-format 的大部分规则都有明确落点；同步模块也已实现 worker、DDL、变更载荷、ACK、row-winner、baseline、selection push 等大量基础设施。当前阻断交付的风险主要集中在同步前后台生命周期、同步上下文 key 使用不一致、profile 加载契约与 OpenSpec 的“加载即失败”要求不一致，以及 `syncSelected` 的受理前校验语义。

## Critical 问题（必须修复，阻断功能）

### C-01：`SyncEngine::stop()` 直接停止整个后台 `SyncWorker`，违反“只停止前台 operation，后台继续收取/应用/广播”的核心同步语义

- 位置：
  - `src/sync/SyncEngine.cpp:225-247`
  - `src/sync/SyncWorker.cpp:149`（`requestStop()` 终止 worker loop）
- 规范依据：
  - `specs/SQLite-同步工具-设计文档.md:364`：`stop()` 仅中止当前前台 operation，不停后台收取；后台 `SyncWorker` 即使不调用 `sync()` 也应周期扫 inbox/广播。
  - `specs/SQLite-同步工具-设计文档.md:366-369`：前台 operation 与后台失败/完成状态分流，终态后释放 `ForegroundGate`。
- 问题说明：
  - 当前 `stop()` 在已初始化状态下调用 `worker_->requestStop()` 并 `wait(3000)`，随后清空 `ctx_->importFn / workerWriteFn / workerCaptureWriteFn / rescanFn`。
  - 这不是“停止前台 operation”，而是把同步后台管线整体下线。
- 影响：
  - `stop()` 后 inbox 扫描、入站 apply、ACK、outbox 周期广播都停止。
  - 同步上下文仍可能存在，但关键回调被清空，后续批量导入/对比会话无法再经 worker 写通道工作。
  - 使用者无法通过公开 API 区分“取消当前前台任务”和“关闭同步引擎”，与规范的前后台双面模型相冲突。
- 建议：
  - 将 `stop()` 改为只取消当前前台 drain / selection push：取消 ACK wait、标记当前 foreground operation 为 `Stopped`、释放 gate。
  - 保持 worker loop、inbox scan、apply、ACK、周期广播继续运行。
  - 如确需整体下线，应放到析构或单独内部 shutdown 路径，不复用公开 `stop()`。

## High 问题（严重，影响正确性）

### H-01：`loadProfile*()` 只解析并缓存 profile，未执行规范要求的 schema / cross-field validation

- 位置：
  - `src/DataBridge.cpp:79-85`
  - `src/DataBridge.cpp:122-149`
  - `src/DataBridge.cpp:378-388`
  - `src/service/ImportService.cpp:447-448`
  - `src/profile/ProfileValidator.cpp:41-305`、`334-524`
- 规范依据：
  - `specs/Qt-SQLite-Excel-批量导入导出-设计文档.md:174-177`：`loadProfile` 与 `loadProfileFromString` 共用 schema 校验。
  - `specs/Qt-SQLite-Excel-批量导入导出-设计文档.md:804`：两条加载路径走完全相同的 Schema 校验与执行流水线。
  - `openspec/specs/export-column-order/spec.md:17-29`、`39-45`、`101-107`：未知 header、重复 columnOrder、raw SQL 互斥等要求 profile loading 失败。
  - `openspec/specs/fk-injection/spec.md:104-112`：`fkInject` 多父、多列、来源/目标列合法性要求 profile validation 失败。
  - `openspec/specs/row-lookup/spec.md:197-205`：Excel / lookup / fkInject 三来源 dbColumn 唯一性由 profile validation 负责。
  - `openspec/specs/export-reverse-lookup/spec.md:18-20`、`66-68`、`176-178`、`188-190`：非法 reverse lookup 声明、非法 `exportOnMissing`、消失 H 列出现在 `columnOrder`、级联 reverse lookup 均要求加载失败。
- 问题说明：
  - `DataBridgePrivate::loadProfileDoc()` 只调用 `ProfileLoader::load()`，随后直接 `profiles_[spec.name] = spec`。
  - 大量跨字段、跨 schema 的校验已经实现于 `ProfileValidator`，但只在 `ImportService::run()` 或 `DataBridge::exportExcel()` 入口执行。
  - `BatchTransfer::startImport()` 通过 `snapshotProfileCatalog()` 取得 profile 后，也是在后台导入执行时才暴露验证错误。
- 影响：
  - 调用方会认为非法 profile 已成功加载，直到导入/导出时才失败。
  - OpenSpec 中明确要求“profile loading SHALL fail”的错误被延迟，破坏 API 契约和用户反馈时机。
  - 对需要在 UI 中提前校验配置的宿主程序不友好，且更难定位是 profile 错误还是执行期数据错误。
- 建议：
  - 在 `loadProfileDoc()` 中，如果数据库已打开，刷新 catalog 并运行 `ProfileValidator` 的 schema/cross-field 校验。
  - 对需要 Excel header 的 import-only 校验，可保留到导入时；但不依赖 Excel 文件的规则必须在加载时失败。
  - 若必须支持“未打开数据库时加载 profile”，应明确拆分为 parse-only API 与 validate/load API，而不是让 `loadProfile*()` 静默接受不可执行 profile。

### H-02：同步上下文注册已用 resolved main path，但 `BatchTransfer` 和 `SyncWorker` 仍用原始 `dbPath/sqlitePath` 查询 context，URI/别名场景会绕过同步写通道

- 位置：
  - `src/sync/SyncEngine.cpp:54-85`：初始化时已通过临时连接 `PRAGMA database_list` 解析 main 库路径再 `getOrCreate()`。
  - `src/batch/BatchTransfer.cpp:60-70`、`110-119`、`146-170`：仍使用 `bridge_.dbPath()` 调 `getExisting()`。
  - `src/sync/SyncWorker.cpp:357`：worker 发布 `canonicalSyncTables/streamEpoch/contextUuid` 时仍使用 `config_.sqlitePath()` 调 `getExisting()`。
  - `src/sync/diff/ComparisonSession.cpp:527`：对比会话依赖 `config.sqlitePath()` 查 context。
  - `src/DataBridge.cpp:252-254`：`dbPath()` 返回的是 `QSqlDatabase::databaseName()`，不是 resolved main path。
- 规范依据：
  - `specs/SQLite-同步工具-设计文档.md:395-405`：同一 `.db` 必须解析到同一 `SyncContext`，key 通过 `PRAGMA database_list` 的 main path 与 OS 文件标识加固，避免 URI、相对路径、符号链接、硬链接等别名拆出多个 context。
  - `specs/SQLite-同步工具-plan.md:74`、`78`、`116`：路径别名指向同一库只能建一个 context；旧导入对同步表应被拒绝或改道到同步写通道。
- 问题说明：
  - `SyncEngine::initialize()` 的注册路径已经较正确，但其它模块查找 context 时没有复用 `canonicalKey_` 或 resolved main path。
  - 对普通绝对路径通常能命中；但如果 `ConnectionSpec.sqlitePath` 是 SQLite URI、相对路径在不同工作目录解析、或其它别名形式，`getExisting(rawPath)` 可能失败。
  - `BatchTransfer::runImport()` 找不到 context 时会回退到直接打开数据库并调用 `ImportService`，不会经过 `SyncWorker` / `CapturedWriteTemplate`。
- 影响：
  - 同步激活时，批量导入可能绕过 session 捕获、changelog、table_state、row_winner 和单写者队列。
  - `startImport()` / `startExport()` 也可能未取得同一个 `ForegroundGate`，破坏同库前台互斥。
  - worker 发布的 `canonicalSyncTables`、`streamEpoch`、`contextUuid` 可能写不到已注册 context，影响对比会话和诊断。
- 建议：
  - 在 `DataBridge` 保存 resolved main path 或 registry canonical key，并让 `dbPath()` 或新增内部 API 返回同步层统一 key 输入。
  - `BatchTransfer`、`SyncWorker`、`ComparisonSession` 不应自行用 raw path 查询 context。
  - 增加 URI、相对路径、symlink/hardlink 指向同库的回归测试，断言只命中一个 context 且导入必经 worker。

### H-03：`syncSelected()` 对空选择没有做受理前同步校验，会错误占用前台 gate 并异步失败

- 位置：
  - `include/dbridge/sync/SyncSelection.h:83-107`
  - `src/sync/SyncEngine.cpp:275-299`
  - `src/sync/SyncWorker.cpp:1835-1850`
- 规范依据：
  - `specs/SQLite-同步工具-设计文档.md:366-369`：空 selection 属于“受理前同步校验”，应经返回值和 `*err` 同步返回，不占前台槽。
  - `specs/SQLite-同步工具-设计文档.md:1097-1100`：空选择/Builder 非法时 `syncSelected()` 返回 `false + E_SYNC_SELECTION_EMPTY`。
  - `specs/SQLite-同步工具-plan.md:139`：`syncSelected` 的受理前校验同步返回。
- 问题说明：
  - `SyncSelection::Builder::build()` 对空选择只设置 `*err`，仍返回空 `SyncSelection`。
  - 如果调用方忽略 builder 的 `err` 并传入空 selection，`SyncEngine::syncSelected()` 不检查 `selection.isEmpty()`，直接获取 gate、进入 `Capturing`、启动 ACK wait 并投递 worker。
  - worker 后续才在 `SelectionResolver` 失败或 `resolved.isEmpty()` 时发出 `E_SYNC_SELECTION_EMPTY`。
- 影响：
  - 空选择从“同步拒绝、不占槽”变成“已受理、占 gate、异步失败”。
  - UI / 调用方会看到错误的状态转换，且在 worker 处理前可能阻塞其它前台操作。
  - 与规范中对错误分流的核心约定相反。
- 建议：
  - 在 `SyncEngine::syncSelected()` 入口添加 `selection.isEmpty()` 检查，直接返回 false，并通过 `err` 写入 `E_SYNC_SELECTION_EMPTY`。
  - Builder 非法表名 / raw where 产生的空选择也应能被 engine 层兜底拒绝，避免调用方忽略 builder error 后进入后台。

### H-04：row-winner 的 `pk_hash` 生成仍存在多套编码，baseline / changeset / captured write 可能不在同一 key 空间

- 位置：
  - `src/sync/apply/RowWinnerStore.cpp:107-110`
  - `src/sync/apply/ChangesetApplier.cpp:28-71`
  - `src/sync/apply/CapturedWriteTemplate.cpp:264-272`
  - `src/sync/apply/CapturedWriteTemplate.cpp:568-600`
  - `src/sync/baseline/BaselineManager.cpp:414-422`
- 规范依据：
  - `specs/SQLite-同步工具-plan.md:100-106`：`RowWinnerStore` 维护逐行胜者，崩溃夹具需断言 `row_winner` 原子一致。
  - `specs/SQLite-同步工具-plan.md:141`：多源跨批、低 rank 后到、反序到达后仍应收敛中心终态。
  - `specs/SQLite-同步工具-设计文档.md:940-945`：`__sync_row_winner` 以 `(table_name, pk_hash)` 标识同一业务行。
- 问题说明：
  - `RowWinnerStore::pkHash()` 已改为 `TableStateStore::rowHash(QVariantMap)` 的类型标记编码。
  - 但 `ChangesetApplier` 和 `CapturedWriteTemplate` 仍用 `value + '\0'` 拼接后 `SHA256.left(16)` 的方式生成 `pk_hash`。
  - `BaselineManager` seeding 使用 `RowWinnerStore::pkHash(pkMap)`，与 changeset/captured write 的实际生成方式不一致。
- 影响：
  - baseline 后 seed 的 winner 与后续 changeset challenger 可能用不同 `pk_hash` 表示同一行，低 rank 后到保护失效。
  - `value + '\0'` 缺少列名、类型标签、长度前缀，存在可构造碰撞或跨类型等价风险。
  - 逐行胜者裁决是同步收敛的核心，不一致会造成跨批冲突裁决不稳定。
- 建议：
  - 抽出唯一的 `canonicalPkHash(table, pkColumns, values)`，使用列名、类型标签和长度前缀。
  - `ChangesetApplier`、`CapturedWriteTemplate`、`BaselineManager`、`RowWinnerStore` 全部调用同一实现。
  - 增加 baseline 后低 rank DELETE/UPDATE 到达、复合 PK、数值字符串混合类型的回归测试。

## Medium 问题（中等，影响质量）

### M-01：`SchemaEligibility` 硬拒复合主键，超出设计文档写明的 eligibility 条件

- 位置：
  - `src/sync/schema/SchemaEligibility.cpp:70-88`
- 规范依据：
  - `specs/SQLite-同步工具-设计文档.md:413-422`：同步表要求普通表、显式非空 PRIMARY KEY（或 sync key）、可用冲突目标；未声明只支持单列 PK。
  - `specs/SQLite-同步工具-plan.md:75`：任务描述为“显式非空 PK + 可用冲突目标”，同样未写复合 PK 一律拒绝。
- 问题说明：
  - 当前 `info.pkCols.size() > 1` 时直接拒绝，并返回 `E_SYNC_COMPOSITE_PK_NOT_SUPPORTED`。
  - 代码注释称这是 MVP 限制，但该限制没有出现在用户列出的同步设计/计划 eligibility 条款中。
- 影响：
  - 合法 SQLite 同步表如果使用复合主键，会在初始化阶段被拒绝。
  - 与 ETL 侧已支持复合 conflict / 多列 lookup / 多列 fkInject 的能力不一致。
- 建议：
  - 若产品确实暂不支持复合 PK，应把该限制补进规范，并明确是同步 MVP 非目标。
  - 若按现有规范实现，应移除硬拒绝，并复用统一的复合 PK 编码、UPSERT conflict target 和 selection resolver 支持。

### M-02：`ForeignKeyPreflight` 的 in-batch parent cache 包含已失败父 route 的 payload，可能让子 route 误判 FK 已存在

- 位置：
  - `src/validation/ForeignKeyPreflight.cpp:16-20`
  - `src/validation/ForeignKeyPreflight.cpp:102-120`
  - `src/service/ImportService.cpp:560-573`
  - `src/service/ImportService.cpp:716-719`
- 规范依据：
  - `openspec/specs/fk-injection/spec.md:90-98`：父 route 行级错误时，parent chain 上的子 route 应 drop，且不产生派生重复错误。
  - `openspec/specs/fk-injection/spec.md:50-65`：FK preflight 的 in-batch hit 应基于可生成的 parent payload。
- 问题说明：
  - `ForeignKeyPreflight::check()` 构建 `batchParentPayloads` 时把所有 `ctx.payloads` 放入 cache，没有过滤 `ctx.failedRouteIndices`。
  - 子 payload 在检查 `fk.fromTable` 时可能命中一个已经因 mapper/lookup/fkInject 错误而不会写入的 parent payload。
  - 写阶段只按 `parent` 链做 descendant suppression；如果 `fkInject.from` 表不在 `parent` 链上，或者多父注入中某个 from route 失败，子 route 仍可能进入写阶段。
- 影响：
  - 子表可能通过 preflight，但实际父表未落库，最终在 DB FK 约束处失败，或在无 FK 约束时落入悬挂引用。
  - 错误定位会从规范要求的 route-local/cascade 语义退化为写阶段数据库错误。
- 建议：
  - 构建 `batchParentPayloads` 时跳过对应 `ctx.failedRouteIndices` 的 payload。
  - 对 `fkInject.from` 依赖单独建立 failure closure，不只依赖 `parent` 字段链。
  - 增加“父 lookup 失败 + 子通过 fkInject 引用该父但 parent 字段不是该表”的多父用例。

### M-03：同步 context 的 worker 发布路径失败时没有显式 fatal，容易形成“初始化成功但共享状态未发布”的半初始化状态

- 位置：
  - `src/sync/SyncWorker.cpp:350-377`
  - `src/sync/SyncContext.cpp:91-105`
- 规范依据：
  - `specs/SQLite-同步工具-设计文档.md:395-405`：同库共享 context 是 gate、写线程和元数据发布的基础。
  - `specs/SQLite-同步工具-plan.md:112-116`：8 getter/counters、table_state、路径别名同 context 为 M1 DoD。
- 问题说明：
  - worker 使用 `getExisting(config_.sqlitePath())` 查 context；若因 URI/别名/raw path 失败，代码只是跳过 context 更新，并不设置 `initError_`。
  - `canonicalSyncTables`、`streamEpoch`、`contextUuid` 发布失败后，`initialize()` 仍可能返回成功。
- 影响：
  - `ComparisonSession`、`BatchTransfer`、诊断工具看到的 context 元数据可能为空或旧值。
  - 难以从 public API 感知初始化不完整。
- 建议：
  - worker 应持有 `SyncEngine` 初始化时得到的 canonical key 或 resolved path。
  - context 发布失败应作为 `E_SYNC_INIT` 或专门错误返回，而不是静默继续。

## Low 问题（轻微，改善建议）

### L-01：SQLite 版本检查包含不可达条件，表达不严谨

- 位置：
  - `src/DataBridge.cpp:41-52`
- 问题说明：
  - `patch` 来自 `QString::toInt()`，`patch < 0` 对正常 `sqlite_version()` 永远不可达。
  - 该判断不会造成功能错误，但降低了版本检查代码的可信度。
- 建议：
  - 改为明确比较 `(major, minor, patch) < (3, 24, 0)`。
  - 同时处理 `parts.size() < 3` 或非数字解析失败，避免异常版本字符串被误判。

### L-02：`FkInjector::inject(QVector<RoutePayload>&, QString*)` 保留为可调用 stub，存在误用风险

- 位置：
  - `src/mapping/FkInjector.cpp:10-19`
- 问题说明：
  - 该重载没有 route context，当前实现返回 false 并提示调用方必须使用 RouteSpec-aware overload。
  - 如果未来新调用点忽略返回值，仍可能以“已调用 inject”的形式继续执行，造成 FK 注入缺失。
- 建议：
  - 删除该重载，或在头文件中将其移入 private / `= delete`。
  - 保留唯一带 `routes/excelRow/sheet/errors/initialFailed` 的实现入口。

## 总结

当前实现不属于“缺少主体功能”的状态，ETL 主链路和同步基础设施已经有较完整实现；SQL 值绑定、identifier quoting、事务、批量 lookup prefetch、time-format 解析/序列化、`columnOrder` 后置列序、反向 lookup 行级容错等关键点总体方向正确。

交付前建议按以下顺序修复：

1. 先修 C-01：把 `stop()` 从“停止 worker”改成“停止当前前台 operation”，保持后台同步管线常驻。
2. 修 H-02 / M-03：统一 SyncContext key 的传递与查询路径，所有模块只使用 resolved main path / canonical key。
3. 修 H-01：把不依赖 Excel 文件的 profile validation 前移到 `loadProfile*()`。
4. 修 H-03：`syncSelected()` 入口同步拒绝空 selection，不占 gate。
5. 修 H-04：统一 `pk_hash` 生成函数，并补 baseline/changelog/low-rank 后到回归。
6. 再处理 M/L 项，尤其是复合 PK 是否纳入同步 MVP 的规范与实现一致性。

本轮未记录 `exportOnMissing:"error"` 行跳过为问题，也未记录 Qt 测试运行库路径为问题。
