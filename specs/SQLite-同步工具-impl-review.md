# SQLite 同步工具实现评审报告（第二十二轮）

评审日期：2026-06-26

## 总体评分（表格）

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 功能合规性 | 88 | 上一轮三项指定修复已落地；Excel OpenSpec 主路径未发现新的 C/H/M。同步侧仍有 changeset 过滤后处理、导入 table_state、复合主键能力差距。 |
| 同步协议正确性 | 84 | RowWinner 决定性 tie-breaker 和 `putOrRefill()` 失败传播已修复；但 `apply_v2` 过滤只覆盖业务 apply，不覆盖 RowWinner/table_state 后处理，可能污染或绕过同步表边界。 |
| Excel 导入/导出合规性 | 92 | `columnOrder`、reverse lookup、fkInject、row lookup、time-format 的 loader/validator/export/import 主干与规格总体一致；运行时测试仍被 Qt 符号冲突阻断。 |
| 安全性 | 88 | SQL 标识符引用和绑定整体可接受；本轮发现 changeset 后处理未复用 `__sync_*` / allow-list 过滤，属于同步边界绕过风险。 |
| 边界处理 | 83 | table_state 在导入、本地写、selection push 和失败回滚语义上仍不够硬；复合主键/WITHOUT ROWID 能力仍窄于设计。 |
| 综合评分 | 85 | 较第二十一轮指定修复有进展，但新增 1 个 High、4 个 Medium，本轮未通过。 |

本轮审查范围：指定 10 组规格文档、`openspec/changes/archive/` 下 27 个归档文档，以及 `src/` 下 122 个源文件。自动验证：`cmake --build build -- -j2` 通过；`ctest --test-dir build --output-on-failure` 未注册测试；直接运行 `build/tests/tst_profile_loader` 仍因 Qt 符号 `_Z13qgpu_featuresRK7QString, version Qt_5` lookup error 失败，未进入业务断言。

## 上一轮修复验证（表格）

| 编号 | 状态 | 验证结论 |
| --- | --- | --- |
| H-01 | 通过 | `SyncConfig::Builder::build()` 已拒绝显式配置的重复 rank；`ConflictArbiter::beats()`、`RowWinnerStore::beats()`、`ChangesetApplier::conflictCb()` / DELETE dominated 判断均加入 `originId` 字典序 tie-breaker，同 rank/同 seq 不再按到达顺序分歧。 |
| M-01 | 通过 | `ChangesetApplier::updateWinnersFromChangeset()` 已对 `winners.putOrRefill()` 传入 `putErr` 并检查返回值，失败时返回 false，由外层回滚 apply。 |
| M-03 | 部分通过 | changeset 分支 `CapturedWriteTemplate::extractMutations()` 已改为列名排序的 `TableStateStore::rowHash()` 口径；但 Branch B/C 的 RowMutation table_state 路径仍使用裸值序列，见本轮 M-02。 |

## Critical 问题（如有）

本轮未发现 Critical 级问题。

## High 问题（如有）

### H-01：`apply_v2` 的 table allow-list 没有同步作用到 RowWinner/table_state 后处理，过滤表仍可污染元数据甚至触发恢复写入

- 位置：`src/sync/apply/ChangesetApplier.cpp:99`、`src/sync/apply/ChangesetApplier.cpp:111`、`src/sync/apply/ChangesetApplier.cpp:144`、`src/sync/apply/ChangesetApplier.cpp:234`、`src/sync/apply/ChangesetApplier.cpp:267`、`src/sync/apply/ChangesetApplier.cpp:368`、`src/sync/apply/ChangesetApplier.cpp:463`、`src/sync/apply/CapturedWriteTemplate.cpp:128`、`src/sync/apply/CapturedWriteTemplate.cpp:444`
- 描述：`filterCb()` 正确拒绝 `__sync_*` 和不在 `syncTables` 中的表，但该过滤只传给 `sqlite3changeset_apply_v2()`。apply 完成后，`updateWinnersFromChangeset()` 重新遍历完整 changeset，未接收/检查同一 allow-list；`CapturedWriteTemplate::extractMutations()` 也对完整 changeset 生成 table_state delta。
- 影响：被 `apply_v2` 跳过的表仍会写入 `__sync_row_winner` 和 `__sync_table_state`。更严重的是，若先前已被污染出 RowWinner，后续一个被过滤表的低 rank DELETE 会进入恢复分支，并执行 `INSERT ... ON CONFLICT DO UPDATE` 写回该表，绕过同步表 allow-list；`__sync_*` 内部表同样缺少后处理层防线。
- 修复建议：抽出统一 `isAllowedSyncTable(table, syncTables)`，同时排除 `__sync_*`；`updateWinnersFromChangeset()`、`extractMutations()` 与 `filterCb()` 使用同一判定。补充测试：payload 同时包含允许表、非允许表、`__sync_*` 表，断言被过滤表没有业务写入、RowWinner、table_state 变化；再构造“先污染 RowWinner 后 DELETE”的回归，断言不会恢复写入过滤表。

## Medium 问题（如有）

### M-01：同步激活后的 `IBatchTransfer` 导入绕过 `CapturedWriteTemplate::branchC()`，成功导入不维护 `table_state`

- 位置：`src/batch/BatchTransfer.cpp:116`、`src/sync/SyncEngine.cpp:114`、`src/sync/SyncWorker.cpp:1383`、`src/sync/SyncWorker.cpp:1412`、`src/sync/SyncWorker.cpp:1443`、`src/sync/SyncWorker.cpp:1452`、`src/sync/SyncWorker.cpp:1464`
- 描述：同步激活后，批量导入通过 `ctx->importFn` 改道到 `SyncWorker::submitImportSync()`。该函数注释称“CapturedWriteTemplate branch C”，但实际手写 `WriteTxn + rec_->begin + ImportService::run + rec_->sealInto + commit`，没有构造 `WriteParams{LocalWrite}`，也没有调用 `TableStateStore::applyMutations()`。
- 影响：导入业务数据和 changelog 会提交，peer 端通过 changeset apply 可能维护了 table_state，但导入源端的 `__sync_table_state` 不更新。场景2 `DiffEngine` 用 `schema_fingerprint + row_count + content_checksum` 判等，源端会出现 missing/陈旧状态，导致表级零全量判等假红或 OnlyRemote/OnlyLocal。
- 修复建议：导入路径应真正复用 `CapturedWriteTemplate`，或让 `ImportService` 输出可复用的 `RowMutation` / `TableMutation` 列表并在同一事务内更新 table_state。补充测试：同步激活后通过 `IBatchTransfer` 导入一行，断言业务表、changelog、`__sync_table_state.row_count/content_checksum` 同事务更新。

### M-02：Branch B/C 的 `table_state` 哈希仍使用裸值序列，未复用上一轮修复后的列名排序 rowHash

- 位置：`src/sync/apply/CapturedWriteTemplate.cpp:255`、`src/sync/apply/CapturedWriteTemplate.cpp:267`、`src/sync/apply/CapturedWriteTemplate.cpp:284`、`src/sync/apply/CapturedWriteTemplate.cpp:404`、`src/sync/apply/CapturedWriteTemplate.cpp:420`
- 描述：上一轮修复只覆盖 changeset 的 `extractMutations()`。Branch B/C 对 `RowMutation` 的 afterHash 使用 `m.values` 裸序列，对 beforeHash 使用 `SELECT *` 裸序列，均不是 `TableStateStore::rowHash(QVariantMap)` 的 `key=value\n` 列名排序口径。
- 影响：selection push、ComparisonSession save 等本地 UPSERT 路径的 checksum 与 baseline/changeset 路径不兼容；UPDATE/DELETE 后无法可靠抵消旧行 hash，DiffEngine 表级判等仍可能漂移。
- 修复建议：为 `TableStateStore` 暴露统一 row fingerprint helper，Branch B/C 的 before/after 都按完整行 `QVariantMap` 计算；UPSERT 后按 PK 重新读取完整行生成 afterHash，避免只 hash mutation 中出现的列。

### M-03：`table_state` 写失败被降级为 warning 后继续提交，破坏 apply 三件套同事务约束

- 位置：`src/sync/apply/CapturedWriteTemplate.cpp:128`、`src/sync/apply/CapturedWriteTemplate.cpp:134`、`src/sync/apply/CapturedWriteTemplate.cpp:420`、`src/sync/apply/CapturedWriteTemplate.cpp:423`、`src/sync/diff/DiffEngine.cpp:43`
- 描述：设计文档 §5.4 明确“任一步失败 rollback，向量/胜者/表态/changelog 一并回退”，计划 §1 也要求维护 table_state + 同事务。当前 Branch A/B/C 在 `ts_.applyMutations()` 失败时只设置 `tableStateStaleSince`，随后仍可推进 `applied_vector`、写 changelog 并提交业务数据。
- 影响：业务数据和同步向量已经前进，但表级判等依据缺失或陈旧；后续场景2会在看似成功的同步流后给出错误判等。该问题不直接破坏业务行，但破坏“DiffEngine 可依赖 table_state”的实现前提。
- 修复建议：同步 apply 的默认策略应把 table_state 写失败视为事务失败并回滚。若确实需要降级模式，应持久化显式 stale 标记，并让 `DiffEngine` 对 stale 表拒绝零全量判等或触发 baseline 重建，而不是继续使用旧 checksum。

### M-04：同步 eligibility / selection / diff 仍只支持单列主键，窄于设计允许的复合主键与 WITHOUT ROWID 能力

- 位置：`src/sync/schema/SchemaEligibility.cpp:80`、`src/sync/selection/SelectionResolver.cpp:14`、`src/sync/diff/DiffEngine.cpp:177`、`src/sync/diff/ComparisonSession.cpp:413`
- 描述：实现已把复合主键显式标成 MVP 限制并报 `E_SYNC_COMPOSITE_PK_NOT_SUPPORTED`；但同步设计文档 §4.4 仍要求普通 rowid 表或 `WITHOUT ROWID` 表均可，只要有显式非空 PK。selection、diff、ComparisonSession 也只取第一个 PK 列。
- 影响：合法 SQLite 业务表（复合 PK、WITHOUT ROWID 复合键）无法进入同步；若绕过 eligibility，行身份会丢失主键的一部分。
- 修复建议：短期在设计/计划能力矩阵中明确“同步 MVP 仅支持单列 INTEGER/TEXT PK”；若按现设计实现，应引入统一复合 PK tuple 编码，并覆盖 eligibility、selection record、diff row key、RowWinner pkHash、FK closure 与 upsert target。

## 修复优先级汇总表（如无 C/H/M 则说明本轮通过）

| 编号 | 文件 | 严重度 | 一句话描述 |
| --- | --- | --- | --- |
| H-01 | `src/sync/apply/ChangesetApplier.cpp:234` | High | changeset 后处理未复用 apply filter，被过滤表仍可写 RowWinner/table_state，DELETE 恢复分支还可能绕过 allow-list 写业务表。 |
| M-01 | `src/sync/SyncWorker.cpp:1443` | Medium | 同步激活后的批量导入只捕获 changelog，不维护 `__sync_table_state`。 |
| M-02 | `src/sync/apply/CapturedWriteTemplate.cpp:255` | Medium | Branch B/C table_state 哈希仍是裸值序列，不是统一列名排序 rowHash。 |
| M-03 | `src/sync/apply/CapturedWriteTemplate.cpp:134` | Medium | table_state 写失败被降级 warning 后提交，违背 apply 三件套同事务。 |
| M-04 | `src/sync/schema/SchemaEligibility.cpp:80` | Medium | 复合主键/WITHOUT ROWID 仍未实现，窄于当前同步设计文档。 |

本轮未通过：无 Critical，但有 1 个 High、4 个 Medium。建议先修 H-01，再收口所有 table_state 写路径；否则 RowWinner 边界和场景2表级判等都不能作为可靠基础。
