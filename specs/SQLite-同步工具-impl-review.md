# 代码审查报告（第三十九轮）

## 总览

审查范围：`src/` 全部源代码，对照两份 Qt/SQLite/Excel 导入导出设计、两份 SQLite 同步工具设计/计划，以及 `export-column-order`、`export-reverse-lookup`、`fk-injection`、`row-lookup`、`time-format` 五份 OpenSpec。

本轮重点验证第三十八轮修复是否全部正确落地：

- H-01（ACK 崩溃补偿）：`SyncWorker.cpp:651-663` — `scheduleChangesetAck` + `flush` 确认在 `markConsumed` 之前调用，正确。
- H-02（push_progress insert 错误传播）：`SyncWorker.cpp:2031-2038` — `ins.exec()` 失败时 `cancelAckWait()` + 上报错误 + 立即返回，正确。
- H-03（stalePending 误报 E_SYNC_GAP）：`SyncWorker.cpp:537-555` — stale 列表先过滤 `__changeset__` 后才触发 baseline fallback，正确。
- H-04（ChangesetApplier 静态列名缓存）：`ChangesetApplier.cpp:331-335` — `uwColCache` 定义在 `while` 循环之外且非 `static`，正确。
- M-01（CapturedWriteTemplate existQ.exec() 静默跳过）：`CapturedWriteTemplate.cpp:296-302` — exec() 失败时 `rec_.abort(); txn.rollback(); return result;`，正确。
- M-02（Mixed 导出二次字符串排序）：`ExportService.cpp:726-730` — `std::stable_sort` 调用已移除，文件中无匹配，正确。
- M-03（OutboxWriter POSIX-only）：`OutboxWriter.cpp:9-12,61-71,106-112,119-130` — POSIX 头文件及 `::fsync`/`::open`/`::close` 均在 `#ifdef Q_OS_UNIX` 块内，正确。

问题统计：
- Critical：0
- High：0
- Medium：0
- Low：1

## Critical

无。

## High

无。

## Medium

无。

## Low

### L-01：`ConsistencyCache::init()` 失败静默降级，没有任何可观测告警

- 规范依据：`specs/SQLite-同步工具-设计文档.md` §5.5/§8.2 要求 selection push 的一致性缓存可观测，错误进入 error/log 环。
- 实现证据：`src/sync/SyncWorker.cpp:339-343` 调用 `consistencyCache_.init()` 失败时仅有注释 "Non-fatal"，没有 `emit errorOccurred` 或日志输出。
- 影响说明：缓存表损坏或迁移缺列时，依赖剪枝会悄悄退化为全量依赖推送，用户只能看到性能/载荷异常，难以定位根因。
- 最小修复建议：保留非阻断语义，但发出 `W_SYNC_UNTRACKED_CHANGE` 或专用 warning，并带上 `cacheErr`。

## 总结

本轮建议可发布。第三十八轮所有 High（H-01～H-04）和 Medium（M-01～M-03）问题均已正确修复，构建通过，17 个测试全部通过。仅剩 L-01 一项 Low 优先级问题（ConsistencyCache 静默降级告警缺失），不影响功能正确性，可在后续维护迭代中补充。
