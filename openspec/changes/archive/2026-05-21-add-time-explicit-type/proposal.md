## Why

当前 `time-format` 能力假设两侧（Excel 与 SQLite）都以**字符串**形态存储时间，由 `excelFormat` / `dbFormat` 两个 Qt 格式串描述。但真实业务场景中，SQLite 列经常直接存 **Unix epoch 秒数**（INTEGER 列，1970-01-01 起的秒数），dbridge 目前无法读写这种 DB schema —— 导入时 epoch 整数被强行 `toString()` 后用日期格式字符串解析必然失败（`E_TIME_PARSE`），导出时同样失败（`E_TIME_PARSE_DB`）。

用户希望在 Profile.json 里**为每一个时间列独立显式声明**两侧的"类型 + 格式"，而不是只声明格式串。这既解决当下的 epoch 秒数兼容问题，也建立起未来扩展（epochMs / nativeDate / iso8601 等）的统一入口。

## What Changes

最小可落地范围（v1）。所有规则均为 spec 级行为约束，影响 ProfileLoader / ProfileValidator / TemporalConvert / Mapper / ExportService。

### 新 JSON schema：`excel` / `db` 子对象

每个时间 slot（`dateFormat` / `datetimeFormat` / `timeFormat`）支持 `excel` 与 `db` 两个子对象，每个子对象结构为 `{ type, format?, fallback? }`：

```jsonc
"datetimeFormat": {
  "excel": { "type": "string",   "format": "yyyy-MM-dd HH:mm:ss" },
  "db":    { "type": "epochSec" }
}
```

### type 取值（v1）

| 值 | excel 侧 | db 侧 | 适用 slot |
|---|---|---|---|
| `"string"`（默认） | ✓ | ✓ | 全部三种 slot |
| `"epochSec"` | ✗（v1 不支持） | ✓ | 仅 `datetimeFormat` |

非法组合（如 `dateFormat.db.type=epochSec`、`excel.type=epochSec`）→ 加载期 `E_PROFILE_PARSE`。`type` 缺省时**视为 `"string"`**。

### 合并语义：按 side 整体覆盖

列级若声明 `excel` 子对象，**整体替换** profile 级 `excel`（type / format / fallback 一并替换）；`db` 同理。列级未声明的 side 完全继承 profile 级。

> **不做字段级合并**。这是为了消除 "列级 type=epochSec + profile 级 format='yyyy-MM-dd'" 经字段级继承后产生 `{type:epochSec, format:'yyyy-MM-dd'}` 非法 effective spec 的死结。用户想 "只换 type 不换 format" 时必须重抄一份 format —— 接受这个代价以换取合并规则的可解性。

### "format / fallback 为空" 的定义

| JSON 写法 | 处理 |
|---|---|
| 字段缺省 / `""` 空字符串 / `[]` 空数组 | 视为空 |
| `null` | `E_PROFILE_PARSE`（明确报错，避免变量未填值的 bug 静默通过） |

### type × format 一致性校验

| type | format 要求 | 违反时 |
|---|---|---|
| `string` | effective `format` 必须非空 | `E_PROFILE_PARSE`（列名 + side） |
| `epochSec` | effective `format` 必须为空 | `E_PROFILE_PARSE` |

校验时机：**effective spec 计算之后**（不是单点 JSON 解析时）。意味着 Loader 必须先完成 profile 级 → 列级合并，再逐列断言。

### 新旧形态共存边界

- **同一个 slot JSON object 内**同时出现旧字段（`excelFormat` / `dbFormat` / `excelFormatFallback` 任一）与新子对象（`excel` / `db` 任一） → `E_PROFILE_PARSE`。
- **profile 级用旧形态、列级用新形态允许**。Loader 在解析时先把 profile 级旧形态正规化为新形态（`excelFormat:"..."` → `excel:{type:"string", format:"..."}`），再做按 side 整体覆盖。
- 旧形态 `excelFormat` / `dbFormat` / `excelFormatFallback` **永久保留**为新形态的简写。

### 空对象与默认值

| 写法 | 语义 |
|---|---|
| `"datetimeFormat": {}` | slot declared，excel/db 均未声明 → 两侧均继承 profile 级 |
| `"datetimeFormat": {"excel": {}}` | excel side declared，`type` 缺省 = `"string"`，format/fallback 缺省 |
| `"datetimeFormat": {"excel": {}, "db": {}}` | 两侧均 declared，均 `type = "string"`，format 缺省 |

`time_formats.json` 现有的 `"timeFormat": {}` 哑声明行为保留。

### 列级多 slot 仍然非法

A column SHALL declare at most one of `dateFormat` / `datetimeFormat` / `timeFormat`。声明多个 → `E_PROFILE_PARSE`。**本 change 在 Loader 补上显式校验**（现有代码注释提到这条规则但未强制）。

### NULL 与 0 的语义区分

- DB 列值为 SQL NULL（`QVariant().isNull()` 为 true）→ 导出 SHALL 写空 Excel 单元格，**跳过 temporal 转换**。
- DB 列值为 `qlonglong(0)` 且 `db.type=epochSec` → 转换为 `1970-01-01T00:00:00`，**不视为空**。

### W_TIME_ORDERBY_NONSORTABLE 措辞调整

旧：dbFormat 不以 `yyyy` 开头时发警告。
**新：当 effective `db.type ≠ "string"` 时一律跳过该警告**（INTEGER 列 SQLite 天然按数值序排），仅在 `db.type == "string"` 时检查 format 字典序条件。**用反向定义而非枚举具体值**，使未来加 epochMs / nativeDate 时不必回头改这条规则。

### 内部 API 升级（实现层关键约束）

- `TemporalConvert::formatValue` 返回类型 `QString` → `QVariant`。`string` 路径返回 `QVariant(QString)`，`epochSec` 路径返回 `QVariant(qlonglong)`，**失败统一返回 `QVariant()`（invalid）**。
- Mapper / ExportService 由 `QString::isEmpty()` 判断改为 `QVariant::isValid() && !isNull()`。
- 该重构对 Profile JSON 用户无感知，但**会破坏现有 `tst_temporal_*` 单元测试**，需同步适配。

### v1 明确不变更的能力

- **AutoProfileBuilder**：永远生成旧形态（`excelFormat` / `dbFormat`），不自动推断 `epochSec`（SQLite 元数据无法区分 epoch 整数与普通整数；启发式不可靠）。
- **lookup × temporal**：反查 A 列若声明 temporal slot 继续走现有 `convertTemporalForExport` 路径，新 `epochSec` 类型自动适配，**本 change 不新增分支**。

### 显式不在 v1 范围（保留未来扩展位）

- `excel.type=epochSec` —— Excel 端 epoch 数字单元格读写
- `epochMs` —— 毫秒精度
- `nativeDate` —— Excel 原生日期单元格的显式声明（当前已有运行时嗅探，无功能缺口）
- `dateFormat.db.type=epochSec` / `timeFormat.db.type=epochSec` —— epoch 仅在 datetime 语义下无歧义

## Capabilities

### New Capabilities

（无）

### Modified Capabilities

- `time-format`: 引入 `excel` / `db` 子对象与显式 `type` 字段；为 `db` 侧增加 `epochSec` 物理类型；定义合并语义（按 side 整体覆盖）、新旧形态共存规则、空值定义、type×format 一致性校验、NULL/0 边界、orderBy 警告范围、列级多 slot 显式禁止。

## Impact

**Profile JSON schema**：新增可选字段，向后兼容；旧 Profile 不修改即可继续工作。

**代码**：

- `src/profile/ProfileSpec.h`：`TemporalFormatSpec` 扩展为 `{ declared, excel: TemporalSideSpec, db: TemporalSideSpec }`，新增 `TemporalSideSpec { type, format, fallback }`；`effectiveTemporalFor` 改为按 side 整体覆盖。
- `src/profile/ProfileLoader.cpp`：识别新子对象 + 旧平铺形态；同 slot 内混用立即报错；profile 级旧形态正规化为新形态；列级多 slot 显式校验；effective spec 计算后做 type×format 一致性校验。
- `src/profile/ProfileValidator.cpp`：`W_TIME_ORDERBY_NONSORTABLE` 跳过 `db.type ≠ string`；slot kind × db.type 合法性矩阵校验。
- `src/mapping/TemporalConvert.{h,cpp}`：`formatValue` 返回 `QVariant`，失败统一 `QVariant()`；新增按 `TemporalSideSpec.type` 分派的解析/序列化路径（`string` / `epochSec`）。
- `src/mapping/Mapper.cpp` / `src/service/ExportService.cpp`：适配 `QVariant` 返回值；epoch 路径 SQL bind 走 `qlonglong`；NULL 与 0 的语义区分。

**错误码**：复用现有 `E_PROFILE_PARSE` / `E_TIME_PARSE` / `E_TIME_PARSE_DB` / `W_TIME_ORDERBY_NONSORTABLE`，无新增。

**测试夹具**：

- `tests/data/sql/08_epoch_time.sql`：`event(happen_at INTEGER)` 表。
- `tests/data/profiles/epoch_time.json`：声明 `datetimeFormat.db.type=epochSec` 的最小 profile。
- `tests/data/xlsx/EpochEvents.xlsx`：字符串日期输入 Excel。
- `tst_temporal_import` / `tst_temporal_export` 增补 epoch 路径用例；旧用例需适配 `QVariant` 返回类型。
- 新增 Profile 加载校验矩阵测试（3 slot × 3 声明位置 × 2 type 至少 18 case；具体清单见 tasks.md）。

**文档**：

- README §14.7 / Q11 加入 `excel`/`db` 子对象示例与新旧形态对照。
- 新增 `docs/adr/0004-explicit-temporal-type.md` 记录"按 side 整体覆盖"等关键决策的权衡。
- `docs/validation/row-to-multitable.md` 场景 III 扩展为包含 epoch 子场景。

**ABI / API**：`TemporalConvert::formatValue` 返回类型变更属于库内部 API，外部公开头不受影响。
