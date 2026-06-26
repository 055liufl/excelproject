# SQLite 同步工具实现评审报告（第二十轮）

评审日期：2026-06-26

## 总体评分（表格）

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 功能合规性 | 88 | 上一轮指定的 baseline RowWinner seed 主行为已补齐；Excel 导入/导出 OpenSpec 能力本轮未发现新的 C/H/M 缺陷。 |
| 同步协议正确性 | 80 | baseline seed 的完整内容恢复已通过，但普通 changeset 冲突路径仍可能写入无 `winning_content` 的胜者，后续低 rank DELETE 会再次卡住同步流。 |
| Excel 导入/导出合规性 | 90 | `columnOrder`、reverse lookup、fkInject、row lookup、time-format 相关实现与规格保持一致；本轮未发现新的高优先级缺陷。 |
| 安全性 | 90 | SQL 标识符引用、绑定、内部表过滤、raw WHERE 拒绝策略整体可接受；未发现新的注入类 Critical。 |
| 边界处理 | 82 | baseline seed 仍对部分失败静默跳过；同步 eligibility / selection / diff 的复合主键能力仍窄于设计规格。 |
| 综合评分 | 84 | 较第十九轮略降：上一轮 H-01 主修复通过，但 RowWinner 普通冲突路径暴露同类 High 风险，且 baseline seed 失败处理仍不够事务化。 |

本轮审查范围：指定 10 组规格文档、`openspec/changes/archive/` 下全部归档变更文档，以及 `src/` 下全部源文件。自动验证：`cmake --build build -- -j2` 通过；`ctest --test-dir build --output-on-failure` 未注册测试；直接运行 `build/tests/tst_profile_loader`、`build/tests/tst_column_order_export`、`build/tests/tst_reverse_lookup_export` 均在启动时因 Qt 符号 `_Z13qgpu_featuresRK7QString` / `qgpu_features_ptr, version Qt_5` lookup error 失败，未进入业务断言。

## 上一轮修复验证（表格）

| 编号 | 状态 | 验证结论 |
| --- | --- | --- |
| H-01 | 通过 | `BaselineManager::applyBaseline()` 在 `rw.resetAll()` 后逐表扫描 baseline 行，按 `ChangesetApplier` 的 `pkHash` 材料构造方式写入 `RowWinner`，并保存 `contentHash` 与 JSON `winningContent`。低 rank DELETE 恢复路径所需的完整行内容主行为已补齐。 |
| H-01 边界 | 部分通过 | seed 过程中 `PRAGMA table_info` 失败、`SELECT *` 失败、`rw.put()` 失败仍可能被静默跳过，未把 baseline 事务回滚为 `E_SYNC_BASELINE_FAILED`；该残留归入本轮 M-01。 |

## Critical 问题（如有）

本轮未发现 Critical 级问题。

## High 问题（如有）

### H-01：普通 changeset 冲突胜者会先写入空 winningContent，后处理无法补全，后续低 rank DELETE 仍会卡死同步流

- 位置：`src/sync/apply/ChangesetApplier.cpp:192`、`src/sync/apply/ChangesetApplier.cpp:203`、`src/sync/apply/ChangesetApplier.cpp:448`、`src/sync/apply/ChangesetApplier.cpp:452`、`src/sync/apply/RowWinnerStore.cpp:48`、`src/sync/apply/RowWinnerStore.cpp:129`
- 描述：非权威 changeset 在 `SQLITE_CHANGESET_DATA/CONFLICT` 回调中，如果 challenger 胜出，会立即调用 `RowWinnerStore::put()`。此时 `RowWinner challenger` 只填了 `origin/rank/originSeq/contentHash`，没有填 `winningContent`。`apply_v2` 结束后，`updateWinnersFromChangeset()` 虽然构造了完整 JSON 行内容，但再次调用 `winners.put()` 时 incumbent 已经是同一 `(rank, originSeq)` 的空内容胜者；`RowWinnerStore::beats()` 只接受更高 rank 或同 rank 更大 seq，导致补全写入被判为 no-op。
- 影响：只要某一行是通过冲突回调胜出的，`__sync_row_winner.winning_content` 就可能永久为空。之后较低 rank 或较旧 seq 的 DELETE 到达时，`updateWinnersFromChangeset()` 判断 incumbent 支配该 DELETE，但因 `winningContent.isEmpty()` 返回 `E_SYNC_APPLY_CONSTRAINT`，事务回滚、`applied_vector` 不前进、不 ACK；同 origin 后续 changeset 会被严格连续水位阻塞。上一轮修复只覆盖 baseline seed，未覆盖普通冲突胜者路径。
- 规格依据：设计文档 §5.6/§6.1、计划 T1.7b/T5.1 要求 `__sync_row_winner` 支持跨批后到、到达序无关的逐行胜者裁决；被低 rank DELETE 支配的高 rank 行应恢复并推进水位，而不是因缺失胜者内容卡死。
- 修复建议：不要在 `conflictCb()` 写入不完整 `RowWinner`；或提供专门的 `putExact/replaceSameWinnerContent`，允许同一 `(table, pk_hash, origin, rank, originSeq)` 用完整 `winningContent/contentHash` 覆盖空内容。补充夹具：高 rank UPDATE/INSERT 通过 conflict callback 胜出后，断言 `__sync_row_winner.winning_content` 非空；再注入低 rank DELETE，断言业务行保留、artifact consumed、`applied_vector` 前进、后续 seq 可继续应用。

## Medium 问题（如有）

### M-01：baseline seed RowWinner 的失败路径仍静默跳过，可能提交缺胜者的 baseline

- 位置：`src/sync/baseline/BaselineManager.cpp:371`、`src/sync/baseline/BaselineManager.cpp:384`、`src/sync/baseline/BaselineManager.cpp:474`
- 描述：上一轮 H-01 主逻辑已经补齐完整 `winningContent/contentHash`，但 seed 阶段仍对 `PRAGMA table_info` 无结果、`SELECT *` 失败、`rw.put()` 失败不报错。baseline 已经删除并重插业务表、重置 `applied_vector/table_state/row_winner`，此时若部分 winner 未写入仍提交，会让 baseline 权威切面和胜者表不一致。
- 影响：被漏 seed 的行后续无法可靠执行低 rank DELETE 恢复；严重时会重新出现被较低 rank 变更覆盖或卡住 changeset 流的问题，但触发条件需要 seed 查询或写表失败，因此评为 Medium。
- 修复建议：seed 阶段任一表无法读取 PK、无法扫描行、或 `rw.put()` 失败，应回滚整个 baseline 并返回 `E_SYNC_BASELINE_FAILED`。同时在 `rw.put()` 调用处传入 `err` 并检查返回值。

### M-02：同步 eligibility / diff / selection 仍只支持单列主键，窄于设计允许的复合主键与 WITHOUT ROWID 能力

- 位置：`src/sync/schema/SchemaEligibility.cpp:80`、`src/sync/selection/SelectionResolver.cpp:24`、`src/sync/diff/DiffEngine.cpp:192`
- 描述：`SchemaEligibility` 仍显式拒绝 composite PRIMARY KEY；`SelectionResolver` 与 `DiffEngine` 也只取 `pk == 1` 的单列作为行身份。错误文案已说明这是 MVP 限制，但同步设计文档 §4.4 要求普通 rowid 表或 `WITHOUT ROWID` 表均可，且应记录每表 PK 列供 selection、diff、upsert、row winner 复用。
- 修复建议：短期可在规格/能力矩阵中降级声明“同步 MVP 仅支持单列 INTEGER/TEXT PK”；若按当前设计实现，应引入统一复合 PK tuple 编码，并覆盖 eligibility、selection record、diff row key、RowWinner pkHash、FK closure 与 upsert target。

## 修复优先级汇总表（如无 C/H/M 则说明本轮通过）

| 编号 | 文件 | 严重度 | 一句话描述 |
| --- | --- | --- | --- |
| H-01 | `src/sync/apply/ChangesetApplier.cpp:203` | High | 冲突回调写入空 `winningContent` 后，后处理被 `RowWinnerStore::beats()` 拦截，低 rank DELETE 仍可卡死同步流。 |
| M-01 | `src/sync/baseline/BaselineManager.cpp:474` | Medium | baseline seed 写 RowWinner 的失败未检查，可能提交缺胜者内容的 baseline。 |
| M-02 | `src/sync/schema/SchemaEligibility.cpp:80` | Medium | 实现仍拒绝复合主键，selection/diff 也只按单列 PK 建模，窄于同步设计规格。 |

本轮无 Critical；上一轮 H-01 的 baseline seed 主修复通过，但 RowWinner 普通冲突路径仍需补齐完整胜者内容写入语义。
