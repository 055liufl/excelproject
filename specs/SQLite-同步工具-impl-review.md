# SQLite 同步工具实现深度审查报告

审查日期：2026-06-26  
审查范围：`src/` 全部源代码，对照以下规范：

- `specs/Qt-SQLite-Excel-批量导入导出-设计文档.md`
- `specs/MVP-Qt-SQLite-Excel-批量导入导出-实现设计.md`
- `specs/SQLite-同步工具-设计文档.md`
- `specs/SQLite-同步工具-plan.md`
- `openspec/specs/export-column-order/spec.md`
- `openspec/specs/export-reverse-lookup/spec.md`
- `openspec/specs/fk-injection/spec.md`
- `openspec/specs/row-lookup/spec.md`
- `openspec/specs/time-format/spec.md`

## 总体结论

当前实现已经补齐不少基础能力：`context_uuid` 的读或写语义已修正，接收端 `push_progress` 重复分片状态回退已用 `CASE WHEN` 保护，`export.columnOrder`、lookup/fkInject 校验、时间格式的大部分 loader 规则、同步 session 可用性自检、表态增量维护等都有实际落点。

但仍不满足“可按规范交付”的标准。剩余主要风险集中在同步身份模型、反向 lookup route-local 语义、row-winner PK 哈希一致性、selection push 分片 origin/epoch 绑定，以及当前测试环境无法执行。

## 重点修复闭合状态

| 项 | 状态 | 结论 |
|---|---|---|
| H-01 `context_uuid ensureContextUuid` 读取-或-写入语义 | 已闭合 | `SyncContextRegistry::ensureContextUuid()` 现在读取既有 `__sync_context_meta.context_uuid` 并回填传入 uuid，仅首次为空时写入。见 `src/sync/SyncContext.cpp:117`、`src/sync/SyncWorker.cpp:363`。 |
| H-02 `SyncContext` key 是否基于 `PRAGMA database_list` | 未闭合 | registry 仍在 worker 打开 SQLite 前直接对传入 `sqlitePath` 做 `stat()`/文件标识。没有通过已打开连接读取 `PRAGMA database_list` 的 `main` 路径。见 `src/sync/SyncEngine.cpp:52`、`src/sync/SyncContext.cpp:22`、`src/sync/SyncWorker.cpp:167`。 |
| H-03 `exportOnMissing:"error"` 规范行为 | 未闭合 | 规范要求 route-local：声明该 lookup 的 route 本行跳过，其他 route 贡献不受影响。当前 `resolveAHeaders()` 仍设置整条输出行 `rowSkip=true` 并返回。见 `src/service/ExportService.cpp:436`、`src/service/ExportService.cpp:480`、`src/service/ExportService.cpp:504`。 |
| H-04 `push_progress ON CONFLICT CASE WHEN` 保护 | 接收端已闭合 | 接收 selection push 分片时 `ON CONFLICT(push_id)` 已保护 `done/failed` 不回退到 `streaming`。见 `src/sync/SyncWorker.cpp:723`。发起端仍有无条件 `status='streaming'`，但常规路径使用新 UUID，风险低。 |
| M-01 `RowWinnerStore::pkHash` 是否使用 `TableStateStore::rowHash` | 部分闭合 | `RowWinnerStore::pkHash()` helper 已改用 `TableStateStore::rowHash()`，但实际 changeset、CapturedWriteTemplate、BaselineManager 仍各自使用 `value + '\0'` 材料计算 pkHash。见 `src/sync/apply/RowWinnerStore.cpp:107`、`src/sync/apply/ChangesetApplier.cpp:28`、`src/sync/apply/CapturedWriteTemplate.cpp:265`、`src/sync/baseline/BaselineManager.cpp:358`。 |
| M-02 push chunk origin 校验 | 部分闭合 | `processSelectionPushArtifact()` 只校验已存在 `push_id` 的 `origin`，未持久绑定/校验 `stream_epoch`，DDL 主键仍只有 `push_id` 或 `(push_id, chunk_seq)`。见 `src/sync/SyncDDL.h:101`、`src/sync/SyncWorker.cpp:741`。 |

## Critical

无新的 Critical。当前问题里没有看到会在普通单进程、规范路径下必然造成不可恢复数据破坏的单点；但 High 项足以阻断交付。

## High

### H-02：`SyncContext` 身份没有基于 SQLite 实际 main 库路径

- 位置：`src/sync/SyncEngine.cpp:52`、`src/sync/SyncContext.cpp:22-64`、`src/sync/SyncWorker.cpp:167-170`
- 违反规范：同步设计 §2.4 / plan T1.0b 要求通过 SQLite 连接解析主库真实路径，再以 OS 文件标识作为 registry key。
- 问题：`getOrCreate(config.sqlitePath())` 在 worker 打开数据库前执行，`canonicalKey()` 只对调用方传入路径做 `stat()`。URI、相对路径、SQLite 解析后的 main 路径、未创建数据库路径都没有按规范收口。
- 影响：同一物理库可能被不同别名拆成多个 `SyncContext`/写线程，破坏单写者和前台互斥；也可能错误拒绝 SQLite 本可创建的新库。
- 建议：先用临时连接打开数据库，读取 `PRAGMA database_list` 中 `main` 的路径，再生成 OS identity key；新库/临时库场景按规范使用临时 path key 并在创建后升级。

### H-03：反向 lookup 的 `exportOnMissing:"error"` 仍是整行跳过

- 位置：`src/service/ExportService.cpp:436-546`、`src/service/ExportService.cpp:773-779`、`src/service/ExportService.cpp:955-963`
- 违反规范：`openspec/specs/export-reverse-lookup/spec.md` 明确要求 `"error"` 跳过 declaring route 的行，其他 routes contributing to the same Excel row unaffected。
- 问题：`resolveAHeaders()` 在 NULL H 值或 miss 时设置 `*rowSkip=true` 并立即返回；调用点虽传入 `failedAHeaders`，但函数从未写入该集合。注释与实现相互矛盾。
- 影响：MultiTable/Mixed 导出中，一个 route 的反查 miss 会丢掉同一 Excel 行内其他 route 可导出的数据。
- 测试问题：`tests/unit/tst_reverse_lookup_export.cpp:449-486` 仍把“整行跳过”写成期望，测试本身与规范冲突。
- 建议：把 reverse lookup resolution 改成 route-local 结果模型，例如返回失败 route/header 集合；`E_REVERSE_LOOKUP_NOT_FOUND` 只屏蔽该 route 恢复出的 A 列或该 route 的投影，不得跳过整条 Mixed/MultiTable 输出行。同步修正测试。

### H-05：Row winner 的实际 pkHash 编码仍不一致

- 位置：`src/sync/apply/RowWinnerStore.cpp:107-110`、`src/sync/apply/ChangesetApplier.cpp:28-71`、`src/sync/apply/CapturedWriteTemplate.cpp:265-272`、`src/sync/apply/CapturedWriteTemplate.cpp:568-600`、`src/sync/baseline/BaselineManager.cpp:358-426`
- 违反规范：plan G-01/R-01/M-01 的逐行胜者状态要求同一行在 changeset、baseline、row_winner 中有统一、无构造碰撞的 PK 身份。
- 问题：helper `RowWinnerStore::pkHash()` 已使用 `TableStateStore::rowHash()`，但实际写入 `__sync_row_winner` 的主路径没有调用它。changeset 路径和 baseline seeding 仍使用未带长度/类型/列名标签的 `value + '\0'` 拼接。
- 影响：不同路径可能为同一 PK 产生不同 pkHash；也可能出现可构造碰撞。baseline 后 seeded winner 与后续 changeset winner 不在同一 key 空间时，低 rank 后到保护会失效。
- 建议：新增一个唯一的 `canonicalPkHash(table, pkColumns, values)`，使用列名排序、类型标签、长度前缀；changeset iterator、CapturedWriteTemplate、BaselineManager、RowWinnerStore 全部调用它。

## Medium

### M-02：selection push 分片幂等键未绑定 `stream_epoch`

- 位置：`src/sync/SyncDDL.h:101-124`、`src/sync/SyncWorker.cpp:741-759`
- 违反规范：分片幂等键应为 `(origin, stream_epoch, push_id, chunk_seq)`，或证明 `push_id` 全局唯一且持久绑定 origin/epoch。
- 问题：DDL 中 `__sync_push_progress` 主键为 `push_id`，`__sync_push_chunk_progress` 主键为 `(push_id, chunk_seq)`；处理分片时只读取并校验 `origin`，没有校验 `hdr.streamEpoch`。
- 影响：跨 epoch 重用同一 `push_id` 时，进度和 checksum 可混入旧 push。UUID 碰撞概率低，但协议层没有规范要求的防线。
- 建议：为 push progress 表加入 `stream_epoch`，唯一键至少覆盖 `(origin, stream_epoch, push_id)`，chunk 表覆盖 `(origin, stream_epoch, push_id, chunk_seq)`；兼容迁移时对旧表补列并回填。

### M-03：测试套件当前无法执行，阻断回归验证

- 位置：构建/运行环境，影响全部测试。
- 现象：在 `build/` 下执行 `ctest --output-on-failure`，17/17 测试全部失败，均为 Qt 符号解析错误，例如 `_Z13qgpu_featuresRK7QString, version Qt_5` 或 `qgpu_features_ptr, version Qt_5`。
- 影响：无法用现有单测验证 columnOrder、reverse lookup、time-format、lookup/fkInject、导入导出回归；当前报告只能基于静态审查和局部命令验证。
- 建议：修正 Qt 运行库路径/链接一致性，确保测试二进制加载与编译时一致的 Qt 5.12.12 运行库。

### M-04：`time-format` 的 `db.fallback` 未被拒绝

- 位置：`src/profile/ProfileLoader.cpp:124-149`
- 违反规范：`fallback` 只能出现在 `excel` side，且只在 Excel→内存解析方向生效。
- 问题：`parseTemporalSide()` 对任意 side 都解析并接受 `fallback`，没有在 `sideName == "db"` 时拒绝。
- 影响：非法 profile 会被接受，旧版本/新版本 profile 的前向错误行为不稳定；用户可能误以为 DB→内存解析会使用 db fallback。
- 建议：在 loader 中对 `db.fallback` 直接 `E_PROFILE_PARSE`，错误信息注明 fallback 仅允许在 excel side。

## Low

### L-01：selection push 发起端的 `push_progress` upsert 仍无状态保护

- 位置：`src/sync/SyncWorker.cpp:1898-1914`
- 问题：发起端创建 push progress 的 `ON CONFLICT(push_id)` 仍无条件 `status='streaming'`。常规路径每次生成新 UUID，冲突概率极低，因此不是 H-04 的主风险。
- 建议：为一致性也改成接收端同款 `CASE WHEN status IN ('done','failed') THEN status ELSE 'streaming' END`。

### L-02：代码注释中仍保留与规范相反的反向 lookup 表述

- 位置：`src/service/ExportService.cpp:436-440`、`src/service/ExportService.cpp:957-958`
- 问题：函数头注释说 NOT_FOUND 会整行跳过；调用点注释说只收集 per-lookup failures。两处都不足以作为规范依据，且会误导后续修复。
- 建议：修复 H-03 时同步改注释，明确 route-local 行为。

## 已符合或基本符合的点

- `export-column-order`：loader 拒绝非数组，validator 覆盖未知 header、重复项、raw SQL 互斥、classColumn 显式/默认位置；导出路径用 `reorderHeaders()` 保持列值身份。
- `fk-injection`：数组形式、旧对象形式拒绝、复合/多父注入、冲突值按列名更新、NULL 父值报 `E_VALIDATE_FK`、preflight 复合谓词与 lookup-derived group skip 均有实现。
- `row-lookup`：lookup identity 合并、批量预取、严格 cardinality、空 key、类型 affinity、route-local 失败和 cascade suppression 有实现。
- `time-format`：legacy/new side-object、side-level overwrite、epochSec 限定、格式 token 校验、有效 spec 校验基本实现；剩余缺口为 `db.fallback`。
- 同步 apply：严格连续/GAP_PENDING、schema guard、appendForward 保留 origin、ACK timeout、session self-check、同步表 eligibility 均有实现落点。

## 验证记录

- `ctest --output-on-failure`（工作目录 `build/`）：失败。17 个测试均因 Qt 符号解析错误无法启动。
- 由于测试环境失败，本报告未声称任何自动测试通过。

## 建议修复顺序

1. 修 H-02：先把 `SyncContext` key 改为基于 `PRAGMA database_list` 的 main 库 OS identity。
2. 修 H-03：把 reverse lookup miss/ambiguous 语义改为 route-local，并更新冲突测试。
3. 修 H-05：统一所有 pkHash 生成点，尤其是 changeset 与 baseline seeding。
4. 修 M-02：push progress/chunk progress schema 加入 origin+epoch 绑定并迁移旧表。
5. 修测试运行环境，恢复 17 个测试的可执行性。
6. 补 M-04/L 项和相应 profile loader 单测。
