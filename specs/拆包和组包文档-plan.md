# 拆包和组包 实现计划（最小可落地）

> 依据：`specs/拆包和组包设计文档.md`（WHAT）；本文件是 HOW —— 落地顺序 + 每步验证 + 风险。
> 核心原则：**每个切片独立可编译、可验证、可回滚**；把风险最高的「新 ARQ 代码路径」**先在安全配置（默认 60000B / 单片 / 近无损）下验证通过**，再切 800B 激活多分片——**把「新代码路径对不对」与「多分片对不对」两个风险解耦**。

---

## 0. 关系、影响面与切片总览

- **影响面（很小、可控）**：只改 demo 传输层 `examples/sync-demo/udp_transport.{h,cpp}`（+ 新增 `udp_framing.{h,cpp}`）、10 个构造点、新增 1 个单测；**不触碰 dbridge 库**。
- **一个源、两处编译**：`udp_transport.cpp` 被 **sync-demo** 与 **sync-suite** 各自编译进自己的二进制。改一次 → **两 demo 同时受影响**，两处都要验。
- **文件组织决策（对设计 §9 的小幅细化，理由：设计 §5.1 要求「脱离 socket 单测」）**：把纯逻辑 `FragmentResult` / `fragmentMessage` / `FragmentReassembler` 放进**新文件 `examples/sync-demo/udp_framing.{h,cpp}`（无 Qt Network、无 QObject）**，由 `udp_transport.cpp` `#include`。这样单测只编译 `udp_framing.cpp`（纯、无 moc、无 network），干净地实现「脱离 socket 单测」；`udp_transport.{h,cpp}` 只保留 socket/ARQ 编排。
  - 代价：`udp_framing.{cpp,h}` 需加入 4 个 demo 构建文件（见 §3）。
  - 备选（若不想加新文件）：全部留在 `udp_transport.{h,cpp}`，则单测须编译 `udp_transport.cpp` + 头（AUTOMOC 处理 `UdpFileTransport` 的 Q_OBJECT）+ `QT += network`——测试更重且脆，故**不采用**。

| 步 | 内容 | landable（本步完成后「什么是working的」） |
|----|------|------|
| 0 | 基线确认 | 两 demo 在默认 60000 下 qmake+cmake 均构建+跑通（回归对照锚点） |
| 1 | `udp_framing`：`fragmentMessage` + 拆包单测 | 新文件编译进两 demo；拆包单测绿；**两 demo 行为不变**（未接入） |
| 2 | `udp_framing`：`FragmentReassembler` + 组包单测 | **接收端**组包健壮性(校验/淘汰/去重/重组/durable 解耦)单测绿；**两 demo 行为不变**（发送端 RTO/giveUp 属 run()，见 Step 4/5） |
| 3 | 接口 `setMaxTransmitBytes` + 常量/成员（未接入 run） | 编译通过；接口存在但 run() 仍旧路径 |
| 4 | `run()` 集成 ARQ + 交付状态机（**默认 60000**） | 两 demo 走**新 ARQ 路径**（ACK/重传/durable/淘汰），60000/单片下 selftest 绿 |
| 5 | 10 构造点设 `setMaxTransmitBytes(800)` | 两 demo 在 **800B/多分片** 下 selftest 绿；**日志断言** `fragCount>1 且每 datagram≤800`、终态 `.sent` |
| 6 | 双构建 + 全量验证 + 观测 | qmake+cmake 全绿；单测 + 两 demo(800) 全通过；重传/分片计数可观测 |

---

## 1. Step 0 · 基线确认（不改代码）

先记录「改造前」两 demo 都能构建+跑通，作为每步回归对照。

- qmake：`cd build_qmake_demos && qmake ../dbridge.pro`，构建 `sync-demo`、`sync-suite`，各跑一次（命令见 §7）。
- cmake：`cmake -S . -B build && cmake --build build --target sync-demo sync-suite`，各跑一次。
- 记录：sync-suite `--selftest` 退出码 0；sync-demo 输出末尾「完成」且无 `[WARNING] … 存在错误记录`、收敛字段一致。

**landable**：无改动，仅确立锚点。**回滚**：无。

---

## 2. 切片实现

> 每步统一格式：目标 / 改动 / 关键实现点（引用设计文档章节，不重复其内容）/ 构建 / 验证 / landable / 回滚。

### Step 1 · 拆包纯函数 `fragmentMessage` + 单测

- **目标**：产出「文件 → N 个 ≤ maxBytes 的 DATA 数据报」的纯函数，可单测。
- **改动**：
  - 新增 `examples/sync-demo/udp_framing.h`：`FragmentResult`、`fragmentMessage(...)` 声明、`type` 常量（DATA=0x01/ACK=0x02）、头长常量（18）、`magic`（沿用现值 `0xDB5ACED0`）。
  - 新增 `examples/sync-demo/udp_framing.cpp`：实现 `fragmentMessage`。守卫（任一 → `ok=false`）：`fname>255`、`fragCount>65535`、**`M = maxBytes−18−fname_len < 1`（纯函数自守，不只靠 `setMaxTransmitBytes(<274)` 拦截）**；`data>UINT32_MAX` 为**防御性**分支（Qt5 `QByteArray::size()` 为 `int`，实际不可构造 → 不单测，仅代码保留）。空文件 1 片；大端 `QDataStream`。
  - 4 个 demo 构建文件登记 `udp_framing.{cpp,h}`（见 §3）——本步只是**编译进来**，`udp_transport.cpp` 尚未使用。
  - 新增单测骨架 `tests/unit/tst_udp_reassembly.{cpp,pro}` + 登记（见 §3），先写 `frag_sizes`、`frag_errors` 两个用例（设计 §6.1）。
- **关键点**：`fragmentMessage` 无副作用、不碰 socket；与接收端头字段严格对应（设计 §4.2）。`frag_errors` 只测**可达**分支——`fname>255`、`M<1`（传极小 `maxBytes`）、`fragCount>65535`（用极小 `M` + 约 64KB 数据即 > 65535 片，内存可控）；`data>UINT32_MAX` 标注不可实测。
- **构建**：qmake 重跑 + 建 `tst_udp_reassembly` + 建两 demo（确认新文件不破坏 demo 编译）。
- **验证**：`tst_udp_reassembly` 中 `frag_sizes`/`frag_errors` 绿；两 demo 仍按 Step 0 跑通（行为不变）。
- **landable**：拆包函数可用、可测；demo 无行为变化。**回滚**：删新文件 + 撤 4 处登记。

### Step 2 · 组包状态机 `FragmentReassembler` + 单测

- **目标**：不依赖 socket 的重组状态机，承载**组包健壮性全部逻辑**（本任务核心）。
- **改动**：
  - `udp_framing.h/.cpp` 增 `FragmentReassembler`（设计 §5.1②）：`feed()`（含设计 §4.4 全部校验 + 越界/元数据/总长/**文件名安全**；组齐→`Completed` 且**不进短表**）、`markDelivered()`（外层 durable 落盘成功后才入短表）、`evictStale()`、`pendingCount()`；构造参数 `(reassemblyTimeoutMs, completedRetentionMs)` 供单测注入短超时（设计 §4.7）。
  - `tst_udp_reassembly.cpp` 补齐用例（设计 §6.1）：`roundtrip_inorder/reorder`、`retransmit_recovery`、`durable_delivery`、`eviction`、`collision`、`dup_after_complete`、`reject_out_of_range/meta_mismatch/size_mismatch/unsafe_filename`。丢包=「不喂某些片」；淘汰=注入小超时 + 手推时间（`feed(..., nowMs)` 的 `nowMs` 由测试给定，天然可控，无需真实等待）。
- **关键点**：`feed→Completed` 与 `markDelivered` 的**解耦**是幂等交付的关键（设计 §4.3、原则 5）；单测里可显式「Completed 后不 markDelivered → 再喂整条 → 仍 Completed」验证落盘失败可重投。
- **构建/验证**：单测全绿（尤其 `retransmit_recovery/durable_delivery/eviction/collision/reject_*`）；两 demo 行为仍不变。
- **landable**：**接收端**组包健壮性（§4.4 校验、pending 淘汰、去重、重组、durable 交付解耦）**完全可单测、已验证**。注意：发送端 `RTO/giveUp`（缺口①的重传侧）在 `run()` 里、**不**由本步单测覆盖（见 Step 4/5 的说明）。**回滚**：撤 `udp_framing` 的 reassembler 部分 + 相应用例。

### Step 3 · 对外接口 `setMaxTransmitBytes` + 常量/成员（尚未接入 run）

- **目标**：加接口与 ARQ 所需常量/成员，但 `run()` 暂不使用（保持可编译、行为不变）。
- **改动**：`udp_transport.h`：
  - `bool setMaxTransmitBytes(int)`（设计 §4.7：`<274` 拒绝返回 false；默认 60000）+ 成员 `maxTransmitBytes_`。
  - 可靠性**编译期常量**：`RTO=500 / maxRetries=5 / reassemblyTimeout=8000 / completedRetention=8000 / pollMs=50`（设计 §4.6）；**不提供** `setReliabilityParams` 公开接口。
  - `quint32 msgSeq_`，构造时 `QRandomGenerator::global()->generate()` 随机起点（设计 §5.1③、原则 7）；`#include "udp_framing.h"`。
- **构建/验证**：编译通过；两 demo 行为不变（run 仍走旧 `sendFile/handleDatagram`）。
- **landable**：接口就位。**回滚**：撤接口/成员。

### Step 4 · `run()` 集成 ARQ + 交付状态机（**默认 60000，先验证新路径**）

- **目标**：把 `run()`/`pollOutbox` 切到 `fragmentMessage` + `FragmentReassembler` + ARQ；**先不改 800B**，用默认 60000（单片、近无损）**隔离验证「新代码路径本身对不对」**。
- **改动**：`udp_transport.cpp` 按设计 §5.1④ 重写 `run()`、按 §5.2 实现交付状态机：
  - **⚠️ 通用 artifact（对设计伪代码 `payload/readPayload` 措辞的落地澄清，务必照此实现）**：`pollOutbox` 扫的是**通用** `*.ready`，其主文件既可能是 `.payload`（changeset/基线/快照）**也可能是同步协议的 `.ack` 文件**。实现**必须**用 `artifactName = readyName 去掉 ".ready"` 统一处理二者——**不得只按 `.payload` 后缀**，否则 `.ack` 工件不发、demo 会卡住。终态改名同时作用于「主文件」与其 `.ready.sending`。
  - 发送：原子认领（`rename(name.ready → name.ready.sending)`）→ **主文件** `rename(artifact → artifact.sending)`（发送前）→ 读取该文件（**用局部 helper struct `{QByteArray data; bool ok;}` 或 `QFile+bool`——仓库无通用 `Result<T>`，勿臆造**）→ `fragmentMessage` → 发送 → 入 `outbound`（顺序与失败处理严格照 §4.3 首发 1–5 与 §5.1 伪代码；1–4 任一失败 → 主文件与 `ready.sending` 均 `.failed` + 告警、不进 outbound）。
  - 接收：`feed` → `Completed` 则「写 inbox(主文件)+`.ready` 成功 → `markDelivered`+回 ACK；失败 → 不 ACK+告警」；`NeedAck` → 回 ACK。
  - 维护：`RTO` 兜底全量重传（`retries<maxRetries`）/ 超限 `.failed`；`evictStale`。
  - ACK 分派：先 §4.2 ACK 校验（精确 9 字节）+ 端点校验（`==outbound[msgId].dest`）→ `.sending→.sent`、删 outbound。
  - **删除** `sentFiles_`（磁盘后缀为唯一出站真源，设计 §5.2）；删除旧 `sendFile/handleDatagram`。
  - **观测（本步必做，验证多分片的唯一可信来源）**：每条消息发送时记日志/计数 `{msgId, artifactName, fragCount, 最大 datagram 字节, retries}`；接收/重传/giveUp 亦计数。文件系统**看不到** wire 层 `fragCount`，故多分片只能由此日志证明。
- **关键点/陷阱**：
  - `.ready.sending` **不得**被 `*.ready` 扫描命中（glob `*.ready` 只匹配以 `.ready` 结尾者，`.ready.sending` 结尾是 `.sending`，安全——实现后抓目录确认）。
  - 单/多 peer 路由 `extractTargetPeer` **保持不变**（中心节点多 peer 仍按文件名路由）。
  - 所有超时用 `QElapsedTimer` 单调毫秒。
- **构建/验证（关键回归，仍用默认 60000）**：两 demo **在默认 60000** 下：
  - sync-suite `--selftest` 退出码 0（场景1 收敛 + 场景2 比对/写回）；sync-demo 输出收敛、无 error 记录；
  - 由**观测日志**确认走了「ACK→`.sent`」新状态机、终态 `.sent`、无 `.sending` 残留。
- **landable（如实、不夸大）**：60000/loopback 近无损**只验证 happy path**——新线格式、ACK、`.sent` 状态机、durable 交付主路径；**不会**自然触发发送端 `RTO` 重传/`giveUp`、接收端 `pending` 淘汰。后三者由 **Step 2 的接收端单测**（`eviction/retransmit_recovery/dup`）覆盖；发送端 `RTO/giveUp` 与端到端丢包恢复**不被 loopback 覆盖**，需**可选**的可控丢包注入（设计 §6.3，本计划列为可选增强）来实证。**回滚**：`git revert` 本步（单文件改写，可整体回退）。

### Step 5 · 10 构造点设 `setMaxTransmitBytes(800)`（激活多分片）

- **目标**：把每次传输上限切到 800B，真正激活多分片 + ARQ。
- **改动**：在**全部 10 个** UdpFileTransport 构造点**紧接构造、`start()` 之前**调 `setMaxTransmitBytes(800)`（设计 §2.3、原则「配置须在 start() 前」）：
  - `examples/sync-demo/main.cpp`：`centerTransport/edgeB/edgeC/edgeDTransport`（栈对象，`x.setMaxTransmitBytes(800)` 在 `x.start()` 前）；
  - `examples/sync-suite/Scenario1Runner.cpp`：同上 4 个；
  - `examples/sync-suite/Scenario2Model.cpp`：`centerTransport_/childTransport_`（`make_unique` 后、`->start()` 前 `->setMaxTransmitBytes(800)`）。
  - 建议各 demo 用一个常量（如 `constexpr int kMaxUdpBytes = 800;`）集中，避免散落魔数。
- **构建/验证（核心，此处才真正证明多分片路径）**：两 demo **在 800B** 下：
  - sync-suite `--selftest` 退出码 0；场景2 的 1702B 快照被切成 **3 片**并正确重组、比对结论不变；
  - sync-demo 收敛、无 error；
  - **由观测日志断言**（非 `ls`）：存在 `fragCount>1` 的消息、每 datagram ≤ 800、终态 `.sent`、`pendingCount()` 有界。
- **landable**：800B 端到端跑通——**需求达成**。**回滚**：把 10 处改回默认（或删该行）。

### Step 6 · 双构建 + 全量验证 + 观测

- **目标**：两套构建、全部测试、观测指标一次性过。
- **改动**：无（或补少量观测日志/计数：每工件分片数、重传次数、终态）。
- **验证**：见 §4 验证矩阵。
- **landable**：交付完成。

---

## 3. 构建接线清单（逐一）

### 3.1 新文件登记（`udp_framing.{cpp,h}`，Step 1）

| 构建文件 | 改动 |
|---|---|
| `examples/sync-demo/sync-demo.pro` | `SOURCES += udp_framing.cpp`、`HEADERS += udp_framing.h` |
| `examples/sync-demo/CMakeLists.txt` | `add_executable(sync-demo … udp_framing.cpp)` |
| `examples/sync-suite/sync-suite.pro` | `SOURCES += ../sync-demo/udp_framing.cpp`、`HEADERS += ../sync-demo/udp_framing.h`（`../sync-demo` 已在 INCLUDEPATH） |
| `examples/sync-suite/CMakeLists.txt` | `add_executable(sync-suite … ${…}/../sync-demo/udp_framing.cpp)` |

### 3.2 新单测登记（`tst_udp_reassembly`，Step 1）

- `tests/unit/tst_udp_reassembly.pro`：
  ```
  include(../test-common.pri)
  TARGET  = tst_udp_reassembly
  SOURCES = tst_udp_reassembly.cpp ../../examples/sync-demo/udp_framing.cpp
  INCLUDEPATH += ../../examples/sync-demo
  # udp_framing 为纯逻辑：无需 QT+=network、无需 moc（不含 Q_OBJECT）
  ```
- `tests/tests.pro`：`SUBDIRS += tst_udp_reassembly` + `tst_udp_reassembly.file = unit/tst_udp_reassembly.pro`（仿 `tst_databridge_schema` 的两处登记）。
- `tests/CMakeLists.txt`：`add_dbridge_test(tst_udp_reassembly "unit/tst_udp_reassembly.cpp;${CMAKE_SOURCE_DIR}/examples/sync-demo/udp_framing.cpp")`——`add_dbridge_test(name sources)` 内部把 `${sources}` 展开给 `add_executable`，**分号 list 多源可行**（已核对）；随后 `target_include_directories(tst_udp_reassembly PRIVATE ${CMAKE_SOURCE_DIR}/examples/sync-demo)` 追加头路径。
- 单测**不碰 SQLite**，故**无需** QSQLITE session 插件（比 `tst_databridge_schema` 更简单）。

### 3.3 10 个构造点（Step 5，均在 `start()` 之前）

`examples/sync-demo/main.cpp`（`centerTransport` + `edgeB/C/DTransport` = 4）、`examples/sync-suite/Scenario1Runner.cpp`（同构 4）、`examples/sync-suite/Scenario2Model.cpp`（`centerTransport_` + `childTransport_` = 2）。**共 4+4+2 = 10。**

---

## 4. 验证矩阵

> **时序分离**：同一最终二进制在 Step 5 后硬编码 800、**不能再跑 60000**。故「60000」在 **Step 4 的 commit** 上验、「800」在 **Step 5/6 的 commit** 上验——不是同一二进制同时验两档。

| 维度 | 方式 | 判据 |
|---|---|---|
| 单测 | `QT_QPA_PLATFORM=offscreen ./tst_udp_reassembly` | 全绿（重点 `retransmit_recovery/durable_delivery/eviction/collision/reject_*`） |
| sync-suite · 60000（Step 4 commit） | `--selftest` | 退出码 0；由观测日志确认走新 ARQ 路径、终态 `.sent`、无 `.sending` 残留 |
| sync-suite · 800（Step 5 commit） | `--selftest` | 退出码 0；日志见 `fragCount>1`（1702B 快照=3 片）且每 datagram≤800；比对结论不变 |
| sync-demo · 60000 / 800（各自 commit） | 跑主流程（**恒返回 0**，须机器核对 stdout） | 收敛：末尾四端探测字段一致；正确性：`! grep -qE '存在错误记录\|\[ERR\]'` 于 stdout；800 档另由观测日志见多分片 |
| 双构建 | qmake（`build_qmake_demos`）+ cmake（`build`） | 两套均编译通过、上述运行判据均满足 |
| 内存有界 | 单测 `eviction` + 长跑观察 `pendingCount()` | 不单调增长 |
| 观测激活 | 每消息 `{msgId/artifact/fragCount/最大datagram/retries}` 日志或计数 | 无损：分片>1(800)、重传=0；（可选）单测注入丢包：重传>0 且最终成功 |

> **注意**：sync-demo `main()` 恒 `return 0`（见其结尾），**不能只看退出码**——须核对 stdout 的收敛字段与「无错误记录」。sync-suite `--selftest` 有真实退出码。

---

## 5. 风险与陷阱（实测经验）

1. **pre-commit `clang-format` 会重排并中止首次提交**：提交 C++ 改动时，钩子自动格式化并失败；**重新 `git add` 已格式化的文件再 `git commit`** 即通过（第二次一次过）。`.md` 不受影响。
2. **一个源、两处编译**：`udp_transport.cpp`/`udp_framing.cpp` 同时进 sync-demo 与 sync-suite；**每步两 demo 都要验**，别只测一个。
3. **`setMaxTransmitBytes` 必须在 `start()` 之前**（10 处）——`run()` 启动后配置只读（设计原则）。栈对象在 `.start()` 前调；`unique_ptr` 在 `->start()` 前调。
4. **`.ready.sending` 命名与 `*.ready` glob**：确认认领后的文件不再被 `pollOutbox` 的 `*.ready` 扫到（实现后抓目录确认，防重复发送）。
5. **多 peer 路由不变**：中心节点仍靠 `extractTargetPeer` 按文件名路由；ACK/重传的目标端点取自 `outbound[msgId].dest`。
6. **大端一致**：`QDataStream` 显式 `BigEndian`（沿用现状）；头字段读写严格对称。
7. **单调时钟**：所有超时用 `QElapsedTimer::elapsed()`，勿用系统时间。
8. **单测时间可控**：`FragmentReassembler::feed/evictStale` 的 `nowMs` 由测试传入 → 淘汰/短表用例**无需真实 sleep**，注入递增时间戳即可，测试快且确定。
9. **cmake AUTOMOC / 多源 test**：`tst_udp_reassembly` 编译纯 `udp_framing.cpp`（无 Q_OBJECT）→ 无 moc 负担；只需确认 `add_dbridge_test` 能接多源（否则本地扩展）。
10. **Qt 运行环境**：跑 demo/GUI 单测需前置 `LD_LIBRARY_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/lib` 与 `QT_QPA_PLATFORM_PLUGIN_PATH`；sync-demo/sync-suite 依赖旁置 `sqldrivers/libqsqlite.so`（其 `.pro`/CMake 已部署）。`tst_udp_reassembly` 不碰 DB，`offscreen` 即可。

---

## 6. 完成定义（DoD）

- [ ] `udp_framing.{h,cpp}` 实现 `fragmentMessage` + `FragmentReassembler`（含设计 §4.4 全部校验、durable 交付语义）。
- [ ] `udp_transport.{h,cpp}`：`bool setMaxTransmitBytes(int)` + ARQ + §5.2 交付状态机（**通用 artifact 处理，`.payload`/`.ack` 一视同仁**）；删除 `sentFiles_` 与旧 `sendFile/handleDatagram`。
- [ ] `udp_transport.{h,cpp}` 加**每消息观测**（msgId / filename / fragCount / 最大 datagram 字节 / retries）日志或计数。
- [ ] `tst_udp_reassembly` 用例全绿（含 `retransmit_recovery/durable_delivery/eviction/collision/reject_*`）。
- [ ] **10** 构造点设 800B（`start()` 前）。
- [ ] **60000 在 Step 4 的 commit 验、800 在 Step 5/6 的 commit 验**（同一最终二进制硬编码 800、不能再跑 60000）：各自 sync-suite `--selftest` 退出码 0；sync-demo 输出收敛且 `! grep 存在错误记录`。
- [ ] qmake 与 cmake 双路径均编译+运行通过。
- [ ] **日志断言**：800B 下存在 `fragCount>1` 的消息、每 datagram ≤ 800、终态 `.sent`、`pendingCount()` 有界（**不靠 `ls` 判分片**）。
- [ ] 分阶段提交（每步一 commit，信息含 Co-Authored-By；注意 pre-commit clang-format 首次会重排 → 重新 `git add` 再提交）。

---

## 7. 命令速查（本会话验证过的形态）

```bash
# 通用 Qt 环境
export PATH=/opt/Qt5.12.12/5.12.12/gcc_64/bin:$PATH
export LD_LIBRARY_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/lib
export QT_QPA_PLATFORM_PLUGIN_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/plugins/platforms

# ── qmake ──
cd build_qmake_demos && qmake ../dbridge.pro                 # 加了新源/新 SUBDIRS 后必须重跑
# 新单测（先让 tests 子项目重生成 Makefile）
make -f Makefile sub-tests-tests-pro-qmake_all
make -C tests/unit -f Makefile.tst_udp_reassembly -j$(nproc)
QT_QPA_PLATFORM=offscreen tests/unit/tst_udp_reassembly
# 两 demo（各自子项目加了新源后需在其构建目录重跑 qmake 再 make）
( cd examples/sync-demo  && qmake ../../../examples/sync-demo/sync-demo.pro   && make -j$(nproc) )
( cd examples/sync-suite && qmake ../../../examples/sync-suite/sync-suite.pro && make -j$(nproc) )
QT_QPA_PLATFORM=offscreen examples/sync-suite/sync-suite --selftest --ws /tmp/ss-ws
QT_QPA_PLATFORM=offscreen examples/sync-demo/sync-demo /tmp/sd-ws | tee /tmp/sd.out
! grep -qE '存在错误记录|\[ERR\]' /tmp/sd.out && echo "sync-demo OK(无错误记录)"   # 机器判据(恒返回0, 须核对输出)

# ── cmake ──
cd /home/lfl/excelproject
cmake -S . -B build
cmake --build build --target tst_udp_reassembly sync-demo sync-suite -j$(nproc)
QT_QPA_PLATFORM=offscreen build/tests/tst_udp_reassembly
QT_QPA_PLATFORM=offscreen build/examples/sync-suite/sync-suite --selftest --ws /tmp/ss-cm

# ── 多分片/终态（Step 5 验证）──
# 注意: 文件系统看不到 wire 层 fragCount。多分片由【传输层观测日志】证明, 不是 ls。
#   期望日志: 某 msgId 的 fragCount>1 且每 datagram<=800; ls 仅能看终态 .sent:
ls /tmp/ss-ws/scenario2/snap_center/outbox   # 仅证终态 snapresp__*.payload.sent(不证分片数)
```

> `cd` 进子目录跑 `qmake/make` 若触发沙箱提示，改用 `make -C <dir> -f <Makefile>` 形式（本会话用过）。
