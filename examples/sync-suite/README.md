# sync-suite —《数据库同步设计2》两场景完整演示（含 GUI）

本 demo 在**一个** Qt Widgets 程序中完整实现设计图 `specs/数据库同步设计2.png`
的两个场景，**不做任何简化**，全部走真实的 dbridge 同步子系统
（`ISyncEngine` / `IComparisonSession` / UDP 文件传输层）。

主窗口用 `QTabWidget` 提供两个标签页：

| 标签页 | 对应设计图 | 关键技术 |
|--------|-----------|----------|
| 场景1 · 多节点指定库同步 | 上半部分拓扑 + 场景1 文字 | `ISyncEngine` + UDP 传输 + SourceWins 冲突仲裁 |
| 场景2 · 差异比对与列级同步 | 场景2 表格 + 双栏对比图 | `IComparisonSession`（DiffEngine + StagingBuffer） |

---

## 场景1：中心节点A ⇄ 子节点B/C/D + 指定数据库

```
        ┌──────────────┐
        │   指定数据库   │  （权威源，以其数据为准）
        └───────┬──────┘
                │ 连接 / 导入
        ┌───────▼──────┐
   ┌────┤   中心节点A    ├────┐
   │    └───────┬──────┘    │   （双向同步）
   ▼            ▼           ▼
┌──────┐   ┌──────┐    ┌──────┐
│子节点B│   │子节点C│    │子节点D│
└──────┘   └──────┘    └──────┘
```

四节点同属一个域，各持一个 SQLite 库，通过 UDP 文件传输层互联
（`center:15101 ⇄ B:15102 / C:15103 / D:15104`）。`Scenario1Runner`
（后台 `QThread`）按四个阶段编排，界面实时刷新日志、阶段与收敛网格：

1. **指定库基线下发** —— 中心A 连接指定数据库导入全部权威数据 → 广播 →
   B/C/D 从空白追平到指定库基线。
2. **并发冲突 · 以指定为准** —— 子节点B 离线把张三薪资改小（与指定库冲突的本地误改）；
   指定库随后更新出新权威值；中心A 重连指定库导入新值并广播；按 `rank`（中心 100 最高）
   仲裁，**指定库/中心A 胜出**，覆盖 B 的本地误改。
3. **子节点重连指定库自我纠正** —— 子节点C 离线改错 → 重新连接指定数据库重导入 →
   本地值被指定值覆盖（节点处亦以指定数据为准）→ 上行保持一致。
4. **收敛校验** —— 对比 `指定库 / A / B / C / D`，确认全域收敛（设计图场景1 第 5 点：
   “最终将指定数据库同步到域内所有子节点”）。

> 冲突仲裁、anti-echo、ACK 窗口等全部由 `ISyncEngine` 真实驱动，与
> `examples/sync-demo` 同源；本 demo 在其之上引入“指定数据库”这一权威源语义。

## 场景2：类 Beyond Compare 的差异比对与列级同步

同步前自动比较「子节点B（当前节点 / 本地）」与「中心节点A（远端）」两个 SQLite 库：

- **表对比清单**（`Scenario2Widget`）：逐表显示 `子节点B 表名 | 中心节点A 表名 | 差异属性`，
  **绿=相同 / 红=不同**（差异状态由真实行级 diff 推导，精确可靠）。
- **双击某表 → 字段级对比窗口**（`CompareDetailDialog`，类 Beyond Compare）：
  左栏 = 子节点B，右栏 = 中心节点A，按主键行对齐，差异单元格红色高亮；
  - `→ 采用中心A · 整行`（`acceptRemote`）
  - `→ 采用中心A · 选中字段（列级）`（`stageCell`，**精确到列**）
  - `← 保留本地B`（`acceptLocal`）
  - 已采用的字段以蓝色预览“合并后将写入B 的值”。
- **保存 / 取消**：「保存」把内存中的合并决策经 `SyncWorker` 写回子节点B 数据库
  （A→B 同步，对应设计图场景2 第 4 点）；「取消」放弃内存暂存（`discard`）。

所有“采用”决策先暂存在比对会话的 `StagingBuffer`（内存）中，点「保存」才落库——
与设计图“数据保持在本地内存中…点击保存（同步）写入数据库”完全一致。

---

## 文件结构

| 文件 | 职责 |
|------|------|
| `main.cpp` | 程序入口；设置插件路径；GUI 或 `--selftest` 两条路径 |
| `MainWindow.{h,cpp}` | 主窗口 + 两个标签页 |
| `Scenario1Runner.{h,cpp}` | 场景1 后台编排（真实多节点 UDP 同步） |
| `Scenario1Widget.{h,cpp}` | 场景1 界面（拓扑 + 日志 + 收敛网格） |
| `Scenario2Model.{h,cpp}` | 场景2 后端逻辑（不依赖 QWidget，GUI 与 selftest 共用） |
| `Scenario2Widget.{h,cpp}` | 场景2 表对比清单 + 取消/保存 |
| `CompareDetailDialog.{h,cpp}` | 场景2 双栏字段级对比对话框 |
| `../sync-demo/udp_transport.{h,cpp}` | 复用的 UDP 文件传输层 |

---

## 构建

### qmake（项目主构建路径）

```bash
cd build_qmake_demos          # 已有的 shadow build 目录
qmake ../dbridge.pro
make sub-examples-sync-suite-sync-suite-pro -j$(nproc)
# 产物：build_qmake_demos/examples/sync-suite/sync-suite
```

### CMake

```bash
cmake -S . -B build && cmake --build build --target sync-suite
```

## 运行

> ⚠️ **运行时 Qt 库路径**：本机 shell 的 `LD_LIBRARY_PATH` 指向 QtCreator 自带的
> Qt 5.15.2，会与项目使用的 Qt 5.12.12 冲突（`Cannot mix incompatible Qt library`）。
> GUI 程序在启动时即加载平台插件，必须把 5.12.12 的库与插件路径前置：

```bash
cd build_qmake_demos/examples/sync-suite
export LD_LIBRARY_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/lib:$LD_LIBRARY_PATH
export QT_QPA_PLATFORM_PLUGIN_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/plugins/platforms

# 1) 启动 GUI（默认 workspace=系统临时目录）
./sync-suite

# 2) 无界面自检：headless 跑通两场景核心逻辑（用于编译后运行验证）
QT_QPA_PLATFORM=offscreen ./sync-suite --selftest --ws /tmp/sync-suite-ws
```

`--selftest` 不弹窗，直接驱动两个场景的核心逻辑（与 GUI 完全相同的代码路径），
校验“场景1 全域收敛 / 场景2 比对·列级采用·写回正确”后以退出码 0 返回。
