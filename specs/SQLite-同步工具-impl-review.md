# 代码审查报告 — 第三十一轮

## 总览

- 审查范围：`src/` 目录下全部源文件，共 122 个文件，约 17,676 行；对照 9 个指定规范文档逐项审查。
- 总体评分：80 / 100
- 问题数量统计：Critical 0，High 4，Medium 2，Low 2。
- 回归结论：上一轮的 Session 运行期自检、`table_state` 规范行编码、无符号模加、导入后增量维护、auto profile draft 输出已基本闭合；但 `SyncContext` 身份修复引入重启后 UUID 不匹配，反向 lookup 的 route-local 语义仍未闭合，selection push 完成状态会被重复分片回退。

## Critical 问题

本轮未确认到 Critical 级别问题。当前实现有多处 High 级正确性问题，建议发布前修复。

## High 问题

### H-01：`context_uuid` 持久化实现会导致进程重启后同步初始化失败

- 文件位置：`src/sync/SyncContext.cpp:67`、`src/sync/SyncContext.cpp:84`、`src/sync/SyncContext.cpp:117`、`src/sync/SyncWorker.cpp:357`
- 违反规范：`specs/SQLite-同步工具-设计文档.md` §4.3；`specs/SQLite-同步工具-plan.md` T1.0b / R-06。
- 问题描述：`getOrCreate()` 每次新建进程内 `SyncContext` 都生成新的随机 `contextUuid`，`ensureContextUuid()` 却要求库内已存在的 `context_uuid` 必须等于这个新随机值。首次初始化会写入 UUID；进程退出后再次初始化同一个库时，新 UUID 与库内旧 UUID 必然不一致，`SyncWorker` 报 `E_SYNC_CONTEXT_UUID_MISMATCH`。
- 影响：同步库一旦写入过 `__sync_context_meta.context_uuid`，后续进程重启可能无法再进入同步模式。这不是兜底校验，而是把持久身份当成一次性随机会话 ID。
- 修复建议：`ensureContextUuid()` 应返回库内已存在 UUID 并回填到 `ctx->contextUuid`；仅当同一个 OS 文件身份下发现两个不同的库内 UUID，或同一进程内已有 context 与库内 UUID 不一致时才报错。首次没有 UUID 时才生成并写入。

### H-02：`SyncContext` key 仍未按 SQLite 实际 main 库路径解析，URI/别名场景未闭合

- 文件位置：`src/sync/SyncContext.cpp:22`、`src/sync/SyncContext.cpp:54`、`src/sync/SyncEngine.cpp:52`
- 违反规范：`specs/SQLite-同步工具-设计文档.md` §4.3；`specs/SQLite-同步工具-plan.md` T1.0b DoD。
- 问题描述：`canonicalKey()` 仍直接对调用方传入的 `sqlitePath` 做 `stat()` / Windows file identity，未在打开 SQLite 后通过 `PRAGMA database_list` 读取 `main` 库解析路径。规范明确要求先以 SQLite 连接解析 URI、相对路径和主库真实路径，再使用 OS 文件身份作为 registry key。
- 影响：`file:foo.db?...` URI、相对路径、SQLite 解析后的路径、尚未创建库路径等情况下仍可能无法合并为同一个 `SyncContext`，破坏同库单写线程和前台互斥假设；也可能错误拒绝 SQLite 本可创建的新库。
- 修复建议：把 registry 获取延后到 `QSqlDatabase` 打开后，基于 `PRAGMA database_list` 中 `main` 的实际路径生成 key；对未建库的临时路径按规范使用受限的临时 path key，并在建库后升级为 OS identity。

### H-03：反向 lookup `exportOnMissing:"error"` 仍跳过整条 Excel 行

- 文件位置：`src/service/ExportService.cpp:441`、`src/service/ExportService.cpp:480`、`src/service/ExportService.cpp:520`、`src/service/ExportService.cpp:778`、`src/service/ExportService.cpp:962`
- 违反规范：`openspec/specs/export-reverse-lookup/spec.md` 中 `exportOnMissing` route-local 语义。
- 问题描述：`resolveAHeaders()` 在 NULL H 值或未命中 G 表时仍设置 `*rowSkip = true` 并返回；调用方随后 `continue`，整条 Excel 行被跳过。虽然调用方传入了 `failedAHeaders`，但 miss 分支没有填充该集合，注释“只空出失败列”与实际行为相反。
- 影响：MultiTable/Mixed 导出时，一个 route 的 lookup miss 会丢掉同一 Excel 行中其他 route 已可导出的字段，违反“other routes contributing to the same Excel row are unaffected”。当前 `tests/unit/tst_reverse_lookup_export.cpp` 还把“整行跳过”写成期望，测试本身也与规范冲突。
- 修复建议：把 NOT_FOUND / NULL H 的 `error` 分支改为 route-local 失败：记录失败 lookup 的 A headers 或 route id，输出时仅清空该 route 贡献的 A cells；只有 ambiguity 或结构性不可恢复错误才整行跳过。

### H-04：重复 selection push 分片会把已完成 `push_progress` 回退为 `streaming`

- 文件位置：`src/sync/SyncWorker.cpp:717`、`src/sync/SyncWorker.cpp:724`、`src/sync/apply/CapturedWriteTemplate.cpp:196`、`src/sync/SyncWorker.cpp:1210`
- 违反规范：`specs/SQLite-同步工具-设计文档.md` §5.5 / §6；`specs/SQLite-同步工具-plan.md` T2.9。
- 问题描述：`processSelectionPushArtifact()` 对任意到达分片执行 `ON CONFLICT(push_id) DO UPDATE SET status='streaming'`。如果一个已完成 push 的重复分片到达，`CapturedWriteTemplate` 会因 `push_chunk_progress` 已 applied 而幂等 no-op 返回，不会重新把 `push_progress` 标回 `done`。随后 `broadcastTopeer()` 看到该 `push_id` 状态不是 `done/failed`，会继续跳过相关 changelog。
- 影响：一次重复投递即可把已完成 push 重新置为未完成状态，导致该 push 产生的 changeset 被永久挡在广播屏障后，中心到其他节点不再收敛。
- 修复建议：`ON CONFLICT` 时不要无条件覆盖 `done/failed`；可使用 `WHERE status NOT IN ('done','failed')`，或在发现分片已 applied 且全部 chunks 已 applied 时重新保持/恢复 `done`。

## Medium 问题

### M-01：`__sync_row_winner.pk_hash` 仍使用非规范编码，可能串行化碰撞

- 文件位置：`src/sync/apply/ChangesetApplier.cpp:43`、`src/sync/apply/ChangesetApplier.cpp:58`、`src/sync/apply/RowWinnerStore.cpp:611`、`src/sync/baseline/BaselineManager.cpp:360`
- 违反规范：`specs/SQLite-同步工具-设计文档.md` §5.6 / §6.1；`specs/SQLite-同步工具-plan.md` R-01。
- 问题描述：`extractHashMaterials()` 将 PK/内容值直接拼接 `bytes + '\0'`，`RowWinnerStore::pkHash()` 仍是 `key=value\n`。这些编码没有长度前缀、类型标签和列序声明，和本轮已修复的 `TableStateStore::rowHash()` 不是同一套规范编码。
- 影响：构造性碰撞会让两个不同 PK 行共用同一个 winner，冲突裁决可能读到错误 incumbent，导致低 rank/高 rank 裁决污染其他行。
- 修复建议：复用 `TableStateStore::rowHash()` 风格的类型标签 + 长度前缀编码，单独实现 `canonicalPkHash(table, pkColumns, values)`，changeset、baseline、RowWinnerStore 全部调用同一函数。

### M-02：`push_chunk_progress` 幂等键未绑定 `origin/stream_epoch`

- 文件位置：`src/sync/SyncDDL.h:116`、`src/sync/SyncWorker.cpp:717`、`src/sync/apply/CapturedWriteTemplate.cpp:190`
- 违反规范：`specs/SQLite-同步工具-设计文档.md` §6，G-05；`specs/SQLite-同步工具-plan.md` T2.9 / R-07。
- 问题描述：规范要求 selection push 分片幂等键为 `(origin, stream_epoch, push_id, chunk_seq)`，或保证 `push_id` 全局唯一且绑定 origin/epoch。当前 DDL 只有 `(push_id, chunk_seq)`，`__sync_push_progress` 也只有 `push_id` 主键，未持久绑定并校验 origin/epoch。
- 影响：正常 UUID 碰撞概率很低，但协议层没有防止跨 origin/epoch 重用同一 `push_id` 的保护；一旦发生，会把不同推送的分片进度和 checksum 混在一起。
- 修复建议：将 `origin`、`stream_epoch` 加入 `push_progress`/`push_chunk_progress` 主键或唯一约束；处理分片时校验 header origin/epoch 与已存在 push 元数据一致。

## Low 问题

### L-01：`OutboxWriter` 仍是 POSIX-only，Windows 构建不可用

- 文件位置：`src/sync/transport/OutboxWriter.cpp:9`、`src/sync/transport/OutboxWriter.cpp:59`、`src/sync/transport/OutboxWriter.cpp:114`
- 违反规范：`specs/Qt-SQLite-Excel-批量导入导出-设计文档.md` 跨平台动态库目标；`specs/SQLite-同步工具-设计文档.md` 同步工具跨平台语境。
- 问题描述：文件直接包含 `<unistd.h>`，调用 `::fsync()`、`::open()`、`::close()`，没有 `Q_OS_WIN` 分支。
- 影响：Windows/MSVC 下无法编译或需要外部兼容层。
- 修复建议：使用平台封装：POSIX 保留 `fsync/open/close`；Windows 用 `_get_osfhandle` + `FlushFileBuffers`，目录 flush 明确实现或文档化为 no-op。

### L-02：现有测试入口不可用，回归验证没有闭环

- 文件位置：`build/tests/tst_auto_profile_builder`；`tests/CMakeLists.txt`
- 问题描述：`ctest --test-dir build --output-on-failure` 返回 “No tests were found”；直接运行 `build/tests/tst_auto_profile_builder` 失败：`undefined symbol: _Z13qgpu_featuresRK7QString, version Qt_5`。
- 影响：本轮无法用现有构建产物验证回归；特别是 H-03 的测试当前还编码了与规范相反的期望，容易让错误行为持续被“绿灯”保护。
- 修复建议：修复 CTest 注册和 Qt 运行库链接/路径；同步更新反向 lookup 测试，使 `exportOnMissing:"error"` 覆盖 route-local 行为。

## 总结

- 已闭合的上一轮重点：实际 SQLite 句柄上的 session 自检已加入初始化；`table_state` 行哈希改为长度前缀/类型标签；模加改为无符号；同步导入后改为 captured changeset 增量维护；auto profile 对无唯一键表已输出 `executable=false + issues`。
- 未闭合或新引入的重点：`SyncContext` 身份模型仍未按规范完成，且当前 UUID 逻辑会阻断重启；反向 lookup 的 route-local 失败语义仍错误；selection push 完成状态可被重复分片回退；row winner 的 PK 哈希还需规范编码。
- 建议修复顺序：先修 H-01/H-02，保证同步初始化和同库单写不变量；再修 H-04，避免推送完成后广播停滞；随后修 H-03 并改测试期望；最后统一 row-winner/push-progress 编码与键设计，并恢复测试入口。
