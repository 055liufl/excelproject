# SQLite 同步工具实现评审报告（第十八轮）

评审日期：2026-06-26

## 总体评分（表格）

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 功能合规性 | 88 | 第十七轮指定修复均已落地；Excel 导入/导出与 reverse lookup 主要路径未发现新的 C/H/M 缺陷 |
| 同步协议正确性 | 80 | selection push、ACK、baseline cut 修复有效；但 baseline 控制报文消耗 origin_seq 会制造 changeset 序列空洞 |
| Excel 导入/导出合规性 | 90 | mixed FK preflight 回写、route-local skip、exportOnMissing:error 行跳过语义已收口 |
| 安全性 | 90 | SQL 标识符引用、绑定、内部表过滤、raw addWhere 拒绝策略整体可接受；未发现新的注入类 Critical |
| 边界处理 | 80 | 复合主键 / WITHOUT ROWID 支持仍窄于设计；baseline 后 row winner 未按权威切面重建 |
| 综合评分 | 84 | 上一轮 6 项显式修复通过验证，评分较第十七轮提升；仍不建议在 baseline fallback 与复合主键场景宣布完成 |

本轮审查范围：指定规格文档、`openspec/specs/`、`openspec/changes/archive/` 归档变更，以及 `src/` 下全部源文件；同时核对了同步公共头文件。未执行编译或自动化测试。

## 上一轮修复验证（表格）

| 编号 | 状态 | 验证结论 |
| --- | --- | --- |
| H-01 | 通过 | `ImportService.cpp` mixed 模式已建立 `excelRow -> contexts` 映射，并将 `ForeignKeyPreflight::check()` 写入临时 `clsContexts` 的 `failedRouteIndices` 合并回原始 `contexts`，写入阶段可见。 |
| H-02 | 通过 | `processSelectionPushArtifact()` 已改用中心自己的 `origin=config_.nodeId()`、`streamEpoch_`、`nextLocalOriginSeq()` 与中心 rank，消除了远端 origin 与中心 epoch/seq 混用。 |
| M-01 | 通过 | `BaselineManager::exportBaseline()` 新增 `localOrigin/localEpoch/localOriginSeq` 参数，并在 manager 内合并 self origin cut；主流程调用也传入了本节点 cut。 |
| M-02 | 通过 | `OutboundAckStore::minUnackedLocalSeq()` 已去掉 `cl.stream_epoch = ?` 过滤，以 `(peer, origin, stream_epoch)` LEFT JOIN 计算跨 epoch 未 ACK 的最小 `local_seq` 下界。 |
| M-03 | 通过 | `SyncConfig::Builder::build()` 已对 lag soft/hard seq、bytes、ms 各字段先做正数校验，再做 soft <= hard 关系校验。 |
| M-05 | 通过 | `enqueueSelectionPush()` 在 resolver、空选择、FK closure、chunk streamer 失败出口均调用 `cancelAckWait()`，会清理 `ackWaiting_`、deadline、`pendingPushId_` 和 `pendingAckWindow_`。 |

## Critical 问题（如有）

本轮未发现 Critical 级问题。

## High 问题（如有）

### H-01：baseline request/response 消耗本地 origin_seq，但接收端只对 changeset 推进 applied_vector，后续 changeset 会永久表现为缺口

- 位置：`src/sync/SyncWorker.cpp:864`、`src/sync/SyncWorker.cpp:1331`、`src/sync/apply/AppliedVectorStore.cpp:20`
- 描述：`processBaselineRequestArtifact()` 发送 baseline response 时调用 `nextLocalOriginSeq()` 写入 header；`runBaselineFallbackFor()` 发送 baseline request 时也调用 `nextLocalOriginSeq()`。这两类 artifact 不是 changeset，不会经 `CapturedWriteTemplate::branchA()` 调用 `AppliedVectorStore::advance()`，接收端也不会对 baseline request/response 的 `(origin, stream_epoch, origin_seq)` 建水位。若节点 A 已发送 changeset seq=1，随后发送 baseline request seq=2，再发送普通 changeset seq=3，对端 applied_vector 仍停在 1；seq=3 会被 `AppliedVectorStore::check()` 判为 gap，触发 pending/baseline fallback。baseline fallback 本身又可能继续消耗控制报文序列，造成反复缺口或无意义 re-baseline。
- 规格依据：设计文档 §6/G-05 明确严格连续 applied_vector 规则“只管 changeset 流”，selectionpush 分片也独立幂等；控制报文不应占用 changeset 的连续 `origin_seq` 空间，除非也被同一水位机制确认推进。
- 修复建议：控制报文 header 的 `originSeq` 使用 0 或独立控制序列，不调用 `nextLocalOriginSeq()`；或将控制报文也持久化进同一 origin stream 并定义接收端 advance/ACK 语义。更简单的兼容修复是 baseline request/response 不参与 changeset origin_seq，并确保 artifact 名称用 UUID/时间戳去重。

## Medium 问题（如有）

### M-01：同步 eligibility / diff / selection 仍只支持单列主键，窄于规格允许的复合主键与 WITHOUT ROWID 能力

- 位置：`src/sync/schema/SchemaEligibility.cpp:80`、`src/sync/selection/SelectionResolver.cpp:14`、`src/sync/diff/DiffEngine.cpp:177`
- 描述：`SchemaEligibility` 直接拒绝 composite PRIMARY KEY；`SelectionResolver` 和 `DiffEngine` 也只取 `pk == 1` 的单列主键作为行身份。虽然错误消息说明这是 MVP 限制，但设计文档 §4.4 要求普通 rowid 表或 `WITHOUT ROWID` 表均可，且通过后记录每表 PK 列与冲突目标供 `UpsertExecutor`/`RowMutation.pkColumns` 复用。
- 修复建议：若短期继续限制，需在能力矩阵和规格中降级声明；若按当前设计实现，应统一引入复合 PK tuple 编码，覆盖 eligibility、selection record、diff row key、row winner pkHash、FK closure 与 upsert target。

### M-02：baseline apply 后清空 `__sync_row_winner`，未按 baseline 权威切面重建胜者

- 位置：`src/sync/baseline/BaselineManager.cpp:345`、`src/sync/apply/RowWinnerStore.cpp:82`、`src/sync/apply/ChangesetApplier.cpp:198`
- 描述：`BaselineManager::applyBaseline()` 在导入 baseline 后调用 `rw.resetAll()` 清空 row winner。规格要求 baseline 后 row winner 随 applied_vector/table_state 一并重置，且“基线即权威切面，逐行胜者以基线 origin/rank 重置”。当前实现没有为 baseline 行写入 incumbent winner；后续任意非 authoritative changeset 到达时，`ChangesetApplier` 看到 `incumbent.rank == INT_MIN` 会允许 challenger 获胜，即使该来源 rank 低于 baseline 源节点，可能覆盖刚建立的权威基线行。
- 修复建议：baseline apply 完成表数据导入后，全扫 baseline 表并为每行写入 `RowWinner{origin=baseline origin, rank=originRank, originSeq=cut/appliedSeq, contentHash, winningContent}`；或在规格中明确 baseline 后首个 changeset 可重建 winner，并接受该冲突语义变化。

## 修复优先级汇总表（如无 C/H/M 则说明本轮通过）

| 编号 | 文件 | 严重度 | 一句话描述 |
| --- | --- | --- | --- |
| H-01 | `src/sync/SyncWorker.cpp:864` | High | baseline request/response 占用 changeset origin_seq，却不推进 applied_vector，后续普通 changeset 会被误判为 gap |
| M-01 | `src/sync/schema/SchemaEligibility.cpp:80` | Medium | 实现仍拒绝复合主键，且 selection/diff 仅按单列 PK 建模，窄于同步设计规格 |
| M-02 | `src/sync/baseline/BaselineManager.cpp:345` | Medium | baseline 后清空 row winner 但不按权威切面重建，后续低 rank changeset 可能无 incumbent 地覆盖基线行 |

本轮无 Critical；上一轮显式修复均通过验证，综合评分提升至 84/100。优先修复 baseline 控制报文序列空洞后，再处理复合主键能力与 baseline row winner 重建。
