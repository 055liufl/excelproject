# SQLite 同步工具实现评审报告（第二十五轮）

评审日期：2026-06-26

## 总体评分（表格）

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 功能合规性 | 90 | 上一轮点名的空 changeset 跳号与 DoNothing no-op 表态污染已修复；同步主干、导入导出主干总体贴合规格。仍存在 selection push 半截外泄、复合主键能力与设计不一致、time-format legacy null 校验偏差。 |
| 同步协议正确性 | 85 | `origin_seq` 连续性明显改善；入站 changeset 严格连续、ACK 台账、row-winner 主路径基本成型。但 selection push 分片写入 changelog 时没有绑定 `push_id`，中心可能在整发未 done 前向下游广播部分 changeset，违反半截不外泄。 |
| Excel 导入/导出合规性 | 91 | `columnOrder`、reverse lookup、fkInject、row lookup 主干未发现新的 C/H 偏差；time-format 新 side-object null 校验正确，但 legacy `excelFormatFallback:null` 被当作缺省接受。 |
| 安全性 | 90 | SQL 标识符引用与参数绑定整体可接受；本轮未发现新的注入类 C/H/M 问题。 |
| 边界处理 | 86 | 空变更序号回滚、DoNothing no-op 跳过已补齐；多分片推送屏障、复合 PK、legacy temporal null、测试注册仍是主要薄弱点。 |
| 综合评分 | 88 | 较第二十四轮修复了 1 High + 1 Medium，但本轮仍有 1 个 High、2 个 Medium，未达到通过标准。 |

验证命令：`cmake --build build -- -j2` 通过；`ctest --test-dir build --output-on-failure` 返回 `No tests were found!!!`，说明构建产物存在但 CTest 仍未注册用例。

## 上一轮修复验证（表格）

| 编号 | 状态 | 验证结论 |
| --- | --- | --- |
| H-01 | 通过 | `SyncWorker` 在 selection push、同步导入、comparison/local capture 三条本地捕获路径中，已在 `tpl_->execute()`/`sealInto()` 成功但 `localChangelogSeq == 0` 或 `localSeq == 0` 时调用 `rollbackOriginSeq(prevSeq)`，见 `src/sync/SyncWorker.cpp:786`、`src/sync/SyncWorker.cpp:1479`、`src/sync/SyncWorker.cpp:1603`。失败路径也继续回滚，下一次真实 changelog 不再因空 changeset 跳号。 |
| M-01 | 通过 | `CapturedWriteTemplate::branchBC()` 已在预扫描后跳过 `m.mode == UpsertMode::DoNothing && ps.rowExists` 的 `TableMutation`，见 `src/sync/apply/CapturedWriteTemplate.cpp:410`、`src/sync/apply/CapturedWriteTemplate.cpp:421`。因此 `INSERT OR IGNORE` 行已存在的 no-op 不再污染 `__sync_table_state` checksum。 |

## Critical 问题（如有）

本轮未发现 Critical 级问题。

## High 问题（如有）

### H-01：SelectionPush 分片 changeset 未绑定 `push_id`，中心会在整发未完成前向下游广播半截

- 位置：`src/sync/apply/CapturedWriteTemplate.cpp:342`、`src/sync/capture/SessionRecorder.cpp:77`、`src/sync/capture/ChangelogStore.cpp:21`、`src/sync/SyncWorker.cpp:1148`、`src/sync/SyncWorker.cpp:1152`
- 描述：设计文档要求中心 “推迟向 C/D 广播本 push 的直选变更，直到 `push_progress.status=done`”，计划 T2.13 也要求 “全片 `PushChunkAck` 才 Completed、半截不外泄”。实现虽给 `__sync_changelog` 增加了 `push_id`，广播层也试图用 `entry.pushId` 查询 `__sync_push_progress` 作为屏障，但 Branch B/C 的本地捕获调用 `SessionRecorder::sealInto()` 时没有把 `p.pushId` 传入 `ChangelogStore::append()`；`append()` 本身也没有 pushId 参数，导致 selection push 产生的 changelog 行 `push_id` 为 NULL。与此同时，`processSelectionPushArtifact()` 还把分片捕获的 changeset 记为中心本地 `origin=config_.nodeId()`，广播屏障条件 `entry.origin != config_.nodeId() && !entry.pushId.isEmpty()` 双重失效。
- 影响：多分片选择推送中，中心应用 chunk 0 后会立即产生可广播 changeset。只要后台 `broadcast()` 在 chunk 1..N 到齐前运行，就会把半截状态发给其他边缘节点，违反 §5.5 “半截不外泄” 和 “下游只见全有” 的全域一致性契约。中断、超时或迁移撞推送时，下游可能已经收到并 ACK 一个本应被中心屏障拦住的部分推送。
- 修复建议：让 Branch B 的 changelog 行显式携带 `push_id`，例如扩展 `SessionRecorder::sealInto(..., pushId)` 并让 `ChangelogStore::append()` 写入该字段；广播屏障应只依据 `entry.pushId` 是否处于 `streaming`/pending，而不应排除 `entry.origin == config_.nodeId()`。补充分片回归：构造 2 个 chunk，中心只消费 chunk 0 后触发 `broadcastTopeer()`，断言没有任何该 push 的 downstream artifact；chunk 1 ACK/`push_progress=done` 后才允许广播。

## Medium 问题（如有）

### M-01：同步 eligibility、selection、diff 仍只支持单列主键，窄于当前设计/需求允许的显式非空主键能力

- 位置：`src/sync/schema/SchemaEligibility.cpp:78`、`src/sync/schema/SchemaEligibility.cpp:80`、`src/sync/selection/SelectionResolver.cpp:14`、`src/sync/selection/SelectionResolver.cpp:24`、`src/sync/diff/DiffEngine.cpp:177`、`src/sync/diff/ComparisonSession.cpp:413`
- 描述：实现现在显式拒绝复合主键并报 `E_SYNC_COMPOSITE_PK_NOT_SUPPORTED`，同时 selection/diff/comparison 仍只取 `pk == 1` 或最小 pk 序号作为单列行身份。当前需求 §FR-2 与设计 §4.4 的 eligibility 只要求普通表、显式非空 PRIMARY KEY、可用冲突目标，并未把同步表限定为单列 PK；计划 T1.0c 也写的是 “普通表 + 显式非空 PK + 可用冲突目标”。
- 影响：带复合业务主键或 `WITHOUT ROWID` 复合主键的合法 SQLite 表无法进入同步；若后续绕过 eligibility，选择推送、行级 diff、ComparisonSession 查找和 row identity 都会丢失主键的一部分。
- 修复建议：二选一收口：若 MVP 决策确为单列 PK，需同步修改需求/设计/计划并把 `E_SYNC_COMPOSITE_PK_NOT_SUPPORTED` 纳入正式错误码；若要满足当前规格，应实现统一 PK tuple 编码，贯穿 `SyncSelection`、FK closure、DiffEngine、RowWinner pkHash、TableState、UpsertExecutor 与 baseline seeding。

### M-02：legacy time-format 的 `excelFormatFallback:null` 未按规格拒绝

- 位置：`src/profile/ProfileLoader.cpp:214`、`src/profile/ProfileLoader.cpp:220`、`src/profile/ProfileLoader.cpp:221`
- 描述：`openspec/specs/time-format/spec.md` 明确要求 `format`/`fallback` 及 legacy `excelFormat`/`dbFormat`/`excelFormatFallback` 的 JSON `null` 不得被当作 empty，必须以 `E_PROFILE_PARSE` 拒绝并指出字段期望类型。新 side-object 的 `fallback:null` 已在 `parseTemporalSide()` 中拒绝；但 legacy 分支对 `excelFormatFallback` 使用 `if (!fbVal.isUndefined() && !fbVal.isNull())`，因此 `{ "datetimeFormat": { "excelFormat": "yyyy-MM-dd HH:mm:ss", "excelFormatFallback": null } }` 会被当作无 fallback 正常加载。
- 影响：profile 作者的显式 null 被静默接受，破坏 time-format 的前向兼容错误语义；老版本库加载新 schema 或错误 profile 时不会及时失败，后续导入行为与作者意图不一致。
- 修复建议：legacy 分支与 side-object 分支统一 null 处理：只要 `excelFormatFallback` 存在且为 null 就返回 `E_PROFILE_PARSE`；同样建议对 `excelFormat`/`dbFormat` 的 null 在解析阶段给出字段级错误，而不是依赖后续 “string requires non-empty format” 间接失败。补充 profile-loader 回归覆盖 legacy 三个字段的 null。

## 修复优先级汇总表（如无 C/H/M 则说明本轮通过）

| 编号 | 文件 | 严重度 | 一句话描述 |
| --- | --- | --- | --- |
| H-01 | `src/sync/capture/SessionRecorder.cpp:77` | High | SelectionPush 捕获写 changelog 时丢失 `push_id`，广播屏障失效，中心可能向下游外泄未完成 push 的半截 changeset。 |
| M-01 | `src/sync/schema/SchemaEligibility.cpp:80` | Medium | 实现拒绝复合 PK 且多处只用单列 PK，窄于当前需求/设计的显式非空主键 eligibility。 |
| M-02 | `src/profile/ProfileLoader.cpp:220` | Medium | legacy `excelFormatFallback:null` 被当作缺省接受，违反 time-format 对显式 null 的拒绝要求。 |

本轮未通过：无 Critical，但有 1 个 High、2 个 Medium。建议优先修复 SelectionPush `push_id` 贯穿 changelog 与广播屏障；否则多分片选择推送仍无法满足 “全片 ACK / 半截不外泄” 的核心同步语义。
