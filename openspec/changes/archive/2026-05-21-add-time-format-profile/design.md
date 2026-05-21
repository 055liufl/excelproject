## Context

当前的时间字段处理只在导入侧的"列校验器"层有一个 `date:fmt` token（`src/validation/Validators.cpp:126`）。它做两件事：把 Excel 字符串解析成 `QDate`，以及对已是 `QDate`/`QDateTime` 的值直接放行。然后链路下游 (`Mapper` → `SqlBuilder` → `QSqlQuery::bindValue`) 把这个 `QVariant` 交给 Qt SQL 驱动；最终在 SQLite 里以什么形式落盘，取决于 Qt 把 `QDate` 转成什么字符串——profile 无法控制。

导出侧 (`src/service/ExportService.cpp`) 直接把 `QSqlQuery::value(i)` 的 `QVariant` 塞进 `ExcelWriter::writeRow`，再交给 xlsx 库写出。同样没有任何格式化层。

profile 解析（`src/profile/ProfileLoader.cpp`）当前没有任何 profile 级或 column 级的时间格式声明字段。

由于 SQLite 没有原生 datetime 类型（都用 TEXT/INTEGER affinity 模拟），"DB 存储格式"实际上就是一个**约定**——必须由 profile 显式声明，否则跨人协作会漂移。

## Goals / Non-Goals

**Goals:**
- 让 profile 显式声明 Excel 显示格式 U 与 DB 存储格式 V，两端 I/O 行为完全由 profile 决定；
- 支持 `date` / `datetime` / `time` 三种类型独立配置；
- 与既有 `date:fmt` validator 共存且优先级清晰；
- 失败语义与现有 row-lookup / fk-injection 一致——行级错误、表级继续；
- Excel 原生日期单元格的行为可预测（旁路 U）。

**Non-Goals:**
- 不引入时区（time zone）概念——Qt 的 `QDate`/`QTime` 无时区，`QDateTime` 默认 local，本变更不主动处理 UTC 转换；
- 不做"自动猜测格式"——除非用户显式声明 `excelFormatFallback`，否则严格按声明走；
- 不替换或废弃 `date:fmt` validator（向后兼容，渐进迁移）；
- 不引入新的第三方时间库；
- 不在本变更里改动 `orderBy` 的运行时行为——只在加载期发 warning 提示用户。

## Decisions

### D1: 新增三个独立 slot（`date` / `datetime` / `time`），而不是合一

**选**：profile 级 `dateFormat`、`datetimeFormat`、`timeFormat` 三个独立对象，每个含 `{ excelFormat, dbFormat, excelFormatFallback? }`。

**为什么**：Qt 类型层面就是 `QDate` / `QDateTime` / `QTime` 三种独立类型；强行合并意味着每次都要解析时判断"用户究竟想要哪一种"。三个 slot 各自配置，零歧义。

**备选**：单个 `temporalFormat` + per-column `type: date|datetime|time`。比目前方案省一行 profile，但代价是 column 级声明必填——比当下"validator 隐式定型"还要重。否决。

### D2: profile 级默认 + per-column 覆盖

**选**：在 `ProfileSpec` 顶层放三个全局 slot，并在 `ColumnSpec` 上允许同名字段覆盖。column 级未声明字段时**字段级合并**继承全局值（不是整段替换）。

**为什么**：跟现有 `validators` 的颗粒度匹配；多数 profile 全局一种格式就够，少数列例外（如 ISO date 与人类可读 datetime 混存）。

**字段级合并示例**：profile 全局 `dateFormat: { excelFormat: "yyyy-MM-dd", dbFormat: "yyyy-MM-dd" }`，column 仅写 `dateFormat: { excelFormat: "yyyy/M/d" }` → 实际生效 `excelFormat: "yyyy/M/d"`, `dbFormat: "yyyy-MM-dd"`（继承全局）。

### D3: 与 `date:fmt` validator 的兼容规则

**选**：
1. 如果一列同时声明了 `dateFormat` 与 `validators: ["date:fmt"]`，**新字段优先**，validator 退化为"已经是 QDate 的话直接通过"的兜底（即跳过 fromString）；
2. 如果只声明 `validators: ["date:fmt"]`，沿用今日行为：U = fmt，V 退化为 `yyyy-MM-dd`（ISO，硬编码隐式默认）；
3. 如果只声明 `dateFormat`（无 validator），系统**自动**等价于挂了一个 `date:U` validator 用于 schema-aware 类型判定（保证 dbColumn affinity 校验等下游能识别这是时间列）；
4. `datetimeFormat` / `timeFormat` 没有对应 validator，无兼容包袱。

**为什么**：现存测试与 `tests/data/profiles/*.json` 大量使用 validator 方式，硬切会破存量。文档化优先级即可。

### D4: Excel 原生日期单元格旁路 U

**选**：在 `ExcelReader` 读取单元格时，若 `QVariant::type()` 已是 `Date` / `DateTime`（即 xlsx 库识别为数值型日期），**不**用 U 走 `fromString`，直接拿这个结构化值进入 V 序列化路径。

**为什么**：U 在这种情况下无意义（拿到的不是字符串）；且强行 `toString().fromString()` 是二次损耗。Validators.cpp:133 已有同样的旁路逻辑（"already QDate or QDateTime, pass through"），沿用心智。

**注意**：`QTime` 也要旁路（Excel 时间类型 `QVariant::Time`）。

### D5: 失败处理 —— 行级错误，表级继续

**选**：
- import 路径：U 解析失败 → `E_TIME_PARSE` 行级错误，行被跳过（与现有 validator 失败语义一致）；V 序列化失败（极少见，比如内部 `QDate` 无效）→ 同 `E_TIME_PARSE`；
- export 路径：V 解析失败 → `E_TIME_PARSE_DB` 行级错误，**该单元格写 NULL，整行继续输出**（导出侧"宽容"原则，避免一条脏数据拖垮整张表）；
- import 侧支持 `excelFormatFallback: [...]`，按声明顺序依次尝试，全部失败才报 `E_TIME_PARSE`；fallback 仅 import 侧、仅 Excel 字符串单元格生效（原生日期单元格本就旁路 U）。

**为什么**：与 row-lookup 的"row-level error / sheet-level continue"一脉相承；export 侧比 import 侧更宽容是惯例（导出数据可能历经多年/多人写入，要假设有脏数据）。

### D6: orderBy + 非字典序 V 的 warning

**选**：在 `ProfileValidator` 加载期，若 `exportSpec.orderBy` 命中某 dbColumn，且该列的 V `dbFormat` 不以 `yyyy` 起首（启发式：以年开头的格式才有字典序==时间序的保证），**发 warning 但不拒绝加载**。warning 通过 `ErrorCollector` 以新错误码 `W_TIME_ORDERBY_NONSORTABLE` 表达（区分 error 与 warning 的具体机制由 ErrorCollector 现有约定决定；如果当前 ErrorCollector 无 warning 通道，则用 stderr 打印并在 ProfileValidator 返回值附带 warning 列表）。

**为什么**：硬拒绝会把"orderBy 命中但格式不合规"的合法 profile 拒掉；但完全静默会让用户在生产里才发现"为什么导出顺序奇怪"。warning 是合适的妥协。

### D7: 序列化/反序列化的实现位置

**选**：
- **解析（U → 结构化值）**：放在 `Mapper`（或紧随 `ExcelReader` 之后的一层）。同一处可以照顾 fallback、原生单元格旁路；
- **DB 写入序列化（结构化值 → V 字符串）**：在 `SqlBuilder` 或 `ImportService` 调用 `bindValue` 之前，把 `QDate`/`QDateTime`/`QTime` 替换为 `QString`；
- **DB 读出反序列化（V 字符串 → 结构化值）**：在 `ExportService` 执行 query 后、写 row 之前的中间层；
- **写 Excel 序列化（结构化值 → U 字符串）**：在写 row 之前，由 `ExportService` 或 `ExcelWriter` 的薄包装层。

挂哪一层视实现便利度，最终决定写到 `tasks.md`；spec 不绑定具体位置。

## Risks / Trade-offs

- **[Risk] V 选错导致存量数据全废**
  → Mitigation：变更默认行为只在 profile 显式声明新字段时启用；未声明时维持今日行为。文档明确建议 V 用 ISO（`yyyy-MM-dd`、`yyyy-MM-dd HH:mm:ss`、`HH:mm:ss`）。

- **[Risk] `date:fmt` 与 `dateFormat` 双写的歧义**
  → Mitigation：D3 已定优先级；ProfileValidator 在两者同时存在时输出 info-level 提示（"使用 dateFormat，已忽略 validator 的 date:fmt"）。

- **[Risk] 用户 V 写了 `yyyy-MM-dd HH:mm:ss` 但 column 实际声明的是 `dateFormat`（无 HH:mm）**
  → Mitigation：profile 加载期严格校验：U 与 V 必须能容纳对应类型的所有字段——`dateFormat` 的 U/V 不允许含 `H/m/s` token；`timeFormat` 的 U/V 不允许含 `y/M/d` token。`datetimeFormat` 不做约束。违规则加载期 error。

- **[Risk] Excel 原生日期单元格被某些 xlsx 写法识别成数值而非 Date 类型**
  → Mitigation：本变更不试图覆盖所有 xlsx 兼容性问题；用户遇到时把那一列声明为字符串处理（不写 `dateFormat`，沿用 validator + U）即可。`E_TIME_PARSE` 的报错消息中包含"原始 QVariant::type()"以便排查。

- **[Trade-off] 不引入时区**
  → 当下用户场景全是本地业务时间，强加时区会引入难以撤销的语义负担。如未来需要，独立 change 再加 `timeZone` 字段。

- **[Trade-off] export 侧不做 fallback**
  → DB 是自己写入的，本应一致；如果出现历史脏数据，希望用户**看到**这件事（行级错误 + NULL 写出），而不是被静默修复。

## Migration Plan

不破坏现有 profile：

1. 现有 profile（仅有 `validators: ["date:fmt"]`，无新字段）→ 沿用今日行为（U=fmt, V 隐式 `yyyy-MM-dd`）；
2. 用户主动迁移：把 `validators: ["date:fmt"]` 换成 `dateFormat: { excelFormat: "fmt", dbFormat: "yyyy-MM-dd" }`，行为等价但更显式；
3. 想换 V 格式的：在 `dateFormat.dbFormat` 写新格式，**必须**配套数据迁移脚本——本变更不提供数据迁移工具，由用户决定（spec 中要写明）。

回滚策略：本变更纯增量，删掉 profile 中的新字段即可回到 0 影响状态。

## Open Questions

1. `ErrorCollector` 当前是否已经支持 warning 等级？如果没有，是临时打 stderr 还是借这次扩展？— 倾向扩展 ErrorCollector，但需要看实现复杂度，归到 tasks.md 评估。
2. `excelFormatFallback` 是否允许出现在 profile **全局**层（不只是 per-column）？— 倾向只 per-column 起作用，因为 fallback 列表通常很列特化；profile 全局做 fallback 容易写出"任何列我都猜"的滥用 profile。最终决定写到 spec。
