# ADR 0001: 时间字段格式声明在 Profile 中（profile + per-column 覆盖）

**状态**: 已采纳

**决策**: 在 `ProfileSpec` 顶层新增 `dateFormat` / `datetimeFormat` / `timeFormat` 三个独立 slot，每个 slot 含 `excelFormat`（U）、`dbFormat`（V）、可选 `excelFormatFallback`；允许 `ColumnSpec` 同名字段做字段级覆盖，未声明字段继承全局值。

**理由**: SQLite 没有原生日期类型，DB 存储格式是约定不是自动推断；把格式声明放在 profile 使 Excel ↔ DB 的 I/O 行为完全由配置决定，不依赖代码硬编码。per-column 覆盖与全局默认分两层，匹配现有 `validators` 颗粒度。

**影响**: 仅 `TemporalFormatSpec.declared == true` 时激活 temporal 转换层，legacy `date:fmt` validator 行为不变。

**后续扩展**: [ADR 0004](0004-explicit-temporal-type.md) 在本决策基础上引入显式 `type` 字段（`"string"` / `"epochSec"`）和 `excel`/`db` 子对象形态，以支持 SQLite `INTEGER` 列的 Unix epoch seconds 存储场景，同时将覆盖语义从字段级合并升级为 side 级整体覆盖。
