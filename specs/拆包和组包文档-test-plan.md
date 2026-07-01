# 拆包和组包 测试计划（覆盖率 ≥ 90%）

> 依据：`specs/拆包和组包设计文档.md`（WHAT）、`specs/拆包和组包文档-plan.md`（HOW 落地）。
> 本文件是 **VERIFY**：如何用最少的测试代码，把已实现的 `examples/sync-demo/udp_transport.{h,cpp}` 覆盖到 **行覆盖率 ≥ 90%**，并逐分支论证达成。
> 核心口径：**覆盖率分母 = `udp_transport.cpp` 的可执行行**（gcov/lcov 默认口径）；不可达防御分支按 §8 显式豁免后再计。

---

## 0. 与实现现状的关键差异（决定测试形态）

设计文档 §9、实现计划 §0/§3 原设想把纯逻辑（`fragmentMessage` / `FragmentReassembler`）拆到**独立文件 `udp_framing.{h,cpp}`（无 Qt Network、无 Q_OBJECT）**，让单测「只编译纯文件、干净脱离 socket」。

但**实际落地把全部代码留在了 `udp_transport.{h,cpp}` 一个文件里**（`fragmentMessage` / `FragmentReassembler` 与 `UdpFileTransport` 同处一 `.cpp`）。这带来三个**测试必须直面**的后果：

1. **单测无法只编译纯逻辑**：`fragmentMessage` / `FragmentReassembler` 的定义与 `UdpFileTransport`（`Q_OBJECT` + `QThread` + `QUdpSocket`）在同一 `.cpp`，不可分离编译。要测前二者，**必须整份编译 `udp_transport.cpp`** → 单测 target **必须** `QT += network`（`QHostAddress`/`QUdpSocket` 来自 QtNetwork）+ **moc 处理 `udp_transport.h` 的 `Q_OBJECT`**（qmake 靠 `HEADERS`、cmake 靠全局 `AUTOMOC`）。
2. **覆盖率反而更好度量**：因为 `udp_transport.cpp` 被**直接列入测试 target 的 `SOURCES`** 一起插桩编译，gcov 能直接产出该文件的逐行覆盖——无需跨库/跨产物合并覆盖数据。
3. **要 ≥ 90% 必须含 socket 集成层**：纯组件测试只能覆盖 `fragmentMessage` + `FragmentReassembler`（约占该 `.cpp` 可执行行的 **~47%**）；`run()` / `pollOutbox` / `sendAck` / ARQ（约 **~53%**）依赖真实 socket 与文件系统，**不做集成测试就到不了 90%**。这与实现计划 Step4/5「发送端 RTO/giveUp 与端到端丢包恢复不被 loopback happy-path 覆盖，需可控丢包注入」的判断一致——本计划把那条「可选增强」**升格为达标必需项**，并给出**确定性触发手法**（§5.2）。

> 结论：测试分**两层**，合并覆盖率。Part A 纯组件（快、确定），Part B socket 集成（用黑洞端口 / 只读 inbox / 预占端口等确定性手法触发 ARQ 与失败分支）。两层放**同一个 target `tst_udp_reassembly`**，用 slot 命名前缀区分（`frag_*`/`reasm_*`/`peer_*` vs `e2e_*`），一次运行产出合并覆盖。

---

## 1. 测试目标与达成判据

| 目标 | 判据 |
|------|------|
| **T1 行覆盖率 ≥ 90%** | `lcov --list` 报告 `udp_transport.cpp` **Lines ≥ 90.0%**（§8 豁免不可达分支后） |
| **T2 组件①②近乎全覆盖** | `fragmentMessage` + `FragmentReassembler` 的 §4.4 各校验分支（**除设计 §4.4 要求但实现未做的「文件名 UTF-8 有效性」子项——见 §3 的偏差说明**）、状态机分支、淘汰/短表分支被独立断言 |
| **T3 ARQ 三缺口修复被实证** | 丢包恢复（重传/giveUp）、pending 淘汰、msgId 碰撞隔离，各有专用用例断言 |
| **T4 durable 交付语义被实证** | `Completed→不入短表` 与 `markDelivered→入短表` 解耦；落盘失败不误判已交付 |
| **T5 确定性、可重复** | 全部用例不依赖真实网络抖动：组件测试注入 `nowMs` + 扣留分片；集成测试用固定 loopback + 明确终态文件断言（`QTRY_*` 轮询，非固定 sleep） |
| **T6 双构建** | qmake（`tests.pro`）与 cmake（`tests/CMakeLists.txt`）均能编译并运行该单测 |

**覆盖率工具**：GNU `gcov` + `lcov`/`genhtml`（项目用 g++，天然适配）。当前仓库**无任何覆盖率配置**（已核实：`tests/` 下无 `coverage/gcov/lcov/--coverage` 出现），故 §9 给出**新增**的插桩接线。

---

## 2. 覆盖率度量方法（分母口径 + 接线 + 命令）

### 2.1 分母口径

- **主判据 = 行覆盖率（Line coverage）**，分子/分母只统计 `examples/sync-demo/udp_transport.cpp`（用 `lcov --extract '*/udp_transport.cpp'` 过滤，排除测试自身与 Qt 头）。
- **辅参考 = 函数覆盖率（Function coverage）**：应达 100%（每个函数至少被调 1 次）。
- **不把 `udp_transport.h` 计入分母**：头里只有内联 `PendingMsg::complete()` 与常量声明；`complete()` 的覆盖会归到调用它的 `.cpp` 行。
- **§8 的不可达/豁免分支**用 `// LCOV_EXCL_LINE` / `LCOV_EXCL_START..STOP` 标注，使其**不进分母**——这是达成 90% 的合规前提（豁免须逐条列明理由，不得滥用）。

### 2.2 插桩接线（仅测试构建，不影响 demo/库）

- **qmake**（`tests/unit/tst_udp_reassembly.pro`）：
  ```
  QMAKE_CXXFLAGS += --coverage -O0
  QMAKE_LFLAGS   += --coverage
  ```
  因 `udp_transport.cpp` 列入本 `.pro` 的 `SOURCES`，插桩对它生效，`.gcno/.gcda` 落在测试构建目录。
- **cmake**（`tests/CMakeLists.txt`，仅对该 target）：
  ```cmake
  target_compile_options(tst_udp_reassembly PRIVATE --coverage -O0)
  target_link_options(tst_udp_reassembly    PRIVATE --coverage)
  ```

### 2.3 度量命令（qmake 侧为准）

```bash
# 环境（同实现计划 §7）
export PATH=/opt/Qt5.12.12/5.12.12/gcc_64/bin:$PATH
export LD_LIBRARY_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/lib

BUILD=build_qmake_demos/tests/unit          # 测试 .o/.gcno/.gcda 所在
lcov --directory "$BUILD" --zerocounters
QT_QPA_PLATFORM=offscreen "$BUILD/tst_udp_reassembly"       # 跑全部用例(组件+集成；路径以 $BUILD 为准)
lcov --directory "$BUILD" --capture --output-file /tmp/cov_all.info
lcov --extract /tmp/cov_all.info '*/examples/sync-demo/udp_transport.cpp' \
     --output-file /tmp/cov_udp.info
lcov --list /tmp/cov_udp.info               # ← 读 Lines% 判据(≥90.0)
genhtml /tmp/cov_udp.info --output-directory /tmp/cov_html   # 可选:逐行红/绿
```

> gcov 计数在**进程正常退出**时落 `.gcda`。集成用例起后台线程，**务必 `requestStop()+wait()` 优雅回收**再让 `main` 退出，否则线程内代码的覆盖计数可能丢失（§11 陷阱）。

---

## 3. 被测面盘点与可测性分层

以下把三组件的每条分支归入 **A=纯组件单测 / B=socket 集成 / X=不可达豁免**，作为 §7 覆盖映射的索引（分支编号沿用探查结论）。

### 组件① `fragmentMessage`（自由函数，头文件可见）→ 全 A
- 守卫：`fname>255`、`M<1`、`fragCount>65535`、空文件特判、正常单/多片、wire 头逐字段、totalSize 记录 → **A**
- `data.size()>UINT32_MAX` → **X**（Qt5 `QByteArray::size()` 为 `int`，不可构造；§8 豁免）

### 组件② `FragmentReassembler`（类，头文件可见）+ 文件内 `static isFilenameSafe` → 全 A
- `feed()` §4.4 六校验（长度<18 / magic / type / fragCount==0 / fragIdx≥fragCount / 文件名长度越界 / 文件名不安全）、短表命中→NeedAck、新建 pending、元数据不一致→丢弃、重复片不刷新进度、未 complete、总长校验失败→损坏、成功→Completed → **A**
- `markDelivered` / `evictStale`（pending 淘汰 + 短表过期）/ `pendingCount` / `PendingMsg::complete()` → **A**
- `isFilenameSafe` 的 `return false`：空 / `/` / `\` / `..` 四支**只能经 `feed()` 间接测**（文件内 `static`，外部不可直接取用）→ **A（间接）**；`QDir::isAbsolutePath` 行经 `feed()` **不可达**（绝对路径必先被 `/`/`\` 拦截）→ **X**（§8 豁免）
- **⚠️ 设计 §4.4 与实现的偏差（如实标注，最小可落地不改生产）**：设计 §4.4 校验 4 要求文件名「**UTF-8 有效**」，但实现的 `isFilenameSafe` 仅查「空 / `/` / `\` / 绝对路径 / `..`」，且 `feed()` 用 `QString::fromUtf8(...)` 做**有损转换**（非法字节 → U+FFFD 替换，**不拒绝**）。故「UTF-8 有效」这一设计子项**实现并未实现**、本计划**不声称覆盖它**（§1 T2 已收敛措辞）；遵最小可落地**不为此改生产代码**。若未来要对齐设计，须在 `isFilenameSafe` 显式校验 UTF-8 并补 `reasm_reject_invalid_utf8` 用例——**当前不列入达标基线**。

### 组件③ `UdpFileTransport`
- `setMaxTransmitBytes`（<274 拒 / =274 / 中间 / >65507 夹取）→ **A**（`friend` 白盒读 `maxTransmitBytes_`，见 §4.3）
- 构造（单/多 peer）、`requestStop` → **A**
- `extractTargetPeer`（private static，全解析分支：ack/blreq/baselineresponse/baselinerequest/changeset/selectionpush/无法识别/`.sending` 后缀预处理）→ **A**（`friend` 直调）
- `sendAck` → **B**（写 socket）
- `pollOutbox`（认领/改名/读取/路由/拆包/发送/入 outbound 及各失败改 `.failed`）→ **B**（大部分）；其中「拆包失败」「不可路由」两条的**判定逻辑**也可由 A 间接印证，但**改名终态**须 B
- `run()`（bind 成败、收包分派 DATA/ACK、durable 落盘成败、ACK 端点校验、RTO 重传/giveUp、evictStale、stop）→ **B**（socket/线程/FS 混杂）；其中「短包/坏 magic/坏 type/坏 ACK 长度/未知类型丢弃」等**判定**可由 B 用「手工构造坏包发给接收端」确定性触发

> 行数权重估算（用于 §7 达成测算）：①≈40 行、②≈110 行、③无 socket 部分（setMax/构造/requestStop/extractTargetPeer）≈50 行、③socket 部分（sendAck/pollOutbox/run）≈220 行。总≈420。**A 覆盖 ≈200 行（47%）；A+B ≈390 行（93%）**（扣 §8 豁免 ≈25–30 行后）。

---

## 4. 文件组织与构建接线（可落地）

### 4.1 新增文件

| 文件 | 作用 |
|------|------|
| `tests/unit/tst_udp_reassembly.cpp` | 单一测试类 `TstUdpReassembly`，含 Part A + Part B 全部 slot |
| `tests/unit/tst_udp_reassembly.pro` | qmake 登记（含 `QT += network` 与覆盖率标志） |

### 4.2 `.pro` 内容（关键：覆盖 `test-common.pri` 的 `QT=` 与 moc）

```pro
include(../test-common.pri)                 # QT = core sql gui testlib（覆盖式）
QT += network                               # ← 必须：QHostAddress/QUdpSocket
TARGET   = tst_udp_reassembly
SOURCES  = tst_udp_reassembly.cpp \
           ../../examples/sync-demo/udp_transport.cpp
HEADERS  = ../../examples/sync-demo/udp_transport.h   # ← 让 qmake 对 Q_OBJECT 头生成并编译 moc
INCLUDEPATH += ../../examples/sync-demo
# 覆盖率插桩（仅本测试）
QMAKE_CXXFLAGS += --coverage -O0
QMAKE_LFLAGS   += --coverage
# 本测试不碰 SQLite：test-common.pri 已链 dbridge/QXlsx/sqlite3，多链无害；如需精简可另起最小 .pri
```

> **陷阱**：`test-common.pri` 用 `QT = core sql gui testlib`（**赋值**非追加），故 `QT += network` **必须写在 `include(...)` 之后**，否则被覆盖丢失（§11.1）。

### 4.3 生产代码的**必需**可测性改动（`udp_transport.h`，1 行）

`extractTargetPeer` 是 `private static`、`maxTransmitBytes_` 是 `private`。为**白盒直测**这两者，**必须**在 `UdpFileTransport` 类内加一行友元声明——这是一处**生产头文件改动**（非纯测试接线），须在 DoD 中显式承认：

```cpp
class UdpFileTransport : public QThread {
    Q_OBJECT
    friend class TstUdpReassembly;   // 仅测试可见；生产运行时行为零变化
    ...
};
```

- **硬约束（用非限定名，切勿加 `::`）**：**必须**写 `friend class TstUdpReassembly;`（非限定名）——C++ 规则下它会在**最近的封闭命名空间**（此处即全局命名空间，因 `UdpFileTransport` 在全局）**隐式声明** `TstUdpReassembly`，无需前置声明。**不可**写 `friend class ::TstUdpReassembly;`：限定名（带 `::`）在友元声明中**不引入新声明**、要求该名字已存在，否则报错 `'TstUdpReassembly' in namespace '::' does not name a type`（已实测确认）。因此测试类 `TstUdpReassembly` **必须定义在全局命名空间**且**精确同名**（与隐式声明作用域一致），否则 A24/A25/A26 的白盒断言无法编译。
- **取舍**：友元是**最小侵入**（一行、不改任何运行时行为、不放宽对外 API）。换来 `extractTargetPeer` 的 8 类解析分支、`setMaxTransmitBytes` 的夹取分支可**直接白盒断言**，而非绕一大圈端到端间接推断。
- **备选（零侵入黑盒，覆盖率降级）**：若团队坚持生产头零改动，则不加友元，A24/A25/A26 降级为：`setMaxTransmitBytes` 只断言返回值（`bool`，public 可直接调，但无法读回 `maxTransmitBytes_` 做白盒校验）+ Part B「设 800 后端到端看分片数」间接印证；`extractTargetPeer` 只能靠 Part B「多 peer 端到端观察工件被投到哪个端口」间接覆盖——**用例更重、更慢、且分支区分度差**（难判定走了哪条解析分支，`size<N` 等边界分支大概率漏，覆盖率随之下降）。**故本计划以「加友元」为达标基线**。

### 4.4 构建登记

| 构建文件 | 改动 |
|---|---|
| `tests/tests.pro` | `SUBDIRS += tst_udp_reassembly` + `tst_udp_reassembly.file = unit/tst_udp_reassembly.pro`（仿 `tst_databridge_schema` 两处登记） |
| `tests/CMakeLists.txt` | **① 先补 `find_package` 组件**：把顶部 `find_package(Qt5 5.12 REQUIRED COMPONENTS Core Sql Test)` 改为 `... COMPONENTS Core Sql Test Network`（否则导入目标 `Qt5::Network` **不存在**，配置期即失败）。**②** `add_dbridge_test(tst_udp_reassembly "unit/tst_udp_reassembly.cpp;${CMAKE_SOURCE_DIR}/examples/sync-demo/udp_transport.cpp")`；随后 `target_include_directories(tst_udp_reassembly PRIVATE ${CMAKE_SOURCE_DIR}/examples/sync-demo)` + `target_link_libraries(tst_udp_reassembly PRIVATE Qt5::Network)`（宏未链 Network，须补）+ §2.2 覆盖率选项。全局 `AUTOMOC ON` 已能处理 `Q_OBJECT` 头 |

---

## 5. 测试分层设计

### 5.1 Part A — 纯组件单测（确定性、注入丢包/时间）

**方法**：直接 `#include "udp_transport.h"`，调 `fragmentMessage(...)` 产出数据报，**按需扣留若干**后 `reasm.feed(...)`，断言 `RxEvent`。淘汰/短表用**构造注入小超时** `FragmentReassembler(reassemblyTimeoutMs, completedRetentionMs)` + **测试给定 `nowMs`**（手推时间戳，无需真实 sleep）。丢包 = 「不喂某些片」，**对生产代码零侵入**。

**断言风格**：`QCOMPARE`（枚举/字节/计数）、`QVERIFY`（bool）、`QVERIFY2`（带诊断）。字节一致用 `QCOMPARE(ev.bytes, original)`。

### 5.2 Part B — socket 集成单测（真实 loopback + 临时目录 + 确定性触发）

**方法**：`QTemporaryDir` 造 outbox/inbox；起真实 `UdpFileTransport` 后台线程；用 `QTRY_VERIFY_WITH_TIMEOUT` 轮询**终态文件后缀**（`.sent`/`.failed`）或 inbox 产出，而非固定 sleep。全部端口用 loopback 高位端口（避开 demo 的 150xx/152xx，用 **199xx** 段，且每用例用不同端口避免 TIME_WAIT/串扰）。

**确定性触发 ARQ / 失败分支的手法**（核心，替代生产内置丢包钩子）：

| 要触发的分支 | 确定性手法 |
|---|---|
| **happy path → `.sent`** | 起「发送端 A→B」+「接收端 B→A、inbox 可写」；放工件 → 断言 outbox 出 `.sent`、B 的 inbox 出工件+`.ready` |
| **多分片端到端** | 同上 + `setMaxTransmitBytes(800)` + 工件 > 752B（如 2KB 随机）；断言 inbox 字节与原文**逐字节一致** |
| **RTO 重传 + giveUp → `.failed`** | 只起发送端，**对端端口无接收者（黑洞）**或起一个「只收不回 ACK 的哑 `QUdpSocket`」；发送端首发后每 `RTO=500ms` 重发，`maxRetries=5` 后 giveUp → 断言 outbox 出 `.failed`；哑 socket 计 DATA 到达数 == **1+5=6**（实证重传次数） |
| **durable 落盘失败 → 不 ACK → 持续重传** | 令接收端 `inboxDir_` **本身是一个已存在的普通文件**（非目录）：`mkpath` 失败、目标路径父级非目录 → `QFile::open(WriteOnly)` 失败。**不用**「inbox 目录下预放同名文件」（会被 `WriteOnly` 截断并**成功**）；`chmod 0500` 只读目录仅作备选。断言接收端 inbox 侧**无 `.ready` 产出**、发送端最终 `.failed`（B5） |
| **不可路由 → `.failed`** | 多 peer 发送端；放一个文件名不符合任何命名契约（`extractTargetPeer` 返回空）或 peer 未登记的工件；断言直接 `.failed`（不发送）（B6） |
| **拆包失败 → `.failed`** | **可落地手法（须单 peer 发送端**，使 `extractTargetPeer` 不介入、避免先走「不可路由」分支截获**）**：主文件名 UTF-8 **249 字节**（使 sidecar `{name}.ready` = 249+6 = **255** 正好 `NAME_MAX`，能建出）+ `setMaxTransmitBytes(274)`（→ `M=274-18-249=7`）+ 数据 **458746B**（`>65535×7`）→ `fragCount>65535` → `ok=false`。**不用**「文件名 255 字节」（`.ready` 会达 261 > `NAME_MAX`，sidecar 建不出、`pollOutbox` 扫不到）。断言 `.failed`、无 DATA 发出（B7） |
| **bind 失败 → 线程即退** | 先用独立 `QUdpSocket` 预占端口 P（`LocalHost:P`），再 `start()` 一个 bind 到同址的 transport；断言其 `wait(1500)` 返回 true。**平台假设**：Linux/当前 CI 默认 bind 模式（跨平台不保证，见 §11.12）（B8） |
| **坏包丢弃不崩溃** | 向接收端端口手工发：①`<5B` 短包 ②错 magic ③`type=0x03` 未知 ④`type=0x02` 但长度≠9 的坏 ACK；断言接收端存活、inbox 无产出、后续正常工件仍能送达（B9） |
| **ACK msgId 不在 outbound** | 向发送端发一个 magic/type 正确、9B、但 `msgId` 不匹配任何在途消息的 ACK；断言被忽略、在途工件不受影响（B10） |
| **ACK 端点校验（可选）** | **哑 socket 抓发送端首个 DATA 包并解析 `msgId`**，再从**错误端点**发该 `msgId` 的合法 ACK；断言工件**不**变 `.sent`。**不写**「friend 读 outbound」——`outbound` 是 `run()` **局部变量**，friend 亦访问不到（B11） |
| **pollOutbox 边界** | ① outbox 目录不存在 → `!dir.exists()` return；② `*.ready` 存在但主文件缺失 → `!QFile::exists` continue（B12） |
| **认领 / 主文件 rename 失败 → 跳过 / `.failed`** | 借 `QFile::rename` **拒绝覆盖已存在目标** 的语义在单线程内确定性触发（无需并发）：① 预建 `{name}.payload.ready.sending` → 认领 `rename(.ready→.ready.sending)` 失败 → `continue`（不认领、不发送、无 `.failed`）；② 预建 `{name}.payload.sending` → 认领成功但主文件 `rename(→.sending)` 失败 → 告警 + `.ready.sending`→`.ready.failed`、不发送（B13） |
| **NeedAck 端到端（可选）** | 组件层已由 `reasm_dup_after_complete`（A18）覆盖 `NeedAck` 逻辑；`run()` 内 `NeedAck→sendAck` 一行端到端触发需重放同 msgId 分片，成本高收益低 → **可选**（覆盖率差临门一脚再补） |

> **时间成本诚实说明**：`RTO/maxRetries` 是**编译期常量**（设计明确不提供 `setReliabilityParams`），集成层无法调快。故每个 **giveUp 类用例 ≈ 5×500ms + 1×RTO ≈ 3.0–3.5s**。含 giveUp 的用例（giveUp、durable-fail）合计约 **6–8s**；happy/多分片/坏包等亚秒级。全套集成 **≈ 10–12s**，可接受但须在 CI 标注。若未来允许「测试构建经宏 override RTO」，可将其压到毫秒级——**本计划不改生产常量**。

---

## 6. 用例清单（逐条：名 / 场景 / 断言 / 覆盖分支）

### 6.1 Part A 用例（组件①②③无 socket 部分）

| # | slot 名 | 场景 | 关键断言 | 覆盖分支 |
|---|---------|------|---------|---------|
| A1 | `frag_sizes` | data ∈ {0,1,M,M+1,多片} × 文件名{空,普通,255} × maxBytes{274,800,60000} | 分片数 = ceil；每报 ≤ maxBytes；空文件=1 片 | ①正常/空/边界 |
| A2 | `frag_wire_header` | 固定输入(msgId/fname/data) | 逐字节校验头 18B：magic=0xDB5ACED0、type=0x01、msgId、frag_idx、frag_count、total_size、fname_len，随后 fname+payload | ①wire 格式 |
| A3 | `frag_err_fname_too_long` | 文件名 UTF-8 = 256 | `ok=false`，error 含 "filename" | ①`fname>255` |
| A4 | `frag_err_no_payload_room` | maxBytes 使 `M<1`（如 fname=1、maxBytes=18） | `ok=false`，error 含 "no room" | ①`M<1` |
| A5 | `frag_err_too_many_frags` | 极小 M + 数据使 fragCount>65535 | `ok=false`，error 含 "fragCount" | ①`fragCount>65535` |
| A6 | `roundtrip_inorder` | 顺序喂全部片 | `Completed`；`bytes` 与原文逐字节一致 | ②complete/assemble/成功 |
| A7 | `roundtrip_reorder_dup` | 乱序 + 重复喂 | `Completed` 且一致；重复片幂等 | ②乱序/重复片 |
| A8 | `reasm_reject_short` | `feed` 一个 17B 包 | `None`；`pendingCount()==0` | ②长度<18 |
| A9 | `reasm_reject_bad_magic` | 18B 但 magic 错 | `None` | ②magic |
| A10 | `reasm_reject_bad_type` | magic 对、type=0x02 | `None` | ②type |
| A11 | `reasm_reject_out_of_range` | `fragIdx==fragCount`、及 `fragCount==0` | `None`；不污染 pending、不误判 complete | ②越界/fragCount==0 |
| A12 | `reasm_reject_fnamelen_overflow` | `dg.size() < 18+fnLen` | `None` | ②文件名长度越界 |
| A13 | `reasm_reject_unsafe_filename` | filename ∈ {空,"a/b","a\\b",".."} 各一 | 每个均 `None`。**注意**：`isFilenameSafe` 校验顺序为「空→含'/'→含'\\'→`isAbsolutePath`→`==".."`」，绝对路径（如 `/abs`）必含 `/`，会**先**被 `contains('/')` 拦截，故 `isAbsolutePath` 的 `return false` 行经 `feed()` **不可达**（§8 豁免） | ②文件名安全（空/'/'/'\\'/".." 四支；isAbsolutePath 见 §8 豁免） |
| A14 | `reasm_reject_meta_mismatch` | 首片后，次片 fragCount/totalSize/fname_len/fname 各改一处 | 每种不一致均被丢弃；不 complete | ②元数据不一致 |
| A15 | `reasm_reject_size_mismatch` | 构造「片数凑满但 assemble().size()≠totalSize」 | `None`；判损坏；`pendingCount()==0` | ②总长校验失败 |
| A16 | `retransmit_recovery` | 先扣留第 k 片→不 complete；再整条重喂 | 补喂后 `Completed`（模拟全量重传恢复） | ②未 complete→补齐 |
| A17 | `durable_delivery` | `Completed` 后**不**`markDelivered`→再喂整条 | 再次 `Completed`（可重投）；随后 `markDelivered`→再喂→`NeedAck` | ②durable 解耦/短表命中 |
| A18 | `reasm_dup_after_complete` | `markDelivered` 后重喂同消息 DATA | `NeedAck`；不重复交付；不重建 pending | ②短表命中→NeedAck |
| A19 | `eviction` | 小 `reassemblyTimeout`；半成品静置超时后 `evictStale(now)` | `pendingCount()==0`（无泄漏）；未超时者保留 | ②pending 淘汰(超时/未超时) |
| A20 | `dup_frag_no_progress` | 小超时；只喂重复片刷不动进度，`now` 越过阈值后 `evictStale` | 半成品仍被淘汰（证「重复片不刷新 lastProgressAt」） | ②重复片不刷进度 |
| A21 | `completed_retention_expiry` | 小 `completedRetention`；`markDelivered` 后 `now` 越阈值 `evictStale`，再喂整条 | 短表已过期→按**新消息**重建并 `Completed`（非 NeedAck） | ②短表过期清理 |
| A22 | `collision_two_senders` | 两个不同 `senderKey`、相同 `msgId` 交替喂 | 各自独立 `Completed`；内容不串扰 | ②归并键含端点(碰撞隔离) |
| A23 | `pending_count_tracking` | feed 若干未 complete → complete/evict | `pendingCount()` 动态正确（0→N→减） | ②pendingCount |
| A24 | `set_max_transmit_bytes` | 273/274/1000/65507/65508 | 273→false 且 `maxTransmitBytes_` 不变；274/1000/65507→true 且值相符；65508→true 且被夹到 65507（friend 读私有） | ③setMax 全分支 |
| A25 | `extract_peer_all` | friend 直调 `extractTargetPeer`，逐一喂下表精确输入 | 见下方「A25 精确期望表」：每类返回预期 peer 或空串 | ③extractTargetPeer 全分支 + `.sending` 预处理 |
| A26 | `ctor_and_stop` | 单 peer / 多 peer / 空 peers 构造；`requestStop` 一次/多次 | 构造不抛；默认 `maxTransmitBytes_==60000`（friend 读私有）；`requestStop` 的置位由 B8「bind 失败线程速退」+ B1「happy path 收尾 wait()」端到端间接印证（`stop_` 为 private `QAtomicInt`，friend 亦可直读断言） | ③构造/requestStop |

**A25 精确期望表**（对照代码：先去 `.sending` 再去 `.payload`/`.ack`；`selectionpush` 取 `peer-` 后首段）：

| 输入文件名 | 期望返回 | 覆盖分支 |
|---|---|---|
| `ack__from__nodeX__ms__uuid.ack` | `nodeX` | ack（size≥3） |
| `ack__nodeX`（size<3） | `""` | ack size 不足→继续 |
| `blreq__from__nodeY__ms.payload` | `nodeY` | blreq（size≥3） |
| `a__nodeZ__x__y__baselineresponse.payload` | `nodeZ` | baselineresponse（取 parts[1]） |
| `a__nodeW__x__y__baselinerequest.payload` | `nodeW` | baselinerequest（取 parts[1]） |
| `origin__ep__changeset__seq__peerA-uuid.payload` | `peerA` | changeset（`lastIndexOf('-')` 取左侧） |
| `origin__ep__changeset__seq__peer-A-uuid.payload` | `peer-A` | changeset：seg=`peer-A-uuid`，`lastIndexOf('-')` 取左侧=`peer-A`（锁定「取**最后**一个 `-` 左侧」，非首个） |
| `origin__ep__changeset__seq__nodash.payload` | `""` | changeset seg 无 `-`→继续 |
| `origin__ep__selectionpush__id__peer-nodeB-uuid.payload` | `nodeB` | selectionpush：去 `peer-` 前缀得 `nodeB-uuid`，再 `lastIndexOf('-')` 取左侧=`nodeB` |
| `origin__ep__selectionpush__id__peer-node-B-uuid.payload` | `node-B` | selectionpush：去 `peer-` 得 `node-B-uuid`，`lastIndexOf('-')` 取左侧=`node-B`（锁定「去 `peer-` 后按**最后**一个 `-` 切分」，非取首段） |
| `origin__ep__selectionpush__id__nopeerprefix.payload` | `""` | selectionpush seg 不以 `peer-` 开头→继续 |
| `ack__from__nodeC__ms__uuid.payload.sending` | `nodeC` | `.sending` 预处理→再去 `.payload`（**关键回归**：终态在途文件的 ACK/重传路由） |
| `ack__from__nodeD__ms__uuid.ack.sending` | `nodeD` | `.sending`→再去 `.ack` |
| `unknown__format.payload` | `""` | 无匹配→空串 |

### 6.2 Part B 用例（组件③ socket 部分）

| # | slot 名 | 场景 | 关键断言 | 覆盖分支 |
|---|---------|------|---------|---------|
| B1 | `e2e_happy_single` | A→B 单片工件端到端 | `QTRY`：outbox 出 `.sent`；B inbox 出工件+`.ready`；无 `.failed` | run(): bind ok / 收 DATA→Completed→写盘 ok→markDelivered→sendAck / 收 ACK→端点匹配→`.sent`；pollOutbox 全 happy；sendAck |
| B2 | `e2e_happy_multifrag_800` | A→B，`setMaxTransmitBytes(800)`，工件 2KB 随机 | `QTRY`：`.sent`；B inbox 字节 == 原文（多片重组正确） | 同 B1 + fragmentMessage 多片 + 接收端多片 assemble |
| B3 | `e2e_multipeer_routing` | 多 peer 中心，两个不同目标工件 | 各工件被投到对应端口的接收者 inbox（证 `extractTargetPeer` 端到端路由）；均 `.sent` | pollOutbox 多 peer 路由(7.2) |
| B4 | `e2e_giveup_no_ack` | 只起发送端；对端为「只收不回 ACK 的哑 `QUdpSocket`」。**工件必须是单片小文件**（如 100B，默认 60000B 上限下 fragCount=1） | `QTRY(≈4s)`：outbox 出 `.failed`；哑 socket 累计收到 **6** 个 DATA（首发1+重传5）。**注**：断言 `6` 仅对单片成立——多片时每轮重发所有分片，总数=`fragCount×6`，故本用例锁定单片 | run(): RTO 重传(9.2) + giveUp(9.3) |
| B5 | `e2e_durable_write_fail` | 让接收端 `inboxDir_` **本身是一个已存在的普通文件**（非目录）：则 `mkpath` 失败、`QDir(inboxDir_).filePath(fn)` 的父路径不是目录 → `QFile::open(WriteOnly)` 失败。**不用**「inbox 目录下预放同名文件」（那会被 `WriteOnly` 截断并**成功**，触发不了失败）；`chmod 0500` 只读目录作**备选**（受 root/FS 影响不稳） | 接收端**无 `.ready`** 产出；发送端最终 `.failed`（不 ACK→重传→giveUp） | run(): 落盘 `writeOk=false`→不 ACK(6.3) |
| B6 | `e2e_unroutable_failed` | 多 peer；工件名不可解析或 peer 未登记 | `QTRY`：直接 `.failed`；对端无任何 DATA 到达 | pollOutbox 不可路由→`.failed`(7.3) |
| B7 | `e2e_fragment_fail_failed` | **须用单 peer 发送端**（`peers_.size()==1`，跳过 `extractTargetPeer`，避免先走「不可路由→`.failed`」而覆盖不到拆包失败）。**可落地触发拆包失败**：主文件名 UTF-8 **249 字节**（则 sidecar `{name}.ready` = 249+6 = **255** = `NAME_MAX`，可创建；主文件本身 249B 亦 ≤ 255）+ 发送端 `setMaxTransmitBytes(274)`（→ `M=274-18-249=7`）+ 工件数据 **458746B**（`>65535×7=458745`）→ `fragCount=65536>65535` → `fragmentMessage` `ok=false`。**不用**「文件名 255 字节」——`{name}.ready` 会达 **261 字节 > `NAME_MAX`**，sidecar 根本建不出、`pollOutbox` 的 `*.ready` 扫不到，触发不了 | `QTRY`：`.failed`；对端无 DATA | pollOutbox 拆包失败→`.failed`(8.2) |
| B8 | `e2e_bind_fail_thread_exits` | 预占端口 P（独立 `QUdpSocket` bind `LocalHost:P`），再起 bind 到同 `LocalHost:P` 的 transport。**平台假设**：Linux/当前 CI 下默认 bind 模式，同址同端口二次 bind 必失败（跨平台不保证，文档限定本环境） | 其 `wait(1500)`==true（线程速退）；放入的工件不被发出 | run(): bind 失败→return(1.2) |
| B9 | `e2e_garbage_datagrams` | 向接收端发 4 类坏包（短/错 magic/未知 type/坏长 ACK），随后发 1 个正常工件 | 接收端存活；坏包无 inbox 产出；正常工件仍 `.sent`+inbox（证丢弃分支不影响后续） | run(): dg<5 / magic / 未知 type / ACK 长度≠9 丢弃(4.1/5.1/7.7/7.2) |
| B10 | `e2e_ack_msgid_not_in_outbound` | 向**发送端**发一个格式合法但 `msgId` 不在其 outbound 的 ACK（9B，magic/type 正确） | 发送端存活、忽略该 ACK（`!outbound.contains` 分支）；在途工件不受影响 | run(): ACK `msgId` 不在 outbound→continue |
| B11 | `e2e_ack_endpoint_mismatch`（可选） | **哑 socket 抓发送端首个 DATA 包、解析出 `msgId`**，再从**错误端点**（另一 socket/端口）发一个该 `msgId` 的合法 ACK | 工件**不**变 `.sent`（端点不匹配丢弃）；随后从正确端点发 ACK 才 `.sent`。**不写**「friend 读 outbound」——`outbound` 是 `run()` 局部变量，friend 也访问不到 | run(): ACK 端点校验(7.6) |
| B12 | `e2e_poll_edge_cases` | ① outbox 目录不存在的 transport；② outbox 里只放 `*.ready` 但**缺主文件** | ① `pollOutbox` 开头 `!dir.exists()` 直接 return（线程存活、无发送）；② `!QFile::exists(artifactPath)` → `continue`（不认领、不发送、无 `.failed`） | pollOutbox: outbox 不存在 return / 主文件缺失 continue |
| B13 | `e2e_rename_fail_deterministic` | **单 peer 发送端**；借 `QFile::rename` **拒绝覆盖已存在目标** 单线程确定性触发：① 认领前预建 `{name}.payload.ready.sending`；② 另一工件预建 `{name}.payload.sending`（认领 `.ready→.ready.sending` 仍成功） | ① 短等/`QTRY`：`.ready` 仍在（未认领）、无 DATA、无 `.failed`；② 出现 `.ready.failed`、主 `.payload` 未变 `.sending`（rename 失败）、无 DATA | pollOutbox: 认领 rename 失败→continue / 主文件 rename 失败→`.ready.failed`(§5.2 状态机②) |

> **B1–B10 + B12 + B13 为达标基线**（B10「ACK msgId 不在 outbound」、B12「pollOutbox 边界」、B13「认领/主文件 rename 失败」——均手法简单、**单线程确定性**触发、§7.5 覆盖论证已计入，必须实现）；**B11「ACK 端点校验」+ NeedAck 为可选补充**：仅为把 `run()` 内个别分支的**那一行**也染绿，达 90% 不强依赖；若覆盖率报告显示仍差临门一脚再补。

---

## 7. 覆盖率达成论证（源分支 → 用例映射 + 测算）

### 7.1 组件① `fragmentMessage`（≈40 行）
- 守卫三分支：A3/A4/A5；空文件：A1；正常单/多片 + M/M+1 边界：A1；wire 头：A2；多片 payload 切分与 append：A1/A2/B2。→ **除 `UINT32_MAX`(X) 外全覆盖**。

### 7.2 组件② `FragmentReassembler` + `isFilenameSafe`（≈110 行）
- `feed` 六校验：A8/A9/A10/A11/A12/A13；短表命中：A17/A18；新建/一致/不一致：A6/A14；重复片不刷进度：A7/A20；未 complete：A16；成功 + assemble：A6/A7；总长失败：A15。
- `markDelivered`：A17/A18/A21；`evictStale` 两支（pending 淘汰 + 短表过期，含未超时保留）：A19/A20/A21；`pendingCount`：A23；`PendingMsg::complete`（满 / 不满）：A6/A16（其 `fragCount==0` 防御条件经 `feed()` 不可达——`feed` 已先拒 `fragCount==0`，不会建 pending 后再调 `complete`；且 `complete()` 为**头文件内联**，不计入 `udp_transport.cpp` 主分母）；`isFilenameSafe` 的空/'/'/'\\'/".." 四支：A13。→ **除 `isAbsolutePath` 的 `return false` 行外全覆盖**（该行经 `feed()` 不可达：绝对路径必含 `/` 或 `\`，先被前置 `contains` 拦截 → §8 豁免）。

### 7.3 组件③ 无 socket 部分（≈50 行）
- `setMaxTransmitBytes` 全分支：A24；`extractTargetPeer` 全解析分支 + 后缀预处理：A25；构造(单/多/空)+`requestStop`：A26。→ **全覆盖**（依赖 §4.3 友元）。

### 7.4 组件③ socket 部分（≈220 行）
- `run` bind 成/败：B1/B8；收 DATA→Completed→落盘成功→markDelivered→sendAck：B1/B2；落盘失败→不 ACK：B5；收 ACK→端点匹配→`.sent`：B1；ACK `msgId` 不在 outbound→continue：B10；端点不匹配丢弃：B11(可选)；坏包/未知类型/坏 ACK 长度丢弃：B9；`NeedAck→sendAck`：可选(见 §5.2)；RTO 重传：B4；giveUp→`.failed`：B4；`evictStale`（在 run 内周期调用）：B1（每轮都走）。
- `pollOutbox` 认领→改名→读取→路由→拆包→发送→入 outbound：B1/B2/B3；outbox 不存在 return / 主文件缺失 continue：B12；不可路由→`.failed`：B6；拆包失败→`.failed`：B7。
- `sendAck`：B1（每次交付都发）。
- **认领 rename 失败 / 主文件改名失败**：由 **B13** 确定性触发（预建目标文件使 `QFile::rename` 拒绝覆盖），**不再豁免**；**读取 open 成功但 `readAll` 出错**（读中损坏）→ §8 豁免（无确定性手法）；**open 失败**可 `chmod 000`（非 root）尽力覆盖（能覆则不豁免）。

### 7.5 达成测算（先验估计，实测以 lcov 为准）

> **本节是先验定性论证，非 gcov 实测行数**。`udp_transport.cpp` 含注释/空行共约 595 行；**可执行行数（lcov 的 `LF`）须实测确定**。达标判据以 §2.3 的 `lcov --list` 输出为唯一依据，不以本节估算数字为准。

记可执行行为 `LF`，§8 豁免行为 `E`（不进分母），A+B 覆盖行为 `LH`。则：

```
分母 = LF − E
分子 = LH ≤ 分母           ← 覆盖行恒 ≤ 分母，不会出现 >100%
行覆盖率 = LH / (LF − E) = 1 − U/(LF − E)     U = 既未测又未豁免的残留红行
```

- **A 覆盖**：组件①（除 UINT32_MAX 注释，无可执行判定）+ ②（除 `isAbsolutePath` 行豁免）+ ③无socket 全部 → 近乎全覆盖其所辖行。
- **B 覆盖**：③socket 除「§8 的 3 类 FS 竞态豁免行」外全部（含 B10/B12 补的 edge case、B4/B5 的 ARQ 路径）。
- **残留红行 `U`**：把 §6.2 全部 B 用例（含 B10/B12）写全后，`U` 应趋近 0——非豁免行几乎都有用例命中。

故先验判断 **行覆盖率 ≥ 90%**（分子恒 ≤ 分母，无逻辑矛盾）。

**实测校准流程（达标以此为准）**：
1. 先只编译+跑 **Part A** → `lcov --list` 记基线（预期 `udp_transport.cpp` 约 **45–50%**，因 socket 部分全红）；
2. 再加 **Part B** 复测 → 应跃升至 **≥ 90%**；
3. 若差临门一脚：用 §7.4 的 FS 错误路径 + HTML 报告定位具体红行 → **优先补 §6.2 的可选用例 B11/NeedAck**（B10/B12/B13 已在必跑基线内），其次才按 §8 逐条豁免（**不得下调 90% 判据**）。

---

## 8. 未覆盖分支的诚实清单与豁免（`LCOV_EXCL_*`）

**豁免须逐条注明理由，禁止用豁免凑数**。以下为**允许**豁免（不进分母）：

| 位置 | 分支 | 豁免理由 | 标注 |
|------|------|---------|------|
| `isFilenameSafe` | `QDir::isAbsolutePath(name)` 的 `return false` 行 | 校验顺序为「空→`contains('/')`→`contains('\\')`→`isAbsolutePath`→`==".."`」；任何绝对路径（Unix `/…`、Win `C:\…`）必含 `/` 或 `\`，**先被前置 `contains` 拦截**，故此 `return false` 行经 `feed()` **不可达** | `// LCOV_EXCL_LINE` 于该 `return false` 行（备选：把 `isAbsolutePath` 检查**提前到 `contains` 之前**即可用 `feed("/abs")` 覆盖，但属生产改动，本计划不做） |
| `feed` | `ds.status() != QDataStream::Ok` 的 `return {}` 行 | 前置已判 `datagram.size() < 18` 返回；能走到此处必 ≥18 字节，读取 18B 固定头不会流失败 → **不可达** | `// LCOV_EXCL_LINE` 于该 `return {}` 行 |
| `extractTargetPeer` | `parts.isEmpty()` 的 `return {}` 行 | `QString::split("__")` **恒返回 ≥1 元素**（空串亦返回 `[""]`），故此判永假 → **不可达** | `// LCOV_EXCL_LINE` 于该 `return {}` 行 |
| `pollOutbox` | ③ 读取失败块 `if (!readOk)`（**统一块**，同时含 open 失败 + `readAll` 后 `f.error()!=NoError`；非两条独立行） | `readAll` **读中损坏**无确定性手法；**open 失败**可 `chmod 000`（非 root）把认领后的 `.sending` 主文件变不可读来触发——**能覆则不豁免**，否则整块属平台相关、收益低而豁免 | `LCOV_EXCL_START/STOP` 包住该 `if (!readOk)` 块（若已用 chmod 覆盖 open-fail，则仅豁免 `readAll` 损坏一路） |

> **原豁免的两条 `pollOutbox` rename 失败分支已移出豁免**：认领 `rename(.ready→.ready.sending)` 失败、主文件 `rename(→.sending)` 失败——原判「需并发竞态、无法确定触发」**有误**。`QFile::rename` **拒绝覆盖已存在目标**，故单线程内**预建目标文件**即可确定性触发，现由 **B13** 覆盖（遵本节「有确定性手法即不得豁免」）。

> **说明（对照实现）**：`fragmentMessage` 里的 `data.size() > UINT32_MAX` **在实现中已是纯注释、无 `if` 判定行**（见 `udp_transport.cpp` 内「不可达但保留语义」注释），故**本就不产生可执行行、不进分母、无需豁免**——不列入本表。
>
> **不豁免**：`run` 的 `waitForReadyRead` 返回 false 空转（每轮必经，B 系列自然覆盖）；以及任何有确定性触发手法（黑洞端口、只读 inbox、预占端口、坏包注入、**249 字节文件名+`fragCount>65535`（B7）**、**认领/主文件 rename 失败——预建目标文件使 `QFile::rename` 拒绝覆盖即可单线程触发（B13）**、不可路由名）的分支——**必须写用例覆盖，禁止用豁免凑数**。

> 若最终仅靠豁免仍 < 90%，说明豁免过度或用例不足 → 回到 §6 补 **B11/NeedAck**（B10/B12/B13 已在达标基线内）或用 `chmod 000` 覆盖 open-fail，**不得下调判据**。

---

## 9. 构建接线清单（逐一）

### 9.1 生产头（1 行）
- `examples/sync-demo/udp_transport.h`：`UdpFileTransport` 内加 `friend class TstUdpReassembly;`（§4.3；若坚持零侵入则跳过并接受 §4.3 备选降级）。

### 9.2 新单测登记
- `tests/unit/tst_udp_reassembly.pro`：§4.2 内容。
- `tests/tests.pro`：`SUBDIRS += tst_udp_reassembly` + `.file` 映射（两处）。
- `tests/CMakeLists.txt`：**先把顶部 `find_package(Qt5 5.12 REQUIRED COMPONENTS Core Sql Test)` 补为 `... Core Sql Test Network`**（否则 `Qt5::Network` 导入目标不存在、配置期失败）；再 `add_dbridge_test(tst_udp_reassembly "unit/tst_udp_reassembly.cpp;.../udp_transport.cpp")` + `target_include_directories(... examples/sync-demo)` + `target_link_libraries(... Qt5::Network)` + §2.2 覆盖率选项。

### 9.3 覆盖率工具
- 本机需 `lcov`/`genhtml`（`gcov` 随 g++）。缺失则 `apt install lcov`（仅度量用，不进生产依赖）。

---

## 10. 验证矩阵

| 维度 | 命令 | 判据 |
|---|---|---|
| 单测编译(qmake) | 见实现计划 §7 的 tests 重生成 + `make -C tests/unit -f Makefile.tst_udp_reassembly` | 编译链接通过（含 network + moc） |
| 单测运行 | `QT_QPA_PLATFORM=offscreen tests/unit/tst_udp_reassembly` | 全部 slot `PASS`；进程退出码 0 |
| 组件用例(A) | 同上，`-run frag*,reasm*,peer*,set_max*,ctor*,extract*` | A1–A26 全绿；确定、亚秒级 |
| 集成用例(B) | 同上，`-run e2e_*` | B1–B10 + B12 + B13 全绿（B11/NeedAck 可选）；含 giveUp 用例(~4s)；无线程泄漏 |
| **行覆盖率** | §2.3 lcov 流程 | `udp_transport.cpp` **Lines ≥ 90.0%**、Functions 100% |
| 双构建(cmake) | `cmake --build build --target tst_udp_reassembly` + `QT_QPA_PLATFORM=offscreen build/tests/tst_udp_reassembly` | 编译+运行通过（AUTOMOC + Qt5::Network） |
| 无回归 | `sync-suite --selftest`（800B）+ `sync-demo` | 加 friend 后 demo 行为不变、selftest 退出码 0（§11.5） |

---

## 11. 风险与陷阱

1. **`QT += network` 位置**：必须在 `include(../test-common.pri)` **之后**（该 pri 用 `QT =` 覆盖式赋值），否则丢 network → `QHostAddress` 未定义。
2. **moc**：qmake 靠 `HEADERS = .../udp_transport.h` 触发；cmake 靠全局 `AUTOMOC ON`。漏了则 `UdpFileTransport` 的 `staticMetaObject`/`qt_metacall` 链接缺失。
3. **`add_dbridge_test` 未链 Network**：cmake 侧**必须**补 `target_link_libraries(tst_udp_reassembly PRIVATE Qt5::Network)`，否则链接报未定义符号。
4. **gcov 计数需优雅退出**：集成用例的后台线程**务必 `requestStop()+wait()`** 于 `cleanup()`；线程被强杀或进程 abort 会丢 `.gcda`，覆盖率虚低。
5. **加 friend 的回归**：友元不改运行时行为，但改了头 → demo 需重编译；跑一次 `sync-suite --selftest`(800B) 确认无回归。
6. **端口串扰/TIME_WAIT**：集成用例每个用**独立高位端口**（199xx 段，逐用例 +1），`init()` 起线程、`cleanup()` 停线程 + `QThread::wait()`；避免与 demo 的 150xx/152xx 冲突。
7. **`QTRY_*` 而非固定 sleep**：终态断言用 `QTRY_VERIFY_WITH_TIMEOUT(cond, ms)` 轮询文件后缀；giveUp 类超时设 ≥ 4500ms（覆盖 5×RTO + 抖动）。
8. **RTO 不可调**：编译期常量，giveUp 用例天然 ~3s，属**已知不可压缩成本**；勿为提速去改生产常量（会破坏 §4.6 时间约束）。
9. **durable-fail 触发手法**：优先用「`inboxDir_` 本身是普通文件」（`open(WriteOnly)` 必失败）；**不要**用「inbox 目录下预放同名文件」（会被 `WriteOnly` 截断并**成功**）；`chmod 0500` 只读目录在 root/某些 FS 上仍可写，仅作备选。
10. **clang-format 首提交重排**：提交测试代码时 pre-commit 会重排并中止首次 → 重新 `git add` 再 commit（实现计划 §5.1 同款）。
11. **`offscreen` 足够**：单测不碰 SQLite/GUI，`QT_QPA_PLATFORM=offscreen` 即可，无需 QSQLITE session 插件。
12. **B8 bind 失败的平台假设**：Linux/当前 CI 默认 bind 模式下，同 `LocalHost:P` 二次 bind 必失败；跨平台（如设 `ShareAddress`/`ReuseAddressHint`）不保证——文档限定本环境；若移植他处需另证 bind 失败可触发。
13. **拆包失败用例（B7）不可用超长文件名**：Linux `NAME_MAX=255` 字节。① 文件名 UTF-8 >255 的工件在磁盘上根本创建不了；② 即便用 255 字节主文件名，其 sidecar `{name}.ready` 也达 **261 字节 > `NAME_MAX`**，`pollOutbox` 的 `*.ready` 扫不到 → 触发不了。**正确手法（与 §5.2/B7 完全一致）**：**单 peer 发送端** + **249 字节**主文件名（sidecar `.ready` = 255 = `NAME_MAX`，可建）+ `setMaxTransmitBytes(274)`（→ `M=274-18-249=7`）+ **458746B** 数据（`>65535×7=458745` → `fragCount=65536>65535`）触发 `ok=false`。**切勿**沿用旧稿的「255 字节文件名 + 65536B」——既建不出 sidecar，65536B 在 `M=7` 下也只有 9363 片、够不到阈值。

---

## 12. 完成定义（DoD）

- [ ] `tests/unit/tst_udp_reassembly.{cpp,pro}` 新增；`tests.pro`（两处）与 `tests/CMakeLists.txt`（含 Network + include + 覆盖率）登记完成。
- [ ] `udp_transport.h` 加 `friend class TstUdpReassembly;`（**非限定名，切勿加 `::`**，见 §4.3；**必需**生产可测性改动；测试类须在全局命名空间且精确同名。或选 §4.3 零侵入备选并接受 A24/A25/A26 覆盖降级）。
- [ ] Part A（A1–A26）全绿：组件①②③无 socket 分支全覆盖。
- [ ] Part B（B1–B10 + B12 + B13 达标基线；B11/NeedAck 视覆盖率缺口可选补）全绿：ARQ 重传/giveUp、durable 落盘失败、路由/**拆包失败（单 peer）**、bind 失败、坏包丢弃、ACK msgId 不在 outbound、pollOutbox 边界、**认领/主文件 rename 失败**均由**确定性手法**触发并断言。
- [ ] `lcov --list` 报告 `udp_transport.cpp` **Lines ≥ 90.0%**、Functions 100%；§8 豁免逐条有理由与 `LCOV_EXCL` 标注。
- [ ] qmake + cmake 双构建均能编译并运行该单测；`sync-suite --selftest`(800B) 与 `sync-demo` 无回归。
- [ ] 分阶段提交（组件层 → 集成层 → 覆盖率接线各一 commit，信息含 Co-Authored-By；注意 pre-commit clang-format 首次重排 → 重新 `git add`）。

---

## 附录：与设计文档 §6.1 的对齐

- 设计 §6.1 原列 **13** 个组件用例（`frag_sizes`/`roundtrip_inorder`/`roundtrip_reorder`/`retransmit_recovery`/`durable_delivery`/`eviction`/`collision`/`dup_after_complete`/`reject_out_of_range`/`reject_meta_mismatch`/`reject_size_mismatch`/`reject_unsafe_filename`/`frag_errors`）→ **本计划 Part A 全部纳入并细化**（如 `frag_errors` 拆为 A3/A4/A5 三个可达分支；`reject_*` 逐条独立）。
- 设计 §6.2 端到端（两 demo 800B）→ 由已通过的 `sync-suite --selftest` / `sync-demo` 承担**系统级**验证；本计划 Part B 补的是**单测级、确定性、可断言重传次数/终态**的 ARQ 集成，二者互补。
- 设计明确「**不**在本版：生产内置丢包注入钩子」→ 本计划遵此，Part A 用「扣留分片」、Part B 用「黑洞端口/只读 inbox/预占端口/坏包注入」等**外部确定性手法**替代，**对生产代码零丢包钩子侵入**（仅 1 行测试友元）。
