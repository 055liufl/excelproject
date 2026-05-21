# ADR 0004: 显式 temporal 物理类型（`type` 字段 + `excel`/`db` 子对象）

**状态**: 已采纳

**背景**

ADR 0001 引入的旧形态（`excelFormat`/`dbFormat`）隐含 DB 侧存储格式为字符串。随着 SQLite `INTEGER` 列（Unix epoch seconds）场景的引入，需要在 Profile 中声明"DB 不是字符串，而是整数"。

**决策**

1. **新增 `excel`/`db` 子对象形态**：每个 slot（`dateFormat`/`datetimeFormat`/`timeFormat`）的 `excel` 和 `db` 字段各自为独立的 `TemporalSideSpec` 子对象，含 `type`、`format`、`fallback`。

2. **`type` 字段**：枚举 `"string"`（默认）或 `"epochSec"`（仅 `datetimeFormat.db` 可用）。`string` 类型行为与旧形态完全一致；`epochSec` 类型导入时输出 `qlonglong`，导出时调 `QDateTime::fromSecsSinceEpoch`。

3. **Side 级整体覆盖**：列级声明 `excel` 或 `db` 子对象时，整体替换对应 side 的 profile 默认值；未声明的 side 继承 profile。此策略避免"列声明 type=epochSec 却意外继承 profile 的 format 字符串"的矛盾有效值问题。

4. **旧形态向后兼容**：`excelFormat`/`dbFormat`/`excelFormatFallback` 仍有效，Loader 自动正规化为新形态（type=string，仅含 key 存在的 side 被声明）。新旧形态不可在同一 slot 对象内共存。

**关键权衡**

| 选项 | 优点 | 缺点 |
|---|---|---|
| 字段级合并（旧行为扩展） | 向后兼容性好 | 列声明 epochSec 而继承 format 时有效值矛盾 |
| Side 级整体覆盖（已采纳） | 语义明确，有效值总是可用 | 列只声明一个 side 时另一 side 需显式从 profile 继承，规则需文档化 |
| Slot 级整体覆盖 | 最简单 | 列声明 db 时必须同时写 excel，冗余大 |

**epochSec 限制为 datetimeFormat.db 的原因**

- `QDate` 和 `QTime` 无法提供完整的 POSIX 时间戳语义（epoch 需要年月日时分秒）。
- Excel 侧写整数会破坏 `ExcelReader` 的日期单元格识别路径，易引发混乱；Excel 端始终以字符串与用户交互更直观。
- v1 限制到 `datetimeFormat.db` 以控制复杂度；后续可独立扩展 `dateFormat.db`（整天 epoch）。

**交叉引用**: 参见 [ADR 0001](0001-time-format-in-profile.md) 了解原始设计背景。
