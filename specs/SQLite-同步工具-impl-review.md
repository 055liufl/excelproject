# SQLite 同步工具实现评审报告（第十七轮）

评审日期：2026-06-26

## 总体评分（表格）

| 维度 | 分数 | 结论 |
| --- | ---: | --- |
| 功能合规性 | 86 | 第十六轮 9 个显式修复大多落地；mixed 导入 FK preflight 仍有 route 失败回填遗漏 |
| 同步协议正确性 | 75 | ACK epoch、push_id、rawPayload 修复有效；selection push 转 changeset 的 origin/epoch/seq 建模仍有高风险偏差 |
| Excel 导入/导出合规性 | 87 | reverse lookup `error` 语义、非 route error 跳过、FK preflight 非 mixed 路径已改善 |
| 安全性 | 89 | SQL 标识符引用、绑定、内部表过滤总体可接受；未发现新的注入类 Critical |
| 边界处理 | 77 | 多 epoch 重发下界、baseline manager 封装、配置正数校验、composite PK 能力仍有残留缺口 |
| 综合评分 | 82 | 已从第十六轮 76/100 明显改善，但仍不建议在长推送/多类 mixed 导入/多 epoch 转发场景宣布完成 |

本轮审查范围：指定规格文档、`openspec/specs/`、`openspec/changes/archive/` 归档变更，以及 `src/` 下 123 个源文件；同时核对了与 `src/` 直接耦合的同步公共头文件。未执行编译或自动化测试。

## 上一轮修复验证（表格）

| 编号 | 状态 | 验证结论 |
| --- | --- | --- |
| C-01 | 主流程通过，组件残留 | `SyncWorker::processBaselineRequestArtifact()` 已把源节点本地 `(origin, streamEpoch, localOriginSeq)` 合并进 baseline cuts；但 `BaselineManager::exportBaseline()` 本身仍只读 `__sync_applied_vector`，直接调用该组件仍会缺 self cut，见 M-01。 |
| C-02 | 通过，另有残留风险 | `broadcastTopeer()` 的 per-entry ACK 查询和 payload header 已使用 `entry.streamEpoch`；但广播读取下界仍按本地 epoch 计算，见 M-02。 |
| C-03 | 通过 | `PayloadCodec::decode()` 校验 magic 后立即写入 `out->rawPayload = data`，schema mismatch quarantine 不再保存空 BLOB。 |
| H-01 | 通过 | `EntryFull` 已带 `pushId`，`readRangeAll()` SELECT `push_id`，广播屏障按 entry 自身 `push_id` 精确过滤。 |
| H-02 | 部分通过 | `ForeignKeyPreflight::check()` 已回填传入 contexts 的 `failedRouteIndices`；非 mixed 模式有效，但 mixed 模式传入的是拷贝，原始 contexts 未更新，见 H-01。 |
| H-03 | 通过 | `resolveAHeaders()` 在 `exportOnMissing:"error"` 的 NULL / NOT_FOUND 路径设置 `rowSkip=true`，不再写出缺业务键行。 |
| M-01 | 部分通过 | 已补软硬阈值关系校验和部分字段正数校验；`peerLagSoftSeq/HardSeq/SoftBytes/HardBytes/HardMs` 等仍可用非正组合绕过，见 M-03。 |
| M-03 | 通过 | payload wire version 已升为 2，`senderPeer` 仅在 `version >= 2` 时读取，旧 v1 payload 不再被误读 body。 |
| M-04 | 通过 | `RowContext::hasNonRouteError` 已接入 mapping error 检测和写入阶段整行跳过判断，混合 route-local / 非 route error 不再被误放行。 |

## Critical 问题（如有）

本轮未发现新的 Critical 级问题。

## High 问题（如有）

### H-01：mixed 模式 FK preflight 失败 route 回填到拷贝，写入阶段仍可能落库

- 位置：`src/service/ImportService.cpp:587`、`src/service/ImportService.cpp:594`、`src/service/ImportService.cpp:678`
- 描述：mixed 模式按 class 构造 `clsContexts` 时复制 `RowContext`，`ForeignKeyPreflight::check()` 只修改这份临时 vector。随后写入阶段遍历原始 `contexts`，看不到 preflight 追加的 `failedRouteIndices`。当 `abortOnError=false` 时，`failedExcelRows` 包含该行错误，但 `ctx.failedRouteIndices` 仍为空，当前逻辑会整行跳过；如果同一行已有其它 route-local failed indices，则也可能按旧集合过滤，漏掉 preflight 失败 route。两种结果都偏离 “失败 route 及 descendants 被抑制，unrelated sibling 不受影响” 的 mixed 语义。
- 规格依据：`openspec/specs/fk-injection/spec.md` 要求 row-level route 失败只级联抑制 descendants，不影响 unrelated sibling；mixed class 只是路由分组，不应改变失败粒度。
- 修复建议：mixed 模式不要复制 contexts。可按 classId 收集原始下标，构造 `QVector<RowContext*>`/下标列表，或让 `ForeignKeyPreflight::check()` 返回 `(excelRow, classId, routeIndex)` 集合后合并回原始 `contexts`。

### H-02：selection push 转发 changeset 使用“远端 origin + 本地 epoch/local seq”，会破坏 provenance 序列语义

- 位置：`src/sync/SyncWorker.cpp:762`、`src/sync/SyncWorker.cpp:763`、`src/sync/SyncWorker.cpp:764`、`src/sync/apply/CapturedWriteTemplate.cpp:325`
- 描述：源节点发 selection push 时 header `originSeq=0`；中心接收后设置 `p.origin = hdr.origin`，但 `p.epoch = streamEpoch_` 且 `p.seq = nextLocalOriginSeq()`。`CapturedWriteTemplate` 随后把该 changeset seal 为 `(origin=远端, stream_epoch=中心本地 epoch, origin_seq=中心本地序列)`。这让远端 origin 的序列空间由中心生成，且与该远端普通 changeset 的 `(origin, 原远端 epoch, 原远端 seq)` 并存。下游 `RowWinnerStore` 的同 rank 比较只看 `originSeq`，不看 epoch；同一 origin 的普通变更 seq=100 与后来的 selection push seq=1 可能被错误排序，导致中心权威推送在下游冲突裁决中输给旧变更。
- 规格依据：设计文档 §5.4/§6 要求 `origin` 不被重铸，`__sync_changelog` 唯一键为 `(origin, stream_epoch, origin_seq)`；§5.6 的 row winner 规范序使用 `(rank, seq)`，隐含同 origin seq 必须单调且可比较。
- 修复建议：为 selection push 明确定义可比较的 source sequence。优先在源节点为 push/chunk 分配源 origin seq 并写入 header/body；中心 capture 后沿用源 `(origin, stream_epoch, origin_seq)` 或为 downstream authoritative changeset 改用中心 origin/rank，并同步规格。禁止继续混用“业务 origin=远端、seq=中心本地计数器”。

## Medium 问题（如有）

### M-01：`BaselineManager::exportBaseline()` 仍不是自包含的权威 cut 导出

- 位置：`src/sync/baseline/BaselineManager.cpp:212`、`src/sync/baseline/BaselineManager.cpp:226`、`src/sync/SyncWorker.cpp:822`
- 描述：self origin cut 的补丁只存在于 `SyncWorker::processBaselineRequestArtifact()`。`BaselineManager::exportBaseline()` 仍只调用 `queryOriginCuts()` 读取 `__sync_applied_vector`，而本地源节点通常不会推进自己的 applied vector。当前主流程可用，但组件契约与类名职责不一致；后续强制 baseline、测试夹具或其它调用方直接使用 manager 时会重新缺 self cut。
- 修复建议：把 self cut 合并逻辑下沉到 `BaselineManager::exportBaseline()`，通过参数显式传入 `localOrigin/localEpoch/localOriginSeq`，或拆出 `completeOriginCuts()` 由所有 baseline export 入口共用。

### M-02：广播重发读取下界仍按本地 epoch 过滤，可能饿死较早的外源未 ACK entry

- 位置：`src/sync/SyncWorker.cpp:1105`、`src/sync/anchor/OutboundAckStore.cpp:126`
- 描述：`minUnackedLocalSeq(peer, streamEpoch_)` 的 SQL 限定 `cl.stream_epoch = ?`。如果 changelog 中存在远端 epoch 的未 ACK entry，且其 `local_seq` 小于某个本地 epoch 未 ACK entry，返回的 `afterLocalSeq` 会越过这条远端 entry，`readRangeAll(local_seq > afterLocalSeq)` 本轮不会再读到它。per-entry `ackedSeq(entry.streamEpoch)` 已修复，但读取窗口仍不是“跨 origin/epoch 的最小未确认 local_seq”。
- 修复建议：`minUnackedLocalSeq()` 不应接收单个 epoch；应 LEFT JOIN `(peer, origin, stream_epoch)` 后在全 changelog 范围内求 `MIN(cl.local_seq)-1`，再由 `shouldRoute()` 做逐 entry 过滤。

### M-03：`SyncConfig::Builder` 仍允许部分非正 lag 阈值通过

- 位置：`include/dbridge/sync/SyncConfig.h:388`、`include/dbridge/sync/SyncConfig.h:395`、`include/dbridge/sync/SyncConfig.h:402`
- 描述：软硬关系已校验，但 `peerLagSoftSeq/HardSeq/SoftBytes/HardBytes/HardMs` 未逐项正数校验。例如 soft=-10、hard=-1 满足 `soft <= hard`，会通过 build，随后 dead-peer eviction 的 lag 判断语义失真。
- 修复建议：对所有 lag seq/bytes/ms 的 soft/hard 字段先做 `>0` 校验，再校验 soft <= hard。

### M-04：同步 eligibility 仍拒绝规格允许的复合主键 / WITHOUT ROWID 能力

- 位置：`src/sync/schema/SchemaEligibility.cpp:80`
- 描述：实现仍直接拒绝 composite PRIMARY KEY，并把能力限制描述为 MVP。错误消息更清楚，但仍窄于设计文档 §4.4/§5.1 对普通 rowid / WITHOUT ROWID、明确 UPSERT target 的能力要求。
- 修复建议：若短期维持 MVP 限制，应在公开能力矩阵中明确；长期需让 `RowMutation.pkColumns`、`RowWinnerStore`、`SelectionResolver`、`UpsertExecutor` 等路径全面按复合 key tuple 建模。

### M-05：`syncSelected()` 后台受理失败未统一清理 ACK wait / pending push 状态

- 位置：`src/sync/SyncEngine.cpp:226`、`src/sync/SyncWorker.cpp:1689`、`src/sync/SyncWorker.cpp:1715`
- 描述：`syncSelected()` 在 enqueue 前调用 `startAckWait()`；worker 任务中 resolver/FK closure/chunker 失败时只 emit `errorOccurred` 并 return。`SyncEngine::onWorkerError()` 会把前台状态置 Failed 并释放 gate，但 worker 内的 `ackWaiting_`、`pendingPushId_` 等状态并没有统一通过 `cancelAckWait()` 清理，后续仍可能触发一次迟到 `E_SYNC_ACK_TIMEOUT` 或影响 stale chunk ACK 判断。
- 修复建议：为 selection push 任务增加单一失败出口：清空 `pendingPushId_`、`pendingAckWindow_`、`ackWaiting_` 和 deadline，再 emit 业务错误。

## 修复优先级汇总表

| 编号 | 文件 | 严重度 | 一句话描述 |
| --- | --- | --- | --- |
| H-01 | `src/service/ImportService.cpp:587` | High | mixed FK preflight 修改的是 RowContext 拷贝，route 失败粒度未回写原始 contexts |
| H-02 | `src/sync/SyncWorker.cpp:762` | High | selection push 转 changeset 混用远端 origin 与中心本地 epoch/seq，破坏同源序列比较 |
| M-01 | `src/sync/baseline/BaselineManager.cpp:226` | Medium | self origin cut 补丁在 SyncWorker 外层，BaselineManager 组件自身仍导出不完整 cuts |
| M-02 | `src/sync/anchor/OutboundAckStore.cpp:126` | Medium | broadcast 重发下界按单个本地 epoch 过滤，跨 epoch 未 ACK entry 可能被窗口越过 |
| M-03 | `include/dbridge/sync/SyncConfig.h:388` | Medium | lag soft/hard 只校验关系，未逐项拒绝非正值 |
| M-04 | `src/sync/schema/SchemaEligibility.cpp:80` | Medium | composite PK / WITHOUT ROWID 能力仍窄于同步设计规格 |
| M-05 | `src/sync/SyncWorker.cpp:1689` | Medium | syncSelected 后台失败没有统一清理 ACK wait 与 pending push 状态 |

本轮无新的 Critical；第十六轮阻断项大多已收口，综合评分提升至 82/100。剩余 High 主要影响 mixed 导入数据正确性和 selection push 下游收敛，建议优先修复后再进入下一轮验收。
