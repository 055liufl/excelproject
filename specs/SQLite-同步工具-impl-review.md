# SQLite 同步工具实现评审报告（第二十八轮）

评审日期：2026-06-26

评审范围：按当前工作区实际文件计，已覆盖 `src/` 下 71 个 `.cpp/.h` 源文件，并对照指定 `specs/`、`openspec/specs/` 与 `openspec/changes/archive/` 文档复核。

## 总体评分（表格，各维度 0-100）

| 维度 | 评分 | 结论 |
|---|---:|---|
| 同步线程/连接/事务模型 | 90 | `SyncWorker` 独立写连接、`WriteTxn`、入站 changeset 三件套事务、ACK 等待总体闭合。 |
| 入站 changeset / ACK / baseline | 86 | 严格连续位点、原 blob 转发、typed ACK 和 baseline fallback 基本实现；传输异常分类仍有偏差。 |
| 选择性推送链路 | 84 | 分片、幂等和全片 ACK 完成判定已实现；制品命名仍偏离稳定契约。 |
| 批量导入 / lookup / fkInject | 82 | lookup、复合 fkInject、preflight 主路径已实现；Mapper 错误隔离存在 High 问题。 |
| 导出列顺序 / 反向 lookup / 时间格式 | 84 | 输出层重排、反向 lookup、`epochSec`/NULL/0 边界主路径基本正确；导入方向失败语义有偏差。 |
| Schema / 安全边界 | 78 | SQL identifier quoting 较完整；partial UNIQUE / 表达式 UNIQUE 冲突目标识别不足。 |
| 综合评分 | 84 | 未发现 Critical；2 个 High 需要先修复，Medium 可并入同一轮整改。 |

## Critical 问题（必须修复）

无。

## High 问题

### H-01：时间格式/Mapper 行级错误会跳过整行所有 route，违反“只跳过声明 route，兄弟 route 继续”

位置：`src/mapping/Mapper.cpp:58`、`src/mapping/Mapper.cpp:94`、`src/service/ImportService.cpp:550`、`src/service/ImportService.cpp:694`

描述：`Mapper::map()` 只用 route 内局部 `rowHasError` 记录 validator/temporal 错误，但没有把“哪个 route 失败”返回给 `ImportService`。`ImportService` 通过 `errors.list().size()` 判断 Mapper 是否新增错误，并把 `ctx.hasNonRouteError = true`。写入阶段只要该 Excel 行有 row-level error 且 `hasNonRouteError` 为 true，就跳过整行，导致同一行中不相关的 sibling route 也不会写入。

规格依据：`openspec/specs/time-format/spec.md:312` 要求 `E_TIME_PARSE` 仅使失败列所在 route 失败；`openspec/specs/time-format/spec.md:475` 要求“row SHALL be skipped for the declaring route”；`openspec/specs/fk-injection/spec.md:100` 要求不相关 sibling route 继续处理。

修复建议：让 `Mapper::map()` 返回或填充 route 级失败集合，例如在 `RowContext` 中记录 `failedRouteIndices`；时间解析/validator 失败时插入当前 route index，而不是设置 `hasNonRouteError`。`hasNonRouteError` 只保留给缺列、结构不可恢复等真正整行不可用的错误。写入阶段沿用现有 descendant fail closure 跳过失败 route 及其子孙。

### H-02：partial UNIQUE 索引会被误判为合法 UPSERT 冲突目标

位置：`src/schema/SchemaIntrospector.cpp:93`、`src/schema/SchemaIntrospector.cpp:101`、`src/profile/ProfileValidator.cpp:26`

描述：`SchemaIntrospector::readIndexes()` 读取 `PRAGMA index_list` 时丢弃了 `partial` 字段，也未用 `PRAGMA index_xinfo` 区分表达式索引。`ProfileValidator::isConflictValid()` 只要发现 `idx.unique && idx.columns == conflict.columns` 就接受。结果是仅存在 `CREATE UNIQUE INDEX ... WHERE ...` 的表会通过 profile 校验，但生成的 `ON CONFLICT(col) DO UPDATE` 在 SQLite 中不是合法完整冲突目标，导入运行期才失败。

规格依据：`specs/SQLite-同步工具-设计文档.md:420` 明确要求 UPSERT `ON CONFLICT` 目标必须是完整唯一索引，partial / 表达式唯一索引不得作冲突目标；`specs/Qt-SQLite-Excel-批量导入导出-设计文档.md:1112` 也把 partial / 表达式索引解析列为元数据正确性要求。

修复建议：扩展 `IndexInfo`，记录 `partial`、`origin`、是否包含表达式列；`readIndexes()` 读取 `PRAGMA index_list` 的 partial 列，并用 `PRAGMA index_xinfo` 排除 `cid < 0` 的表达式项。`isConflictValid()` 只接受 PK 或 `unique && !partial && !hasExpression && 完整列集匹配` 的索引。

## Medium 问题

### M-01：损坏 ACK 制品不会标记 `corrupt`，会留在 `seen` 状态反复处理

位置：`src/sync/SyncWorker.cpp:496`、`src/sync/SyncWorker.cpp:967`

描述：普通 payload 解码失败会 `markCorrupt` 并移入 quarantine；但 `.ack` 分支只调用 `processAckArtifact()`。当 ACK 既不是 `ChangesetAck` 也不是 `PushChunkAck` 时，函数返回 `false`，ledger 保持 `seen`，后续周期扫描会继续处理同一坏 ACK，甚至进入 stale pending 逻辑。

规格依据：`specs/SQLite-同步工具-设计文档.md:755` 定义 ledger 状态包含 `corrupt`；`specs/SQLite-同步工具-设计文档.md:758` 要求 `PayloadCodec.decode` 失败后标 `corrupt`、报 `E_SYNC_PAYLOAD_CORRUPT`，不重复尝试。

修复建议：ACK 解码失败时同样执行 `ledger_->markCorrupt()`，发出 `E_SYNC_PAYLOAD_CORRUPT`，并按 `quarantineDir` 规则移动/删除坏 ACK 及 `.ready`。

### M-02：`.ready` 存在但主文件缺失的超时被归类为 `E_SYNC_GAP`

位置：`src/sync/transport/InboxWatcher.cpp:41`、`src/sync/transport/InboxWatcher.cpp:50`、`src/sync/SyncWorker.cpp:467`、`src/sync/SyncWorker.cpp:479`、`src/sync/SyncWorker.cpp:513`

描述：`InboxWatcher` 在看到 `.ready` 后会先 `markSeen`，即使主 payload 文件尚不存在也会留下 `seen`。之后 `scanInbox()` 把所有 `pendingSeen` 当作业务 pending 处理；主文件打不开时只返回 `false`，超时后统一报 `E_SYNC_GAP` 并尝试 baseline fallback。缺主文件是 transport 半发布/搬运失败，不是 changeset sequence gap。

规格依据：`specs/SQLite-同步工具-设计文档.md:757` 要求“缺主文件 / 哨兵先到”超时后报 `E_SYNC_TRANSPORT` 告警，不阻塞其它制品。

修复建议：ledger 增加或复用可区分的缺主文件状态/错误路径；`processArtifact()` 打不开主文件时记录 transport pending，超过阈值发 `E_SYNC_TRANSPORT`，不要调用 `runBaselineFallbackFor()`。

### M-03：同步制品命名不符合阶段 0 锁定的稳定契约

位置：`src/sync/SyncDDL.h:169`、`src/sync/SyncDDL.h:183`

描述：当前 changeset 名称为 `origin__epoch__seq__peer__changeset.payload`，selection push 名称为 `pushId__chunkSeq__peer__selectionpush.payload`，没有规格要求的 `kind` 固定位置和 UUID 后缀。虽然加入 `peer` 能避免多对端覆盖，但这已经偏离第三方搬运工具依赖的命名契约。

规格依据：`specs/SQLite-同步工具-设计文档.md:749` 规定命名为 `<origin>__<stream_epoch>__<kind>__<seq|push_id.chunk_seq>__<uuid>.payload`，并说明这是与第三方的稳定契约、阶段 0 锁定项。

修复建议：将目标 peer 纳入规范字段或 manifest 元数据，但保留规格指定的字段顺序与 UUID 后缀。例如 `origin__epoch__changeset__seq__peer__uuid.payload`，并同步更新 watcher/ledger/测试和第三方搬运约定。

## 修复优先级汇总表

| 优先级 | 问题 | 建议动作 |
|---|---|---|
| P0 | H-01 Mapper 错误隔离 | 先修，避免合法 sibling route 数据被错误跳过。 |
| P0 | H-02 partial UNIQUE 冲突目标 | 先修，避免 profile 校验通过但运行期 UPSERT 失败。 |
| P1 | M-01 损坏 ACK 反复处理 | 与 transport ledger 整改一起修。 |
| P1 | M-02 缺主文件误报 gap | 修正错误分类，避免错误触发 baseline fallback。 |
| P2 | M-03 制品命名契约偏差 | 与搬运工具/兼容策略一起调整。 |

本轮结论：无 Critical，但存在 High 阻断项；建议修复 H-01/H-02 后再进入下一轮通过判定。
