# SQLite 同步工具实现评审报告（第二十七轮）

评审日期：2026-06-26

## 总体评分（表格）

| 维度 | 评分 | 结论 |
|---|---:|---|
| 上一轮修复完成度 | 100/100 | 三项指定修复均已落到实际调用链，未发现伪接入或遗漏传递。 |
| 入站 changeset 参数传递 | 98/100 | `processChangesetArtifact` 已设置 `authoritative`、`syncTables`、`conflictPolicy`，并传入 `CapturedWriteTemplate::execute`。 |
| SQLite changeset 冲突处理 | 96/100 | `ConflictPolicy` 已进入 `ConflictCtx`，`TargetWins`/`Manual` 在非 authoritative 冲突下直接 `OMIT`。 |
| 表过滤一致性 | 96/100 | `canonicalSyncTables_` 已传入 Branch A，随后进入 `ChangesetApplier::filterCb`，能限制入站 changeset 表范围。 |
| 本轮综合评分 | 97/100 | 第二十六轮指出的问题已修复；本轮未发现 Critical/High/Medium 新问题。 |

## 上一轮修复验证（表格）

| 上一轮修复项 | 验证位置 | 验证结果 | 评审结论 |
|---|---|---|---|
| `WriteParams.authoritative` 传递到 `ApplyOptions` | `src/sync/apply/CapturedWriteTemplate.cpp:106-113` | `branchA` 创建 `ApplyOptions opts` 后执行 `opts.authoritative = p.authoritative`，并将 `opts` 传入 `applier_.apply(...)`。 | 通过 |
| Edge 节点收到 center changeset 时设为 authoritative | `src/sync/SyncWorker.cpp:608-612` | `processChangesetArtifact` 使用 `senderPeer = hdr.senderPeer.isEmpty() ? hdr.origin : hdr.senderPeer`，当本节点是 Edge 且 `senderPeer == centerNodeId` 时设置 `p.authoritative = true`。 | 通过 |
| `processChangesetArtifact` 设置 `p.syncTables=canonicalSyncTables_` | `src/sync/SyncWorker.cpp:614-616` | 入站 changeset 的 `WriteParams` 已设置 `p.syncTables = canonicalSyncTables_`。 | 通过 |
| `ConflictCtx` 接受 `ConflictPolicy` | `src/sync/apply/ChangesetApplier.h:44-58`、`src/sync/apply/ChangesetApplier.cpp:88-104` | `ConflictCtx` 定义 `conflictPolicy` 字段，`apply(...)` 中从 `opts.conflictPolicy` 赋值。 | 通过 |
| `TargetWins`/`Manual` 下 `conflictCb` 直接 `OMIT` | `src/sync/apply/ChangesetApplier.cpp:174-236` | 非 authoritative 的 `SQLITE_CHANGESET_DATA`/`SQLITE_CHANGESET_CONFLICT` 分支先检查 `ConflictPolicy::TargetWins` 或 `Manual`，命中后增加 `ignored` 并返回 `SQLITE_CHANGESET_OMIT`。 | 通过 |
| `conflictPolicy` 从入站参数传入 applier | `src/sync/SyncWorker.cpp:618-620`、`src/sync/apply/CapturedWriteTemplate.cpp:106-113` | `processChangesetArtifact` 设置 `p.conflictPolicy = config_.conflictPolicy()`；`branchA` 设置 `opts.conflictPolicy = p.conflictPolicy`。 | 通过 |

## Critical 问题（如有）

无。

## High 问题（如有）

无。

## Medium 问题（如有）

无。

## 修复优先级汇总表

| 优先级 | 问题 | 建议动作 | 状态 |
|---|---|---|---|
| Critical | 无 | 无需处理 | 已清零 |
| High | 无 | 无需处理 | 已清零 |
| Medium | 无 | 无需处理 | 已清零 |
