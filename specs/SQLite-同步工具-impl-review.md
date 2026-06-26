# SQLite 同步工具实现评审报告（第二十九轮）

评审日期：2026-06-26

评审范围：静态覆盖当前工作区 `src/` 下 123 个源文件，并对照指定 `specs/`、`openspec/specs/` 与 `openspec/changes/archive/` 文档复核。尝试执行 `ctest --test-dir build --output-on-failure`，但 CTest 未注册测试；直接运行 `build/tests/tst_auto_profile_builder` 因 Qt 符号 `_Z13qgpu_featuresRK7QString` 缺失失败，故本轮测试结论以静态审查为主。

## 总体评分（表格）

| 维度 | 评分 | 结论 |
|---|---:|---|
| 上一轮修复闭合度 | 76 | H-02/M-01/M-02 基本闭合；H-01 实际控制流仍未闭合；M-03 仅 changeset 命名闭合。 |
| 入站 changeset / 严格连续 / ACK | 88 | Branch A 原 blob 转发、applied vector、ACK 解码损坏处理、缺主文件错误分类主路径正确。 |
| selection push / 上行链路 | 76 | 分片、schema guard、chunk 幂等有实现；但中心捕获后把业务 origin 重铸为中心 origin，偏离设计核心语义。 |
| 批量导入 / lookup / fkInject / 时间格式 | 80 | 多数 OpenSpec 主路径存在；Mapper route-local 失败集合被覆盖，导致 sibling route 语义仍错。 |
| schema / profile / 公共边界 | 83 | partial UNIQUE 已拒绝；同步激活后旧 `importExcel` 拒绝范围过宽。 |
| 传输制品契约 / 可验证性 | 75 | changeset 命名已改；selection push 命名仍旧格式；当前测试二进制不可执行。 |
| 综合评分 | 81 | 无 Critical；2 个 High 阻断通过判定，Medium 需并入下一轮修复。 |

## 上一轮修复验证（表格）

| 上轮编号 | 上轮问题 | 本轮验证结论 | 证据 | 判定 |
|---|---|---|---|---|
| H-01 | Mapper payload.hasError → failedRouteIndices，时间/validator 失败不再跳整行 | 未通过。Mapper 确实设置了 `payload.hasError`，`ImportService` 也短暂写入 `ctx.failedRouteIndices`，但随后被 `FkInjector::inject(...)` 返回值整体覆盖，且没有把 Mapper 失败集合传入。 | `src/mapping/Mapper.cpp:72`、`src/service/ImportService.cpp:555`、`src/service/ImportService.cpp:567`、`src/mapping/FkInjector.cpp:66` | 失败 |
| H-02 | SchemaIntrospector 读取 partial 字段，ProfileValidator 拒绝 partial UNIQUE 索引 | partial UNIQUE 主问题已修。`IndexInfo.partial` 已持久化，`ProfileValidator` 对 `idx.partial` 执行跳过。表达式索引仍未显式建模，但本轮未确认会被误接收为合法冲突列。 | `src/schema/SchemaCatalog.h:19`、`src/schema/SchemaIntrospector.cpp:113`、`src/profile/ProfileValidator.cpp:220` | 通过 |
| M-01 | ACK 解码失败时 markCorrupt + E_SYNC_PAYLOAD_CORRUPT | 已修。`.ack` 进入 ledger，解析失败后 `markCorrupt` 并发 `E_SYNC_PAYLOAD_CORRUPT`。 | `src/sync/SyncWorker.cpp:496`、`src/sync/SyncWorker.cpp:506` | 通过 |
| M-02 | 主文件打不开时发 E_SYNC_TRANSPORT，不触发 baseline fallback | 已修。主 payload 打不开时标 `corrupt` 并发 `E_SYNC_TRANSPORT`，不会留在 `seen` 触发 `stalePending()` 的 baseline fallback。 | `src/sync/SyncWorker.cpp:520`、`src/sync/SyncWorker.cpp:526` | 通过 |
| M-03 | 制品命名改为规格格式 `origin__epoch__changeset__seq__[peer-]uuid.payload` | 部分通过。changeset helper 已符合字段顺序和 UUID 后缀；selection push helper 仍是 `pushId__chunkSeq__[peer]__selectionpush.payload`，缺少 origin/epoch/kind 固定位置和 UUID。 | `src/sync/SyncDDL.h:174`、`src/sync/SyncDDL.h:191`、`src/sync/SyncWorker.cpp:1884` | 部分通过 |

## Critical 问题（如有）

无。

## High 问题（如有）

### H-01：Mapper route-local 失败集合被 FK 注入阶段覆盖，上一轮 H-01 未真正修复

位置：`src/service/ImportService.cpp:555`、`src/service/ImportService.cpp:567`、`src/mapping/FkInjector.cpp:66`

描述：Mapper 在 validator/temporal 失败时设置 `payload.hasError`，`ImportService` 随即把这些 payload index 写入 `ctx.failedRouteIndices`。但后续调用：

```cpp
ctx.failedRouteIndices = fkInjector.inject(..., std::move(lookupFailed));
```

只把 `lookupFailed` 作为 `initialFailed` 传入，之前从 `payload.hasError` 得到的失败集合被丢弃。结果是：仅有时间/validator 错误时，写入阶段仍会因为 `failedRouteIndices.isEmpty()` 跳过整行；若同一行同时有 lookup/fkInject 失败，Mapper 失败 route 反而可能不在 skip 集合中。

规格依据：`openspec/specs/time-format/spec.md` 要求 `E_TIME_PARSE` 只跳过声明 route；`openspec/specs/fk-injection/spec.md` 要求不相关 sibling route 继续处理。

修复建议：在调用 `FkInjector::inject()` 前合并失败集合，例如 `initialFailed = ctx.failedRouteIndices | lookupFailed`，并用返回值覆盖；同时补一个多 route 用例：route A 时间解析失败、sibling route B 成功写入、A 的 child 被级联跳过。

### H-02：中心处理 selection push 后把业务 origin 重铸为中心节点，违反上行推送 origin 元数据契约

位置：`src/sync/SyncWorker.cpp:784`、`src/sync/SyncWorker.cpp:790`、`src/sync/apply/CapturedWriteTemplate.cpp:335`、`src/sync/capture/ChangelogStore.cpp:146`

描述：中心收到 edge 的 selection push 后，`processSelectionPushArtifact()` 构造 Branch B 参数时写死：

```cpp
p.origin = config_.nodeId();
p.epoch = streamEpoch_;
p.seq = nextLocalOriginSeq();
```

随后 `CapturedWriteTemplate::branchBC()` 把 `p.origin` 作为 changelog origin 写入 `SessionRecorder::sealInto()`，最终 `__sync_changelog.origin` 变成中心节点。设计文档明确要求上行 UPSERT 推送经中心 fresh 捕获后，changelog 元数据仍记录发起方 `origin=B`，这样下行 changeset 能表达真实业务来源；当前实现把业务来源改成中心，破坏审计、防回声和后续冲突语义。

规格依据：`specs/SQLite-同步工具-设计文档.md:586` 要求 selectionpush 分支产物为“session 捕获的 changeset + origin=B 元数据”；`specs/SQLite-同步工具-设计文档.md:689` 明确“changelog 元数据列记 origin=B”。

修复建议：把“业务 origin/epoch”和“中心本地 changelog local_seq/发送顺序”分开建模。Branch B 写 changelog 时保留 `hdr.origin` 作为业务 origin，不要用 `config_.nodeId()` 替代；若需要中心本地连续编号，用 `local_seq` 或独立 source sequence 字段承载，不要混入业务 origin 命名空间。

## Medium 问题（如有）

### M-01：selection push 制品命名仍不符合稳定传输契约

位置：`src/sync/SyncDDL.h:191`、`src/sync/SyncWorker.cpp:1884`

描述：上一轮命名整改只覆盖 changeset。selection push 仍生成 `pushId__chunkSeq__[peer]__selectionpush.payload`，缺少规格要求的 `<origin>__<stream_epoch>__<kind>__<push_id.chunk_seq>__<uuid>.payload` 字段顺序和 UUID 后缀。第三方搬运工具若按阶段 0 锁定契约路由/排查制品，会无法用统一规则处理 selection push。

规格依据：`specs/SQLite-同步工具-设计文档.md:749`。

修复建议：把 `selectionPushArtifactName()` 改为接收 `origin, epoch, pushId, chunkSeq, targetPeer`，输出同一稳定格式，并补命名单测覆盖 changeset、selectionpush、baseline/ACK 的兼容策略。

### M-02：同步激活后旧 `DataBridge::importExcel()` 无条件拒绝，误伤非同步表导入

位置：`src/DataBridge.cpp:183`、`src/DataBridge.cpp:186`

描述：当前只要 `syncActive_` 为 true，`importExcel()` 直接返回 `E_SYNC_WRITE_BLOCKED`，没有加载 profile 判断本次导入是否写入同步表。设计要求同步模式下旧 API 对同步表写入被拒绝，但非同步表不受限，以保持既有 ETL 兼容。

规格依据：`specs/SQLite-同步工具-设计文档.md:205`、`specs/SQLite-同步工具-设计文档.md:208`。

修复建议：在拒绝前解析 `options.profileName` 对应 routes，与当前 `canonicalSyncTables` 求交；只有命中同步表才返回 `E_SYNC_WRITE_BLOCKED`。非同步表仍可走主线程 `db_`，或在后续 M3 按计划统一改道写队列。

### M-03：现有测试入口不可执行，无法为修复提供自动化回归信号

位置：`build/tests/*`、`tests/*`

描述：`ctest --test-dir build --output-on-failure` 返回“No tests were found”；直接运行 `build/tests/tst_auto_profile_builder` 立即因 Qt 符号 `_Z13qgpu_featuresRK7QString` 缺失退出。并且当前 `tests/unit/tst_temporal_import.cpp` 主要覆盖单 route 行跳过，没有覆盖 H-01 所需的 sibling route 继续写入场景。

影响：上一轮 H-01 这类控制流回归无法被测试捕获；后续修复即使完成，也缺少可重复验证证据。

修复建议：修复测试运行环境/CTest 注册；增加多 route temporal/validator 失败、sibling route 成功、child route 级联跳过的单元或集成测试。

## 修复优先级汇总表（如无 C/H/M 则说明本轮通过）

| 优先级 | 问题 | 建议动作 |
|---|---|---|
| P0 | H-01 Mapper route-local 失败集合被覆盖 | 先修集合合并和 sibling route 回归测试，避免错误跳整行或写入坏 route。 |
| P0 | H-02 selection push origin 被中心重铸 | 重建 Branch B origin 元数据模型，保留发起方 origin。 |
| P1 | M-01 selection push 命名契约偏差 | 与 changeset 使用同一稳定命名格式，补制品命名单测。 |
| P1 | M-02 `importExcel()` 拒绝范围过宽 | 只拒绝命中同步表的旧 API 写入。 |
| P2 | M-03 测试入口不可执行/覆盖不足 | 修复 CTest/Qt 运行环境并补 route-local 回归测试。 |

本轮结论：未发现 Critical，但存在 2 个 High；第二十九轮不通过，建议先修 P0 后再复评。
