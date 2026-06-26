# SQLite 同步工具实现评审报告（第二十一轮）

评审日期：2026-06-26

## 总体评分（表格）

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 功能合规性 | 90 | 第二十轮指定的 RowWinner 普通冲突路径与 baseline seed 失败回滚均已修复；Excel 导入/导出 OpenSpec 能力本轮未发现新的 C/H/M 缺陷。 |
| 同步协议正确性 | 84 | 空 `winning_content` 主风险已消除，但逐行胜者依赖“rank 全局唯一”的设计前提，配置层未强制，默认 rank=0 会让多源同序冲突退化为到达序相关。 |
| Excel 导入/导出合规性 | 91 | `columnOrder`、reverse lookup、fkInject、row lookup、time-format 的实现与规格总体一致；直接测试二进制仍受 Qt 运行时符号冲突阻断。 |
| 安全性 | 91 | 标识符引用、绑定、内部表过滤、raw WHERE 拒绝策略整体可接受；未发现新的注入类 Critical/High。 |
| 边界处理 | 84 | RowWinner 后处理写入失败未上抛；复合主键同步能力仍窄于设计；`table_state` 基线与增量哈希材料不一致，影响场景2表级判等可信度。 |
| 综合评分 | 86 | 较第二十轮上升：上一轮 High/Medium 修复通过；但本轮仍有 1 个 High 和 3 个 Medium，尚未达到通过线。 |

本轮审查范围：指定 10 组规格文档、`openspec/changes/archive/` 下全部归档变更文档，以及 `src/` 下全部源文件。自动验证：`cmake --build build -- -j2` 通过；`ctest --test-dir build --output-on-failure` 未注册测试；直接运行 `build/tests/tst_profile_loader`、`build/tests/tst_column_order_export`、`build/tests/tst_reverse_lookup_export` 均因 Qt 符号 `_Z13qgpu_featuresRK7QString` / `qgpu_features_ptr, version Qt_5` lookup error 失败，未进入业务断言。

## 上一轮修复验证（表格）

| 编号 | 状态 | 验证结论 |
| --- | --- | --- |
| H-01 | 通过 | `ChangesetApplier::conflictCb()` 已不再写入缺 `winningContent` 的临时 `RowWinner`；`updateWinnersFromChangeset()` 构造完整 JSON 行内容后调用 `RowWinnerStore::putOrRefill()`，同 rank/seq 且旧内容为空时可补全。 |
| M-01 | 通过 | `BaselineManager::applyBaseline()` 的 RowWinner seed 已把 `PRAGMA table_info` 失败、无 PK、`SELECT *` 失败、`rw.put()` 失败都转为 `seedOk=false`，随后回滚 baseline 事务并返回失败。 |
| M-02（上轮遗留） | 未解决 | `SchemaEligibility` 仍拒绝复合主键，`SelectionResolver` / `DiffEngine` 仍按单列 PK 建模；该限制仍窄于同步设计文档 §4.4，继续列为本轮 Medium。 |

## Critical 问题（如有）

本轮未发现 Critical 级问题。

## High 问题（如有）

### H-01：胜者裁决依赖“rank 全局唯一”，但 SyncConfig 未强制，默认配置会让多源冲突到达序相关

- 位置：`include/dbridge/sync/SyncConfig.h:45`、`include/dbridge/sync/SyncConfig.h:198`、`include/dbridge/sync/SyncConfig.h:283`、`src/sync/apply/ChangesetApplier.cpp:198`、`src/sync/apply/ChangesetApplier.cpp:263`、`src/sync/apply/RowWinnerStore.cpp:165`、`src/sync/conflict/ConflictArbiter.cpp:13`
- 描述：设计文档 §5.6 明确写明 `rank` 全局唯一，因此 `(rank, origin_seq)` 是全序；但 `SyncConfig::originRank()` 对未配置 origin 返回 `0`，`Builder::build()` 没有要求本节点、中心、peer 全部配置 rank，也没有拒绝重复 rank。`ChangesetApplier`、`RowWinnerStore::beats()`、`ConflictArbiter::beats()` 在 rank 相等时只比较 `originSeq`，不比较 origin，也没有稳定 tie-breaker。
- 影响：若两个 edge 未配置 rank（默认都为 0），或误配置相同 rank，则 B(seq=1) 与 D(seq=1) 更新同一行时，先到者成为 incumbent；后到者因为 `(rank, seq)` 完全相等被 `OMIT` / no-op。反向到达顺序会得到相反终态，违反计划 §5 “B/D 反序到达终态一致”和设计 §5.6 的逐行胜者到达序无关要求。
- 修复建议：首选在 `SyncConfig::Builder::build()` 中校验参与同步的全部 origin rank 已显式配置且全局唯一，未配置/重复时初始化失败并返回明确错误；同时在裁决函数中加入防御性稳定 tie-breaker（例如 `(rank, originSeq, originId)`），避免配置漂移直接破坏收敛。补充夹具：两个不同 origin 同 rank 同 seq 冲突，按 B→D 与 D→B 两种顺序应用，断言要么配置被拒绝，要么两序终态一致。

## Medium 问题（如有）

### M-01：普通 changeset 后处理写 RowWinner 失败被忽略，可能提交缺胜者状态的业务变更

- 位置：`src/sync/apply/ChangesetApplier.cpp:442`、`src/sync/apply/ChangesetApplier.cpp:453`、`src/sync/apply/RowWinnerStore.cpp:120`
- 描述：`updateWinnersFromChangeset()` 已正确构造完整 `winningContent`，但调用 `winners.putOrRefill(..., nullptr)` 后没有检查返回值，也丢弃错误信息。若 `__sync_row_winner` 写入失败，`ChangesetApplier::apply()` 仍返回成功，外层 `CapturedWriteTemplate::branchA()` 会继续推进 `applied_vector`、写 changelog 并提交事务。
- 影响：业务行已经应用，但逐行胜者状态缺失或未更新。后续低 rank UPDATE/DELETE 可能不被支配，重新出现到达序相关或低 rank 删除胜者的问题。触发需要 RowWinner 写库失败，频率低于 H-01，但违反设计 §5.4 “apply 三件套同事务”和 §5.6 RowWinner 同事务维护的原子性要求。
- 修复建议：为 `putOrRefill()` 传入 `QString putErr` 并检查返回值；失败时设置 `err`，`updateWinnersFromChangeset()` 返回 `false`，由外层回滚整个 apply。补充注入式测试：让 `__sync_row_winner` 写入失败，断言业务表、`applied_vector`、changelog 均不提交。

### M-02：同步 eligibility / diff / selection 仍只支持单列主键，窄于设计允许的复合主键与 WITHOUT ROWID 能力

- 位置：`src/sync/schema/SchemaEligibility.cpp:80`、`src/sync/selection/SelectionResolver.cpp:18`、`src/sync/diff/DiffEngine.cpp:186`
- 描述：`SchemaEligibility` 仍显式拒绝 composite PRIMARY KEY；`SelectionResolver` 只取 `pk == 1` 的单列，`DiffEngine::getPkColumn()` 也只返回最小 pk 序号列。错误文案说明这是 MVP 限制，但同步设计文档 §4.4 要求普通 rowid 表或 `WITHOUT ROWID` 表均可，并应记录每表 PK 列供 selection、diff、upsert、row winner 复用。
- 影响：合法 SQLite 业务表（复合 PK、WITHOUT ROWID 复合键）无法进入同步；若后续模块绕过 eligibility，也会在 selection/diff 阶段丢失行身份的一部分。该问题已在上一轮列出，本轮未修复。
- 修复建议：短期在规格/能力矩阵中明确“同步 MVP 仅支持单列 INTEGER/TEXT PK”；若按当前设计实现，应引入统一复合 PK tuple 编码，并覆盖 eligibility、selection record、diff row key、RowWinner pkHash、FK closure 与 upsert target。

### M-03：`table_state` 基线重建与增量维护使用不同哈希材料，场景2表级判等可能假红/假绿

- 位置：`src/sync/schema/TableStateStore.cpp:40`、`src/sync/schema/TableStateStore.cpp:117`、`src/sync/schema/TableStateStore.cpp:160`、`src/sync/apply/CapturedWriteTemplate.cpp:266`、`src/sync/apply/CapturedWriteTemplate.cpp:464`
- 描述：`TableStateStore::resetFromBaseline()` 扫全表时使用 `rowHash(QVariantMap)`，材料是按列名排序的 `key=value\n`；但增量路径传入的 `beforeHash/afterHash` 来自 `CapturedWriteTemplate` 的裸值序列：branch B/C 用 `RowMutation.values` 或 `SELECT *` 值序列，branch A 用 changeset iterator 的列序列。`applyMutations()` 直接用这些 hash 做 `+H(new)-H(old)`，因此 baseline 的旧行 hash 与增量删除/更新时扣减的旧行 hash 不是同一个函数。
- 影响：基线后发生 UPDATE/DELETE 时，checksum delta 不能正确抵消旧行，`__sync_table_state.content_checksum` 会漂移。DiffEngine 按设计只看 `schema_fingerprint + row_count + content_checksum` 判等，漂移会导致内容一致时报 Different，或在模加碰撞/抵消时漏报差异。该问题不直接破坏业务数据，但会削弱场景2零全量表级比对的可信度。
- 修复建议：抽出唯一的 row fingerprint 规范（列名、类型标签、NULL、BLOB、整数格式、列排序均固定），baseline 重建、changeset 提取、RowMutation pre-scan/after-scan 全部复用同一函数。对 UPDATE 建议在写后按 PK 重新读取完整行生成 afterHash，而不是只 hash 本次 mutation 的列值。补充夹具：baseline 后更新一行再把表内容恢复一致，断言两端 table_state checksum 与全表重算一致。

## 修复优先级汇总表（如无 C/H/M 则说明本轮通过）

| 编号 | 文件 | 严重度 | 一句话描述 |
| --- | --- | --- | --- |
| H-01 | `include/dbridge/sync/SyncConfig.h:45` | High | rank 未配置默认 0 且不校验唯一，导致同 rank/同 seq 多源冲突按到达顺序决定终态。 |
| M-01 | `src/sync/apply/ChangesetApplier.cpp:453` | Medium | `putOrRefill()` 写失败被忽略，业务变更可能提交但 RowWinner 缺失。 |
| M-02 | `src/sync/schema/SchemaEligibility.cpp:80` | Medium | 复合主键仍被拒绝，selection/diff 也只建模单列 PK。 |
| M-03 | `src/sync/schema/TableStateStore.cpp:117` | Medium | baseline 与增量使用不同 row hash 口径，`table_state` 校验和会漂移。 |

本轮未通过：无 Critical，但仍有 1 个 High、3 个 Medium。建议先修 H-01，再修 M-01；M-03 影响场景2可信度，应在场景2继续扩展前收口。
