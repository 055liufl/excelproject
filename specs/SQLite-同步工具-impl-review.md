# 代码审查报告（第三十七轮）

## 总览

审查范围：`src/` 全部源代码，对照以下规范与设计文档进行正确性审查：两份 Qt/SQLite/Excel 导入导出设计、两份 SQLite 同步工具设计/计划，以及 `export-column-order`、`export-reverse-lookup`、`fk-injection`、`row-lookup`、`time-format` 五份 OpenSpec。

本轮重点结论：

- H-02：`FrozenManifest::save()` 已在 `SyncWorker::enqueueSelectionPush()` 中调用，但当前调用顺序使它在启用外键时会失败，且失败被吞掉。因此 H-02 仍未真正闭合。
- M-01：`stampFromAuthoritative()` 已在 baseline apply 成功后调用；`persistStamp()` 已写入 `updated_ms`。这两个指定检查点已闭合。但 baseline 盖章指纹算法与 `FkClosureBuilder` 剪枝指纹算法不一致，导致缓存命中仍可能失效，形成新的 Medium 正确性缺陷。
- 按要求用 `LD_LIBRARY_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/lib` 直接运行 `build/tests` 下 17 个 `tst_*` 测试二进制，全部通过。`ctest --test-dir build` 和 `ctest --test-dir build/tests` 当前没有发现 CTest 注册项，但未作为缺陷标记。
- 未将 `exportOnMissing:"error"` 的导出整行跳过标记为问题；该行为符合 `export-reverse-lookup` 规范。
- 未将 `SchemaEligibility` 拒绝复合 PK 标记为问题；按审查规则视为已知 MVP 限制。

问题统计：

- Critical：0
- High：1
- Medium：1
- Low：0

## Critical

无。

## High

### H-02：`FrozenManifest::save()` 虽被调用，但因先写子表后写父表而在启用 FK 时失效

规范依据：

- `specs/SQLite-同步工具-设计文档.md` 要求 `FrozenManifest` 持久化，用于释放读快照、保护 WAL，并作为分片续传的冻结来源。
- 同文档的 `ReadSnapshot` 契约要求在短读快照中完成选择解析与闭包物化，之后释放读事务；同一 `pushId/chunkSeq` 的续传不得重新读取已变化的业务库。
- `specs/SQLite-同步工具-plan.md` T2.8/T2.10 要求 `FrozenManifest + ChunkStreamer` 支撑 `(push_id, chunk_seq)` 幂等续传。

实现证据：

- `src/sync/SyncWorker.cpp:1989-1999` 在 `enqueueSelectionPush()` 中调用了 `FrozenManifest::save()`，位置也在 outbox 写入之前。
- 但 `src/sync/SyncDDL.h:128-139` 定义 `__sync_frozen_manifest` 通过 `FOREIGN KEY(push_id)` 引用 `__sync_push_progress(push_id)`。
- `src/sync/SyncWorker.cpp:179-183` 对写连接启用了 `PRAGMA foreign_keys=ON`。
- 父表 `__sync_push_progress` 的插入发生在 `src/sync/SyncWorker.cpp:2002-2018`，晚于 `FrozenManifest::save()`。
- `src/sync/SyncWorker.cpp:1995-1998` 忽略了 `fm.save()` 的返回值和错误；`src/sync/selection/FrozenManifest.cpp:33-37` 在 FK 失败时会返回 `false`。

影响：

启用外键的正常同步连接上，第一条 frozen manifest 插入会因父 `push_id` 尚不存在而触发外键失败。由于错误被吞掉，`syncSelected` 仍会写出 outbox 分片，看起来前台流程可继续，但本地没有规范要求的冻结清单。进程崩溃、传输中断、ACK 超时或需要续传时，只能依赖 payload 或重新读库，无法保证同一 push 的分片来自原冻结切面，破坏长推送续传语义。

最小修复建议：

先创建 `__sync_push_progress` 的 `streaming` 父行，再保存所有 `__sync_frozen_manifest` 行，然后写 outbox；或者把两者放进同一事务且保证父行先插入。`fm.save()` 失败必须取消本次 foreground ACK 等待、上报 `E_SYNC_TRANSPORT` 或更精确的 syncSelected 后台失败，并禁止继续发布 outbox 分片。补一条测试：启用 `PRAGMA foreign_keys=ON` 后发起多分片 `syncSelected`，断言 `__sync_frozen_manifest` 有对应 `push_id/chunk_seq` 行。

## Medium

### M-01：baseline 后已调用 `stampFromAuthoritative()`，但盖章指纹与剪枝指纹不一致

规范依据：

- `specs/SQLite-同步工具-设计文档.md` 要求 `ConsistencyCache` 仅由下行/基线喂养，用于 `pruneConsistentDependencies()` 剪掉已与中心一致的依赖。
- 同文档定义 `stampFromAuthoritative()` 为权威盖章接口，baseline apply 后应喂养 `ConsistencyCache`。

已闭合部分：

- `src/sync/SyncWorker.cpp:1035-1037` 在 baseline response 中成功调用 `BaselineManager::applyBaseline()`。
- `src/sync/SyncWorker.cpp:1043-1083` 在 baseline apply 成功后遍历同步表并调用 `consistencyCache_.stampFromAuthoritative()`。
- `src/sync/selection/ConsistencyCache.cpp:76-82` 的 `persistStamp()` 已包含 `updated_ms`，满足 `src/sync/SyncDDL.h:81-86` 的 `NOT NULL` 约束。

缺陷证据：

- `src/sync/selection/FkClosureBuilder.cpp:20-28` 的剪枝指纹通过 `QVariantMap` 迭代计算。`QVariantMap` 是按 key 排序的 map，因此指纹材料顺序是列名排序。
- `src/sync/SyncWorker.cpp:1071-1081` 的 baseline 盖章指纹按 `QSqlRecord` 字段顺序计算，即表定义/查询结果顺序。
- 当表列顺序不是字典序时，同一行在 baseline 盖章侧与 FK 闭包剪枝侧会得到不同 SHA1，`ConsistencyCache::isConsistent()` 无法命中。

影响：

baseline 后缓存表面上已被喂养，持久化也能写入，但 `pruneConsistentDependencies(true)` 对很多真实表仍会误判“不一致”。结果是已与中心一致的父依赖不会被剪掉，合法选择推送可能被不必要的 FK 闭包放大，甚至错误触发 `E_SYNC_SELECTION_TOO_LARGE`。这影响上行选择性推送的正确可用性，不只是性能问题。

最小修复建议：

抽出一个共享的行指纹函数，供 `FkClosureBuilder` 和 baseline 盖章共同使用；至少让 `SyncWorker.cpp:1075-1080` 改为按与 `QVariantMap` 一致的 key 顺序构造材料。补一条回归测试：创建列顺序为 `b, a` 的表，baseline 后对同一行执行 FK 依赖剪枝，断言 `isConsistent()` 命中。

## Low

无。

## 总结

H-02 不能仅按“有调用”判定闭合：当前 `FrozenManifest::save()` 会在 FK 开启时因父行尚不存在而失败，且错误被忽略，长推送续传仍没有可靠冻结清单。

M-01 的两个指定检查点已经补上：baseline apply 后确实调用了 `stampFromAuthoritative()`，`persistStamp()` 也包含 `updated_ms`。但盖章侧和剪枝侧的指纹材料顺序不一致，导致缓存喂养后仍可能无法命中，应作为本轮剩余 Medium 修复。
