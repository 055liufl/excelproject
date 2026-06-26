# SQLite 同步工具实现评审报告（第二十三轮）

评审日期：2026-06-26

## 总体评分（表格）

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 功能合规性 | 90 | 第二十二轮指定 4 项修复均已落地；同步主干继续向设计靠拢。仍存在本地 origin_seq 事务外分配导致严格连续协议可被错误路径打断的问题，以及复合主键能力窄于设计文档的问题。 |
| 同步协议正确性 | 86 | allow-list、table_state、哈希口径和回滚语义明显改善；但本地 changeset 序列在失败事务中被消耗，会制造对端永久 GAP_PENDING，属于协议层 High 风险。 |
| Excel 导入/导出合规性 | 92 | columnOrder、reverse lookup、fkInject、row lookup、time-format 的 loader/validator/export/import 主干未发现新的 C/H/M 偏差。 |
| 安全性 | 90 | SQL 标识符引用和绑定整体可接受；上轮 changeset 后处理绕过 allow-list 的边界风险已修复。 |
| 边界处理 | 86 | table_state 写失败现在回滚，导入后会重算；但失败路径的本地序号、复合 PK/WITHOUT ROWID 能力、自动化测试可运行性仍是主要薄弱点。 |
| 综合评分 | 88 | 较第二十二轮有实质进展，但本轮仍有 1 个 High、1 个 Medium，未达到通过标准。 |

本轮审查范围：指定 10 组规格文档、`openspec/changes/archive/` 下归档变更文档，以及 `src/` 下全部源文件。验证命令：`cmake --build build -- -j2` 通过；`ctest --test-dir build --output-on-failure` 返回 “No tests were found”；直接运行 `build/tests/tst_*` 时首个二进制因 Qt 运行时符号 `_Z13qgpu_featuresRK7QString, version Qt_5` lookup error 失败，未进入业务断言。

## 上一轮修复验证（表格）

| 编号 | 状态 | 验证结论 |
| --- | --- | --- |
| H-01 | 通过 | `ChangesetApplier::isAllowedSyncTable()` 已抽出并被 `filterCb()`、`updateWinnersFromChangeset()`、`CapturedWriteTemplate::extractMutations()` 共享；`__sync_*` 和非同步表不再进入 RowWinner/table_state 后处理。见 `src/sync/apply/ChangesetApplier.cpp:144`、`src/sync/apply/ChangesetApplier.cpp:159`、`src/sync/apply/ChangesetApplier.cpp:269`、`src/sync/apply/CapturedWriteTemplate.cpp:508`。 |
| M-01 | 通过 | `submitImportSync()` 在 `SessionRecorder::sealInto()` 成功后、`txn.commit()` 前调用 `TableStateStore::resetFromBaseline()`，失败时回滚事务；空 `syncTables` 也会在 worker 初始化时展开为用户表清单。见 `src/sync/SyncWorker.cpp:219`、`src/sync/SyncWorker.cpp:1450`、`src/sync/SyncWorker.cpp:1473`。 |
| M-02 | 通过 | Branch B/C 的 `beforeHash` 和 `afterHash` 已改为构造 `QVariantMap` 后调用 `TableStateStore::rowHash()`，与 baseline 全扫口径一致；当前调用方传入的是完整 frozen/staged row。见 `src/sync/apply/CapturedWriteTemplate.cpp:273`、`src/sync/apply/CapturedWriteTemplate.cpp:293`、`src/sync/diff/StagingBuffer.cpp:71`。 |
| M-03 | 通过 | Branch A 与 Branch B/C 在 `TableStateStore::applyMutations()` 失败时均回滚并返回错误，不再降级 warning 后提交。见 `src/sync/apply/CapturedWriteTemplate.cpp:135`、`src/sync/apply/CapturedWriteTemplate.cpp:429`。 |

## Critical 问题（如有）

本轮未发现 Critical 级问题。

## High 问题（如有）

### H-01：本地 `origin_seq` 在事务成功前自增，失败路径会制造严格连续协议无法补齐的缺口

- 位置：`src/sync/SyncWorker.cpp:765`、`src/sync/SyncWorker.cpp:1450`、`src/sync/SyncWorker.cpp:1516`、`src/sync/SyncWorker.cpp:1562`、`src/sync/apply/CapturedWriteTemplate.cpp:342`、`src/sync/apply/CapturedWriteTemplate.cpp:429`、`src/sync/capture/SessionRecorder.cpp:77`、`src/sync/apply/AppliedVectorStore.cpp:24`
- 描述：`nextLocalOriginSeq()` 直接 `++localOriginSeq_`，但它在 selection push 接收、本地导入、ComparisonSession save 等路径调用时，均发生在 `CapturedWriteTemplate` 或手写事务真正 commit 之前。若后续 `sealInto()` 之后的 `resetFromBaseline()`、`applyMutations()`、push progress 更新或 commit 失败，业务/changelog 事务会回滚，但内存中的 `localOriginSeq_` 不会回退。
- 影响：下一次成功本地写会写入更大的 `origin_seq`，而被回滚的序号没有任何 changelog 制品。对端 `AppliedVectorStore::check()` 要求首包为 1、后续严格 `applied_seq + 1`，因此会把后续 payload 判为 `GAP_PENDING`，等待一个永远不会出现的缺口。该问题可导致当前进程生命周期内的广播同步停滞；重启后从 `MAX(origin_seq)` 重建内存序号只能缓解，不是协议保证。
- 修复建议：把本地序号分配纳入同一事务和持久化状态。可选方案：新增 `__sync_origin_seq(origin, stream_epoch, next_seq)` 表，用 `BEGIN IMMEDIATE` 内的 `UPDATE ... RETURNING` 或等价 SELECT+UPDATE 分配；或先用候选序号，只有 commit 成功后才推进内存计数，失败时从 `MAX(origin_seq)` 重新校准。补充回归：在导入 seal 后 table_state 失败、Branch C applyMutations 失败、selection push chunk progress 失败三处注入失败，断言下一次成功 changelog 的 `origin_seq` 不跳号，远端不会进入 gap。

## Medium 问题（如有）

### M-01：同步 eligibility、selection、diff 仍只支持单列主键，窄于当前设计文档允许的复合主键能力

- 位置：`src/sync/schema/SchemaEligibility.cpp:80`、`src/sync/selection/SelectionResolver.cpp:24`、`src/sync/diff/DiffEngine.cpp:177`、`src/sync/diff/ComparisonSession.cpp:413`
- 描述：`SchemaEligibility` 明确拒绝复合主键并返回 `E_SYNC_COMPOSITE_PK_NOT_SUPPORTED`，`SelectionResolver`、`DiffEngine`、`ComparisonSession` 也只取 `pk == 1` 或最小 pk 序号的单列。同步设计文档 §4.4 的 eligibility 表仍要求普通 rowid 表或 `WITHOUT ROWID` 表均可，只要有显式非空 PRIMARY KEY，并未把能力限制到单列 PK。
- 影响：合法 SQLite 业务表，例如复合业务主键或 `WITHOUT ROWID` 复合主键表，无法按当前规格进入同步。若未来绕过 eligibility，选择、diff、row identity 和 upsert conflict target 也会丢失主键的一部分。
- 修复建议：短期在规格和计划中明确 “MVP 仅支持单列 PK” 并把复合 PK 作为后置能力；若要满足现设计，应引入统一 PK tuple 编码，贯穿 SyncSelection、FK closure、DiffEngine、RowWinner pkHash、TableState、UpsertExecutor 与 baseline seeding。

## 修复优先级汇总表（如无 C/H/M 则说明本轮通过）

| 编号 | 文件 | 严重度 | 一句话描述 |
| --- | --- | --- | --- |
| H-01 | `src/sync/SyncWorker.cpp:1516` | High | 本地 origin_seq 在事务提交前自增，失败回滚后会跳号，使远端严格连续应用永久等待缺失 payload。 |
| M-01 | `src/sync/schema/SchemaEligibility.cpp:80` | Medium | 实现仍拒绝复合主键并在 selection/diff/save 中只使用单列 PK，窄于当前同步设计文档。 |

本轮未通过：无 Critical，但有 1 个 High、1 个 Medium。建议优先修复 `origin_seq` 事务化分配；否则上一轮刚收紧的 table_state 回滚语义会放大跳号风险，导致同步流在错误恢复路径上停滞。
