# SQLite 同步工具实现评审报告（第十九轮）

评审日期：2026-06-26

## 总体评分（表格）

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 功能合规性 | 88 | 上一轮指定的 baseline 控制序列与 RowWinner seed 均有实现；Excel 导入/导出 OpenSpec 相关路径本轮未发现新的 C/H/M 缺陷 |
| 同步协议正确性 | 82 | baseline request/response 不再消耗 changeset `origin_seq`，严格连续水位空洞问题已收口；但 baseline 后的低 rank DELETE 会因缺少 `winning_content` 卡住 changeset 流 |
| Excel 导入/导出合规性 | 90 | columnOrder、reverse lookup、fkInject、row lookup、time-format 相关实现与上一轮状态一致；现有测试可执行文件因 Qt 运行库符号问题未能执行 |
| 安全性 | 90 | SQL 标识符引用、绑定、内部表过滤、raw WHERE 拒绝策略整体保持可接受；未发现新的注入类 Critical |
| 边界处理 | 81 | baseline RowWinner 修复只覆盖 UPDATE/INSERT 冲突胜者，不覆盖 DELETE 恢复；复合主键 / WITHOUT ROWID 支持仍窄于设计规格 |
| 综合评分 | 85 | 较第十八轮小幅提升：上一轮 High 已修复，但 RowWinner baseline seed 不完整仍形成新的 High 级同步卡死风险 |

本轮审查范围：指定 10 组规格文档、`openspec/changes/archive/` 下全部归档变更文档，以及 `src/` 下全部源文件。自动验证：`cmake --build build -- -j2` 通过；`ctest --test-dir build --output-on-failure` 显示未注册测试；直接运行 `build/tests/tst_*` 在首个测试启动时因 Qt 符号 `_Z13qgpu_featuresRK7QString, version Qt_5` lookup error 失败，未进入业务断言。

## 上一轮修复验证（表格）

| 编号 | 状态 | 验证结论 |
| --- | --- | --- |
| H-01 | 通过 | `processBaselineRequestArtifact()` 与 `processBaselineResponseArtifact()` 均将控制报文 header `originSeq` 置为 `0`，未再调用 `nextLocalOriginSeq()`；baseline request/response 不再占用 changeset 连续序列空间。 |
| M-02 | 部分通过 | `BaselineManager::applyBaseline()` 已在 `rw.resetAll()` 后扫描 baseline 表并逐行写入 `RowWinner`，基础 incumbent 缺失问题已修复；但 seed 的 winner 没有 `winning_content`，会触发本轮 H-01。 |

## Critical 问题（如有）

本轮未发现 Critical 级问题。

## High 问题（如有）

### H-01：baseline 重建 RowWinner 时不保存 winning_content，低 rank DELETE 会被回滚并永久阻塞该 origin 流

- 位置：`src/sync/baseline/BaselineManager.cpp:401`、`src/sync/baseline/BaselineManager.cpp:405`、`src/sync/apply/ChangesetApplier.cpp:266`
- 描述：上一轮 M-02 的修复会为 baseline 行写入 `RowWinner{origin, rank, originSeq}`，但显式留下空 `contentHash/winningContent`。当前 `ChangesetApplier::updateWinnersFromChangeset()` 的低 rank DELETE 防护路径会先让 `apply_v2` 执行 DELETE，再用 incumbent 的 `winningContent` 恢复高 rank 行；当 `winningContent` 为空时直接返回 `E_SYNC_APPLY_CONSTRAINT`，整个事务回滚，`AppliedVectorStore::advance()` 不执行，也不会 ACK。结果是一个本应被忽略的陈旧低 rank DELETE 会反复重扫失败，后续同 origin 的更高 seq 因严格连续水位被卡住。
- 规格依据：设计文档 §5.6/§6.1 与计划 T1.7b/T5.1 要求 `__sync_row_winner` 保证低 rank 跨批后到不覆盖高 rank，baseline 后逐行胜者以权威切面重置。该语义应使低 rank DELETE 被 OMIT 或恢复后前进水位，而不是把同步流停在该 artifact。
- 修复建议：baseline seed RowWinner 时按 `ChangesetApplier` 的 JSON 编码规则保存完整 `winningContent` 和 `contentHash`；同时不要静默忽略 `SELECT *` 或 `rw.put()` 失败，任一表 seed 失败应回滚 baseline 并返回 `E_SYNC_BASELINE_FAILED`。补充夹具：baseline 后注入低 rank DELETE，断言业务行保留、artifact consumed、applied_vector 前进、后续 seq 可继续应用。

## Medium 问题（如有）

### M-01：同步 eligibility / diff / selection 仍只支持单列主键，窄于设计允许的复合主键与 WITHOUT ROWID 能力

- 位置：`src/sync/schema/SchemaEligibility.cpp:80`、`src/sync/selection/SelectionResolver.cpp:24`、`src/sync/diff/DiffEngine.cpp:192`
- 描述：`SchemaEligibility` 仍显式拒绝 composite PRIMARY KEY；`SelectionResolver` 与 `DiffEngine` 也只取 `pk == 1` 的单列作为行身份。错误文案已说明这是 MVP 限制，但同步设计文档 §4.4 要求普通 rowid 表或 `WITHOUT ROWID` 表均可，且应记录每表 PK 列供 selection、diff、upsert、row winner 复用。
- 修复建议：短期可在规格/能力矩阵中降级声明“同步 MVP 仅支持单列 INTEGER/TEXT PK”；若按当前设计实现，应引入统一复合 PK tuple 编码，并覆盖 eligibility、selection record、diff row key、RowWinner pkHash、FK closure 与 upsert target。

## 修复优先级汇总表（如无 C/H/M 则说明本轮通过）

| 编号 | 文件 | 严重度 | 一句话描述 |
| --- | --- | --- | --- |
| H-01 | `src/sync/baseline/BaselineManager.cpp:401` | High | baseline seed 的 RowWinner 缺少 `winning_content`，低 rank DELETE 会反复 apply 失败并阻塞后续 changeset |
| M-01 | `src/sync/schema/SchemaEligibility.cpp:80` | Medium | 实现仍拒绝复合主键，selection/diff 也只按单列 PK 建模，窄于同步设计规格 |

本轮无 Critical；上一轮 baseline 控制报文序列修复通过，RowWinner seed 修复部分通过但需要补齐 DELETE 恢复内容后才能认为 baseline fallback 语义闭环。
