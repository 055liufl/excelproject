# 代码审查报告（第三十六轮）

## 总览

审查范围：`src/` 全部源码，对照 9 份设计/规格文档，重点复核上一轮 H-01 与 M-01 修复闭合情况。

验证结果：

- H-01 复核结论：已闭合。`syncSelected` 在读连接上调用 `rconn.transaction()`，并把 `resolvePk()` 与 `FkClosureBuilder::build()` 包在同一个 WAL 读事务内，随后 `commit()` 并释放读快照。
- M-01 结构性复核结论：部分闭合。`ConsistencyCache` 已作为 `SyncWorker` 成员，并在 `run()` 中 `init()`；但功能闭环仍缺失，详见 Medium。
- 按要求使用 `LD_LIBRARY_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/lib` 运行 `build/tests` 下 17 个 `tst_*` 测试二进制，全部通过。
- 未将 `exportOnMissing:"error"` 导出整行跳过标记为问题；该行为符合 `export-reverse-lookup` 规范。
- 未将 `SchemaEligibility` 拒绝复合 PK 标记为问题；按审查规则视为已知 MVP 限制。

问题统计：

- Critical：0
- High：1
- Medium：1
- Low：0

## Critical

无。

## High

### H-02：`syncSelected` 未物化 `FrozenManifest`，长推送无法满足“冻结清单 + 分片可续”契约

规范依据：

- `specs/SQLite-同步工具-设计文档.md:647` 要求 `FrozenManifest` 持久化，用于释放读快照、护 WAL，并支撑 `(pushId, chunkSeq)` 幂等续传。
- `specs/SQLite-同步工具-设计文档.md:649` 明确要求 `resolvePk + build` 后先物化 `FrozenManifest`，再 `COMMIT` 释放快照。
- `specs/SQLite-同步工具-设计文档.md:1107-1108` 的上行选择性推送时序为 `FCB -> FrozenManifest -> ChunkStreamer`。
- `specs/SQLite-同步工具-plan.md:134-136` 将 `FrozenManifest + ReadSnapshot`、`PushProgressStore`、`ChunkStreamer` 列为续传链路的连续任务。

实现证据：

- `src/sync/selection/FrozenManifest.h:10-21` 与 `src/sync/selection/FrozenManifest.cpp:15-34` 已实现持久化清单接口。
- 但全局搜索 `FrozenManifest|__sync_frozen_manifest` 显示，除 DDL 和类自身外，没有任何生产路径调用 `FrozenManifest::save()`。
- `src/sync/SyncWorker.cpp:1858-1917` 在读事务内解析选择集和 FK 闭包后直接 `commit()` / `cleanupRconn()`。
- `src/sync/SyncWorker.cpp:1919-1924` 随后直接把内存 `manifest` 交给 `ChunkStreamer::stream()`；`src/sync/SyncWorker.cpp:1943-1959` 只写 `__sync_push_progress`，没有写 `__sync_frozen_manifest`。

影响：

- 当前进程内、一次性写出所有 outbox 分片时可能看似可用，但一旦进程在部分分片写出、ACK 超时、传输中断或需要重发时，本地没有规范要求的冻结清单可作为续传来源。
- 若重新执行 `syncSelected` 来补发，会重新读取当前数据库，而不是复用原冻结快照；这会破坏“同一 pushId 下所有分片来自同一已冻结切面”的语义。
- 这不是单纯性能问题，会影响长推送中断后的正确性与可恢复性。

最小修复建议：

- 在 `enqueueSelectionPush()` 中生成 `pushId` 后、释放读事务前，将拓扑排序后的 `FrozenEntry` 按计划分片写入 `FrozenManifest::save()`。
- `ChunkStreamer` 应从已物化清单生成或至少与保存的清单使用同一份 frozen entries；失败路径回滚/清理 `push_progress` 与 `frozen_manifest`。
- 增加回归测试：构造多分片选择推送，模拟只写出部分分片后恢复，断言续传使用原 `pushId/chunkSeq` 与原冻结行，而不是重新读库。

## Medium

### M-01：`ConsistencyCache` 已成员化并初始化，但没有任何权威盖章路径，剪枝仍不会命中

规范依据：

- `specs/SQLite-同步工具-设计文档.md:647` 要求 `ConsistencyCache` 仅由下行/基线喂养。
- `specs/SQLite-同步工具-设计文档.md:729` 定义 `stampFromAuthoritative()` 为权威盖章接口。
- `specs/SQLite-同步工具-设计文档.md:741` 要求 `BaselineManager` 应用基线后喂养 `ConsistencyCache`。

实现证据：

- 已闭合的部分：`src/sync/SyncWorker.h:189-191` 将 `ConsistencyCache consistencyCache_` 作为成员；`src/sync/SyncWorker.cpp:334-338` 在 `run()` 中调用 `consistencyCache_.init(...)`；`src/sync/SyncWorker.cpp:1901-1903` 在选择推送中使用该成员。
- 未闭合的部分：全局搜索 `stampFromAuthoritative` 只有声明和定义，没有任何调用点。
- `src/sync/SyncWorker.cpp:1031-1034` 注释称 baseline apply 会喂养缓存，但实际进入 `BaselineManager::applyBaseline()` 后，`src/sync/baseline/BaselineManager.cpp:520-523` 只对每个表调用 `cache.invalidateTable(...)`，没有按基线权威行写入指纹。
- 即使未来调用 `stampFromAuthoritative()`，`src/sync/selection/ConsistencyCache.cpp:73-75` 的持久化 SQL 未写入 `updated_ms`，而 DDL 中 `src/sync/SyncDDL.h:81-85` 将 `updated_ms` 定义为 `NOT NULL`，默认 durable 持久化会失败且返回值被 `stampFromAuthoritative()` 忽略。

影响：

- `pruneConsistentDependencies(true)` 仍然无法利用基线/权威下行缓存剪掉已一致依赖。
- 合法的小选择集可能因为未剪枝依赖过多而错误触发 `E_SYNC_SELECTION_TOO_LARGE`。
- 默认 durable 模式下，即使后续补上内存盖章，跨重启缓存仍会因 `updated_ms` 持久化失败而丢失。

最小修复建议：

- baseline apply 完成权威切面导入后，逐行计算与 `FkClosureBuilder` 一致的依赖指纹，并调用 `consistencyCache_.stampFromAuthoritative(...)`。
- 权威下行 apply 成功后也应对确认来自中心权威态的行盖章；本地普通写入或非权威入站只应 invalidate，不应 stamp。
- 修正 `persistStamp()`：写入 `updated_ms`，检查并上抛失败；补一条 durable 缓存跨重启命中的测试。

## Low

无。

## 总结

上一轮重点 H-01 已按要求用 `rconn.transaction()` 固定 WAL 快照。M-01 的“成员 + run init”也已实现，但还没有权威盖章调用，因此依赖剪枝能力仍未真正落地。

本轮新增的主要正确性缺口是 `FrozenManifest` 未物化：当前选择推送只依赖内存 manifest 和 outbox payload，缺少规范要求的冻结清单续传来源。现有 17 个测试全部通过，但未覆盖上述同步长推送恢复与缓存剪枝场景。
