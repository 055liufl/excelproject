# SQLite 同步工具实现评审报告（第二十六轮）

评审日期：2026-06-26

## 总体评分（表格）

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 功能合规性 | 88 | 上一轮 `push_id` 屏障与 legacy time-format null 拒绝已落地；导入导出新增能力主干仍较完整。但同步 changeset 入站/下行路径仍有关键协议语义未实现。 |
| 同步协议正确性 | 80 | selection push 半截外泄问题已修复；但下行 changeset 始终走非权威 apply，`syncTables` allow-list 未传入入站 Branch A，`ConflictPolicy` 配置未被消费，复合 PK 能力仍窄于规格。 |
| Excel 导入/导出合规性 | 93 | `columnOrder`、reverse lookup、fkInject、row lookup、time-format 主路径未发现新的 C/H 偏差；ProfileLoader legacy null 修复通过。 |
| 安全性 | 88 | SQL 标识符引用与绑定总体可接受；但入站 changeset 未带同步表 allow-list，配置了 `syncTables` 子集时仍可能应用载荷中的非同步业务表变更。 |
| 边界处理 | 84 | ACK/ledger/selection push 屏障较上一轮改善；下行权威、策略配置、复合主键、CTest 注册仍是主要薄弱点。 |
| 综合评分 | 84 | 较第二十五轮修复了 1 个 High 和 1 个 Medium，但本轮新发现 2 个 High、2 个 Medium，未达到通过标准。 |

验证命令：`cmake --build build -- -j2` 通过；`ctest --test-dir build --output-on-failure` 仍返回 `No tests were found!!!`，说明测试目标已构建但 CTest 未注册用例。

## 上一轮修复验证（表格）

| 编号 | 状态 | 验证结论 |
| --- | --- | --- |
| H-01 | 通过 | `CapturedWriteTemplate::branchBC()` 已把 `p.pushId` 传入 `SessionRecorder::sealInto()`，`SessionRecorder` 再传给 `ChangelogStore::append()`；广播屏障也已移除 `entry.origin != config_.nodeId()` 条件，仅按 `entry.pushId` 查询未完成 push，见 `src/sync/apply/CapturedWriteTemplate.cpp:342`、`src/sync/capture/SessionRecorder.cpp:77`、`src/sync/SyncWorker.cpp:1155`。selection push 分片 changeset 可以被 `push_id` 精确拦截。 |
| M-02 | 通过 | legacy `excelFormat`、`excelFormatFallback`、`dbFormat` 的显式 JSON null 已分别拒绝，见 `src/profile/ProfileLoader.cpp:221`、`src/profile/ProfileLoader.cpp:232`、`src/profile/ProfileLoader.cpp:286`。这与 time-format 对 null 不得等同 empty 的要求一致。 |

## Critical 问题（如有）

本轮未发现 Critical 级问题。

## High 问题（如有）

### H-01：下行 changeset 没有进入 `AuthoritativeApply`，中心权威收敛语义不成立

- 位置：`src/sync/apply/CapturedWriteTemplate.cpp:105`、`src/sync/apply/CapturedWriteTemplate.cpp:107`、`src/sync/apply/ChangesetApplier.cpp:131`、`src/sync/SyncWorker.cpp:597`
- 描述：需求 §FR-9 与设计 §5.6 要求中心 rebase 后的下行广播以 `AuthoritativeApply` 强制 `REPLACE` 应用，不受本地冲突策略或既有行胜者影响。实现中 Branch A 对所有入站 changeset 固定 `opts.authoritative = false`，`ChangesetApplier` 因此总会执行非权威路径的 `updateWinnersFromChangeset()` 和 row-winner 裁决。`SyncWorker::processChangesetArtifact()` 也没有根据物理发送者、中心角色、routeTag 或 payload 标记区分“中心权威下行”和“普通非权威入站”。
- 影响：Edge 收到中心广播时仍可能被本地 `__sync_row_winner` 的既有高 rank/seq 记录裁掉，无法保证“落败侧被中心权威值覆盖”和“所有下游强制收敛中心终态”。这直接违反设计 §5.6 “下游 `AuthoritativeApply`（强制 REPLACE）” 和计划 M2 DoD “Edge 配 `TargetWins/Manual` 仍收敛中心权威下行”。
- 修复建议：在 payload/header 中显式标记权威下行，或由 `senderPeer == centerNodeId` 且 routeTag/拓扑关系验证后进入权威 apply；Branch A 应把该判断传给 `ApplyOptions::authoritative`。补充回归：Edge 先有本地高 rank/未胜者状态，再收到中心 rebase 后 changeset，断言最终值等于中心值且不更新非权威 row_winner。

### H-02：入站 changeset 未传 `canonicalSyncTables_`，配置子集时 filter 退化为接受所有业务表

- 位置：`src/sync/SyncWorker.cpp:597`、`src/sync/SyncWorker.cpp:606`、`src/sync/apply/ChangesetApplier.cpp:101`、`src/sync/apply/ChangesetApplier.cpp:151`、`src/sync/apply/CapturedWriteTemplate.cpp:129`
- 描述：`ChangesetApplier::filterCb()` 的语义是 `syncTables` 为空时接受所有非 `__sync_*` 表；`CapturedWriteTemplate::extractMutations()` 也用同一 allow-list 更新 table_state。但 `processChangesetArtifact()` 构造 Branch A `WriteParams` 时只填 origin/seq/schema/blob，没有设置 `p.syncTables = canonicalSyncTables_`。因此只要本节点配置了同步表子集，入站 changeset 仍会对载荷里的任意用户表执行 apply，并把这些非同步表写入 table_state。
- 影响：节点配置 `syncTables=["orders"]` 时，一个包含 `customers` 或其它业务表的 changeset 仍可能落库，突破“同步表集合”边界。合法但配置不一致的 peer、损坏载荷或误投载荷都可能修改本端不该由同步引擎管理的数据。
- 修复建议：`processChangesetArtifact()` 设置 `p.syncTables = canonicalSyncTables_`；对载荷中全被 filter 掉的 changeset 明确 no-op/ACK 语义。补充测试：本端只同步表 A，入站 changeset 同时含 A/B，断言 B 不变、table_state 不含 B，且 applied_vector/changelog 行为符合协议。

## Medium 问题（如有）

### M-01：`ConflictPolicy` 公开配置未被应用，`TargetWins` / `Manual` 语义形同虚设

- 位置：`include/dbridge/sync/SyncConfig.h:42`、`include/dbridge/sync/SyncConfig.h:194`、`src/sync/apply/ChangesetApplier.cpp:178`
- 描述：公开 API 暴露 `SyncConfig::conflictPolicy()` 与 Builder `conflictPolicy()`，需求 §5.2 定义 `SourceWins`、`TargetWins`、`Manual` 三档策略，并说明非权威入站 changeset 应受该策略约束。但源码中除 SyncConfig 存取外没有任何对 `config_.conflictPolicy()` 的消费，`ChangesetApplier::conflictCb()` 只依据 `authoritative` 和 `RowWinnerStore` rank/seq 决定 `REPLACE/OMIT`。
- 影响：调用方配置 `TargetWins` 或 `Manual` 不会改变行为，既没有保留目标端值，也没有暂存人工裁决项。这是公开 API 的静默失效，并会误导宿主应用以为冲突策略已经生效。
- 修复建议：将 `ConflictPolicy` 传入 `ApplyOptions` 或 `ConflictCtx`；非权威 `DATA/CONFLICT` 按策略返回 `REPLACE/OMIT/ABORT+暂存`，权威下行则继续豁免策略。补充三档策略单元测试。

### M-02：同步 eligibility、selection、diff 仍只支持单列主键，窄于规格的显式非空主键范围

- 位置：`src/sync/schema/SchemaEligibility.cpp:80`、`src/sync/selection/SelectionResolver.cpp:14`、`src/sync/selection/SelectionResolver.cpp:24`、`src/sync/diff/DiffEngine.cpp:177`、`src/sync/diff/DiffEngine.cpp:190`
- 描述：实现仍显式拒绝复合主键并报 `E_SYNC_COMPOSITE_PK_NOT_SUPPORTED`，selection/diff/comparison 也只取 `pk == 1` 或最小 pk 序号作为行身份。当前需求与设计的 eligibility 写的是“普通表 + 显式非空 PRIMARY KEY + 可用冲突目标”，并包含 `WITHOUT ROWID` 表，没有正式把同步表收窄到单列 PK。
- 影响：复合业务主键表、常见多租户 `(tenant_id, id)` 主键表、`WITHOUT ROWID` 复合主键表无法进入同步；若后续绕过 eligibility，选择推送、行级 diff、row identity 都会丢失主键的一部分。
- 修复建议：若 MVP 决策是单列 PK，需同步修改需求/设计/计划并把错误码列入正式错误表；若按当前规格交付，应实现统一 PK tuple 编码，贯穿 selection、FK closure、DiffEngine、RowWinner pkHash、TableState 与 baseline。

## 修复优先级汇总表（如无 C/H/M 则说明本轮通过）

| 编号 | 文件 | 严重度 | 一句话描述 |
| --- | --- | --- | --- |
| H-01 | `src/sync/apply/CapturedWriteTemplate.cpp:107` | High | 下行 changeset 固定按非权威 apply，中心 rebase 后广播不能保证强制覆盖并收敛下游。 |
| H-02 | `src/sync/SyncWorker.cpp:606` | High | 入站 Branch A 未传 `canonicalSyncTables_`，配置同步表子集时 changeset filter 接受所有业务表。 |
| M-01 | `src/sync/apply/ChangesetApplier.cpp:178` | Medium | `SyncConfig::conflictPolicy()` 未接入 apply 路径，`TargetWins` / `Manual` 配置静默无效。 |
| M-02 | `src/sync/schema/SchemaEligibility.cpp:80` | Medium | 实现拒绝复合 PK 且多处只用单列 PK，窄于当前规格的显式非空主键能力。 |

本轮未通过：无 Critical，但有 2 个 High、2 个 Medium。建议优先修复下行 `AuthoritativeApply` 和入站 changeset allow-list；这两项直接影响同步边界与中心权威收敛。
