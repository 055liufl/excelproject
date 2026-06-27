# 代码审查报告（第三十五轮）

## 总览

评分：88/100。

问题统计：

- Critical：0
- High：1
- Medium：1
- Low：0

审查范围：`src/` 全部源码，对照批量导入导出设计文档、SQLite 同步工具设计/plan，以及 `export-column-order`、`export-reverse-lookup`、`fk-injection`、`row-lookup`、`time-format` OpenSpec。

验证结果：

- 按要求使用 `LD_LIBRARY_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/lib` 运行 `build/tests` 下 17 个 `tst_*` 测试二进制，全部通过。
- 未将 `exportOnMissing:"error"` 导出整行跳过标记为问题；该行为符合 `export-reverse-lookup` 规范。
- 未将 `SchemaEligibility` 拒绝复合 PK 标记为问题；按审查规则视为已知 MVP 限制。

## Critical

无。

## High

### H-01：`syncSelected` 构建 FrozenManifest 时没有显式读事务，无法满足 ReadSnapshot 一致性契约

规范依据：

- `specs/SQLite-同步工具-设计文档.md:649` 明确要求闭包解析在只读连接上按 `BEGIN -> resolvePk + build -> COMMIT` 执行，以 WAL 固定一致性快照。
- 同一段还要求 `resolvePk`、取行值、判悬挂父、算指纹、剪枝都基于同一个已发布的一致视图。

实现证据：

- `src/sync/SyncWorker.cpp:1830-1845` 为 `syncSelected` 打开短生命周期读连接，但没有 `transaction()` / `BEGIN`，也没有只读打开选项。
- 随后 `src/sync/SyncWorker.cpp:1851` 调用 `SelectionResolver::resolvePk()`，`src/sync/SyncWorker.cpp:1875-1876` 调用 `FkClosureBuilder::build()`，最后 `src/sync/SyncWorker.cpp:1888` 直接关闭连接；整个过程没有固定快照。
- `src/sync/selection/SelectionResolver.cpp:48-77` 与 `src/sync/selection/FkClosureBuilder.cpp:53-82` 都是普通 autocommit `SELECT`。在 SQLite WAL 下，autocommit 只保证单条语句自身的快照，不保证跨多条查询的一致视图。

影响：

- 当其他连接在 `resolvePk` 与 FK 闭包查询之间提交变更时，FrozenManifest 可能混合两个时间点的数据。
- 具体风险包括：已选行使用旧 FK 值，依赖行查询看到新状态而误报 `E_SYNC_FK_CLOSURE_MISSING`；或 selected/dependency 行来自不同快照，导致 selection push 发送一个数据库从未真实存在过的一致切面。
- 这违反同步设计里 selection push 的冻结清单语义，会影响上行选择性推送正确性。

最小修复建议：

- 在 `enqueueSelectionPush()` 打开 `rconn` 后立即开启显式读事务，例如 `BEGIN` / `rconn.transaction()`；确保 `resolvePk()` 与 `FkClosureBuilder::build()` 全部在同一事务内执行。
- 在所有成功和失败路径上 `COMMIT` 或 `ROLLBACK` 后再 `removeDatabase()`。
- 如需严格满足“只读连接”，为该连接使用 Qt/SQLite 支持的只读打开方式或 URI `mode=ro`。

## Medium

### M-01：`ConsistencyCache` 从未初始化或持久化，`pruneConsistentDependencies` 实际不会命中

规范依据：

- `specs/SQLite-同步工具-设计文档.md:647` 要求 `ConsistencyCache` 用于选择性推送依赖剪枝，并且仅由下行/基线喂养。
- `specs/SQLite-同步工具-设计文档.md:649` 要求剪枝判定读取已发布的一致视图。
- `specs/SQLite-同步工具-设计文档.md:741` 明确要求 `BaselineManager` 应用基线后喂养 `ConsistencyCache`。
- `include/dbridge/sync/SyncConfig.h:105-106` 暴露 `consistencyCacheDurable()`，且 `include/dbridge/sync/SyncConfig.h:151` 默认值为 `true`。

实现证据：

- `src/sync/SyncWorker.cpp:1870-1876` 在每次 `syncSelected` 中构造一个新的局部 `ConsistencyCache cache`，没有调用 `cache.init(...)`，也没有从 `__sync_consistency_cache` 加载已持久化内容。
- `src/sync/selection/ConsistencyCache.cpp:17-31` 只有调用 `init(db, durable)` 才会启用 durable 模式并加载表内容；当前 selection push 路径没有调用。
- `src/sync/SyncWorker.cpp:1016-1024` 应用 baseline 时同样构造局部 `ConsistencyCache cache` 传给 `BaselineManager::applyBaseline()`，但也没有 `init(..., config_.consistencyCacheDurable())`。
- `src/sync/selection/ConsistencyCache.cpp:63-67` 只有 `durable_ == true` 时 `stampFromAuthoritative()` 才持久化；未初始化的临时对象 `durable_` 为默认 false，即使 baseline 内部喂养也只会写入临时内存，函数返回后丢失。

影响：

- `SyncSelection::Builder::pruneConsistentDependencies(true)` 在实际运行中无法利用基线/权威下行缓存剪掉已一致依赖，行为退化为全部依赖都进入 manifest。
- 这不仅是性能问题：当依赖图很大时，原本应被剪枝后小于 `maxSelectionSize` 的合法选择，可能因为缓存永远空而错误触发 `E_SYNC_SELECTION_TOO_LARGE`。

最小修复建议：

- 将 `ConsistencyCache` 作为 `SyncWorker` 成员或通过 `SyncContext` 发布，启动时调用 `init(wconn, config_.consistencyCacheDurable(), ...)`。
- baseline apply 和权威下行 apply 后用同一个 cache 实例调用 `stampFromAuthoritative()`；selection push 读取该已初始化实例，必要时用 mutex 或写线程发布快照。
- 增加回归测试：先应用 baseline/权威下行喂养缓存，再执行 `syncSelected(... pruneConsistentDependencies=true)`，确认已一致 dependency 不进入 FrozenManifest，且不会误触 `maxSelectionSize`。

## Low

无。

## 总结

本轮未发现批量导入导出 OpenSpec 能力上的新真实缺陷；上一轮 lookup affinity 问题已在当前源码中修复。

当前需要优先处理的是同步选择性推送路径：先补上显式 ReadSnapshot 事务，避免 FrozenManifest 混合多时间点数据；随后补齐 `ConsistencyCache` 的初始化、持久化和发布路径，让依赖剪枝能力按设计生效。
