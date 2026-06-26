# SQLite 同步工具实现评审报告（第二十四轮）

评审日期：2026-06-26

## 总体评分（表格）

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 功能合规性 | 89 | 上一轮 `origin_seq` 失败回滚修复已落地；同步主干、导入导出主干与规格总体一致。仍有本轮发现的无变更成功路径跳号，以及复合主键能力窄于当前同步设计的问题。 |
| 同步协议正确性 | 84 | 事务失败后的序号回退已补齐，但本地/selection push 的“成功但没有 changelog”路径仍会消耗 `origin_seq`，可让对端严格连续校验进入永久 gap。Branch B/C 的 `table_state` 还会把 `INSERT OR IGNORE` 的 no-op 行计入校验和。 |
| Excel 导入/导出合规性 | 92 | `columnOrder`、reverse lookup、fkInject、row lookup、time-format 的 loader/validator/service 主干未发现新的 C/H/M 偏差。 |
| 安全性 | 90 | SQL 标识符引用与绑定整体可接受；本轮未发现新的注入或越权类 C/H/M 问题。 |
| 边界处理 | 85 | 失败事务回退改善明显；但无变更、DoNothing、复合 PK、测试注册/运行环境仍是主要薄弱点。 |
| 综合评分 | 87 | 较第二十三轮的指定 High 已修复，但本轮新增 1 个 High、2 个 Medium，未达到通过标准。 |

本轮审查范围：指定 10 组规格文档、`openspec/changes/archive/` 下归档变更文档，以及 `src/` 下全部源文件。验证命令：`cmake --build build -- -j2` 通过；`ctest --test-dir build --output-on-failure` 返回 “No tests were found”；直接运行 `build/tests/tst_*` 时首个二进制因 Qt 运行时符号 `_Z13qgpu_featuresRK7QString, version Qt_5` lookup error 失败，未进入业务断言。

## 上一轮修复验证（表格）

| 编号 | 状态 | 验证结论 |
| --- | --- | --- |
| H-01 | 通过 | `SyncWorker::rollbackOriginSeq()` 已新增，且上一轮点名的三处事务失败路径均保存 `prevSeq` 并在失败后回退：selection push 的 `tpl_->execute()` 失败路径见 `src/sync/SyncWorker.cpp:767`、`src/sync/SyncWorker.cpp:781`；导入的 `sealInto()`、`resetFromBaseline()`、`commit()` 失败路径见 `src/sync/SyncWorker.cpp:1457`、`src/sync/SyncWorker.cpp:1464`、`src/sync/SyncWorker.cpp:1485`、`src/sync/SyncWorker.cpp:1500`；comparison/captured write 的失败路径见 `src/sync/SyncWorker.cpp:1580`、`src/sync/SyncWorker.cpp:1589`。该修复覆盖“事务失败回滚”场景。 |

## Critical 问题（如有）

本轮未发现 Critical 级问题。

## High 问题（如有）

### H-01：无变更成功路径仍会消耗本地 `origin_seq`，下一次真实 changeset 会跳号

- 位置：`src/sync/SyncWorker.cpp:768`、`src/sync/SyncWorker.cpp:1458`、`src/sync/SyncWorker.cpp:1581`、`src/sync/capture/SessionRecorder.cpp:70`、`src/sync/capture/SessionRecorder.cpp:74`、`src/sync/apply/CapturedWriteTemplate.cpp:342`、`src/sync/apply/CapturedWriteTemplate.cpp:445`
- 描述：上一轮修复只处理了失败回滚路径，但 `SessionRecorder::sealInto()` 在 `changeset.isEmpty()` 时返回成功并设置 `outLocalSeq=0`，不会写入 `__sync_changelog`。调用方已经在进入 `sealInto()` 前调用 `nextLocalOriginSeq()`，且 `res.ok == true` 时不会回退。可触发场景包括：导入文件没有产生实际 DB 变更；comparison save 的 staged row 与当前值相同；selection push 的 dependency 行走 `INSERT OR IGNORE` 且全部已存在；以及全 PK 列 upsert 退化为 DO NOTHING。
- 影响：内存中的 `localOriginSeq_` 推进了，但没有任何 payload 可广播。下一次真实本地写入会使用更大的 `origin_seq`；对端 `AppliedVectorStore::check()` 要求首包为 1、后续严格 `applied_seq + 1`，因此会把后续真实 payload 判为 `GAP_PENDING`，等待一个永远不存在的序号。该问题与上一轮 High 同属协议层跳号，只是从“失败路径”转移到“无变更成功路径”。
- 修复建议：不要在确认存在实际 changeset 之前推进可观察的 `origin_seq`。可选方案：让 `sealInto()` 区分 `NoChanges`，调用方在 `localSeq == 0` 时回退 `localOriginSeq_` 且不要更新 `table_state` high water；或改为先采集 changeset，再仅在非空 changeset 内分配并写入持久化 origin sequence。补充回归：对导入空变更、captured write 同值保存、selection push 全 dependency 已存在三种路径断言下一次真实 changelog 的 `origin_seq` 不跳号。

## Medium 问题（如有）

### M-01：Branch B/C 的 `table_state` 基于请求行而非实际 changeset，`INSERT OR IGNORE` no-op 会污染校验和

- 位置：`src/sync/apply/UpsertExecutor.cpp:102`、`src/sync/apply/UpsertExecutor.cpp:120`、`src/sync/apply/CapturedWriteTemplate.cpp:256`、`src/sync/apply/CapturedWriteTemplate.cpp:306`、`src/sync/apply/CapturedWriteTemplate.cpp:410`、`src/sync/apply/CapturedWriteTemplate.cpp:429`、`src/sync/schema/TableStateStore.cpp:40`
- 描述：selection push 的 dependency 行使用 `DoNothing`，`UpsertExecutor` 生成 `INSERT OR IGNORE`；全 PK 列更新也退化为 `INSERT OR IGNORE`。这些 SQL 在冲突时不会改动业务表，SQLite session 也不会产生 changeset，但 `CapturedWriteTemplate::branchBC()` 仍按原始 `RowMutation` 构造 `TableMutation`，把 `afterHash` 加进 `__sync_table_state`。
- 影响：`__sync_table_state` 可能表示一个并未落库的行版本，导致后续 DiffEngine/一致性缓存误判表内容是否一致。该问题尤其容易出现在选择推送依赖闭包中：依赖行本应“存在则不覆盖”，但校验和会被当成已覆盖后的内容。
- 修复建议：Branch B/C 的 `table_state` 增量应来自实际捕获 changeset，而不是输入 mutation；或者至少在每行 upsert 后检查 `sqlite3_changes()`/`QSqlQuery::numRowsAffected()` 并跳过 no-op 行。更稳妥的是复用 `extractMutations(changeset, syncTables)` 作为唯一增量来源；当 changeset 为空时不推进 high water、不改 `table_state`。

### M-02：同步 eligibility、selection、diff 仍只支持单列主键，窄于当前设计文档允许的复合主键能力

- 位置：`src/sync/schema/SchemaEligibility.cpp:80`、`src/sync/selection/SelectionResolver.cpp:24`、`src/sync/diff/DiffEngine.cpp:177`、`src/sync/diff/ComparisonSession.cpp:413`
- 描述：`SchemaEligibility` 仍拒绝复合主键并返回 `E_SYNC_COMPOSITE_PK_NOT_SUPPORTED`；`SelectionResolver`、`DiffEngine`、`ComparisonSession` 也只取 `pk == 1` 或最小 pk 序号的单列。同步设计文档 §4.4 的 eligibility 表要求普通 rowid 表或 `WITHOUT ROWID` 表只要有显式非空 PRIMARY KEY 即可，并未把能力限制到单列 PK。
- 影响：合法 SQLite 业务表，例如复合业务主键或 `WITHOUT ROWID` 复合主键表，无法按当前规格进入同步。若未来绕过 eligibility，选择、diff、row identity 和 upsert conflict target 也会丢失主键的一部分。
- 修复建议：短期在规格和计划中明确“MVP 仅支持单列 PK”并把复合 PK 作为后置能力；若要满足现设计，应引入统一 PK tuple 编码，贯穿 SyncSelection、FK closure、DiffEngine、RowWinner pkHash、TableState、UpsertExecutor 与 baseline seeding。

## 修复优先级汇总表（如无 C/H/M 则说明本轮通过）

| 编号 | 文件 | 严重度 | 一句话描述 |
| --- | --- | --- | --- |
| H-01 | `src/sync/capture/SessionRecorder.cpp:70` | High | `sealInto()` 对空 changeset 返回成功但不写 changelog，调用方已消耗 `origin_seq`，下一次真实 payload 会跳号。 |
| M-01 | `src/sync/apply/CapturedWriteTemplate.cpp:410` | Medium | Branch B/C 用请求 mutation 更新 `table_state`，会把 `INSERT OR IGNORE` 没有落库的行计入校验和。 |
| M-02 | `src/sync/schema/SchemaEligibility.cpp:80` | Medium | 实现仍拒绝复合主键并在 selection/diff/save 中只使用单列 PK，窄于当前同步设计文档。 |

本轮未通过：无 Critical，但有 1 个 High、2 个 Medium。建议优先修复无变更成功路径的 `origin_seq` 分配语义；否则上一轮的失败回滚修复仍不能保证本地 changeset 流严格连续。
