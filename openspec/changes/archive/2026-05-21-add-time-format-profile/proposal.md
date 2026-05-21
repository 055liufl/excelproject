## Why

时间字段当前在导入/导出两个方向上各自为政：导入侧只有一个 per-column 校验器 `date:yyyy-MM-dd`，把字符串解析成 `QDate`，但**没有**任何东西规定写到 DB 时的字符串格式；导出侧根本不格式化，`QVariant` 原样写到 Excel。结果是：

- 一次导入往返之后，Excel 单元格里看到的字符串可能跟原文件不一样（取决于 Qt / xlsx 库的默认行为）；
- DB 里时间列存储格式不可预测，跨 profile / 跨人协作时无法约束；
- 只有 `QDate`，没有 `QDateTime` / `QTime` 的入口；
- 同一来源的 Excel 多种日期写法（如 `yyyy-MM-dd` 与 `yyyy/M/d`）无法在不分裂 profile 的情况下处理。

需要让 profile 显式声明"Excel 怎么显示 (U)"和"DB 怎么存 (V)"两个方向的格式，把往返语义写死。

## What Changes

- 在 `profile.json` 新增**全局默认**三段格式：`dateFormat`、`datetimeFormat`、`timeFormat`，每段含 `excelFormat` (U) 与 `dbFormat` (V) 两个字段；
- 在 `route.columns[*]` 上允许**per-column 覆盖**同样的三段格式（同名字段），未声明者继承全局默认；
- import 路径：
  - Excel 字符串单元格按 U 解析为 `QDate`/`QDateTime`/`QTime`；解析失败 → 行级错误 `E_TIME_PARSE`，行被跳过；
  - 同一列可声明 `excelFormatFallback: [...]`（仅 import 侧），列表内格式按序尝试；
  - 写 DB 时先把结构化值按 V 序列化为字符串再 bind；
  - Excel **原生**日期单元格（`QVariant::type() == Date / DateTime`）**旁路 U**，直接走 V 序列化；
- export 路径：
  - 从 DB 读到的字符串按 V 解析回 `QDate`/`QDateTime`/`QTime`；解析失败 → 行级错误 `E_TIME_PARSE_DB`，该单元格写 NULL，**继续整行**输出；
  - 再按 U 序列化为字符串写入 Excel；
- 与现有 `validators: ["date:yyyy-MM-dd"]` 的关系：保留**向后兼容**——若一列同时声明了新字段，新字段优先；validator 单独存在时按 U=fmt、V=ISO `yyyy-MM-dd` 的隐式默认运行；
- profile 加载期：若 `exportSpec.orderBy` 命中某列且该列的 V 不满足"字典序 == 时间序"（启发式：不以 `yyyy` 打头），**warning**（非错误）。

## Capabilities

### New Capabilities
- `time-format`: profile 中三种时间类型（date / datetime / time）的双向格式声明、双向 I/O 转换、原生日期单元格旁路、import 多格式回退、orderBy 字典序 warning，以及与既有 `date:fmt` validator 的兼容规则。

### Modified Capabilities
（无——`row-lookup` 与 `fk-injection` 不涉及时间字段语义；validator 的扩展属于实现层兼容，不改它们的 spec 行为。）

## Impact

- **代码**：
  - `src/profile/ProfileSpec.h`：新增时间格式结构体 + ColumnSpec 字段；
  - `src/profile/ProfileLoader.cpp`：解析 `dateFormat` / `datetimeFormat` / `timeFormat` 与 per-column 覆盖、`excelFormatFallback`；warning 输出；
  - `src/profile/ProfileValidator.cpp`：V 字典序启发式检查；
  - `src/excel/ExcelReader.*`：识别原生日期单元格并旁路 U；字符串单元格按 U 解析（含 fallback）；
  - `src/excel/ExcelWriter.*`：写入前按 U 序列化时间值；
  - `src/sql/SqlBuilder.cpp` 或 `src/service/ImportService.cpp` / `ExportService.cpp`：bind 前按 V 序列化、读出后按 V 解析；
  - `src/validation/Validators.cpp`：`makeDate` 与新机制并存，文档化优先级；
  - 新增错误码 `E_TIME_PARSE`、`E_TIME_PARSE_DB` 到 `dbridge/Errors.h`。
- **profile 文件**：向后兼容，不需要迁移已有 profile；不声明新字段时行为与今日一致（仅 validator 解析）。
- **测试**：新增 profile 解析测试（含 warning 路径）、import 多格式 fallback 测试、export 反向序列化测试、原生日期单元格旁路测试。
- **依赖**：无新增第三方依赖。
