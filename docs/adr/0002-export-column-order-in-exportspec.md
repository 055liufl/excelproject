# ADR 0002: 导出列顺序声明放在 exportSpec，用 Excel header 命名

**状态：** 已采纳  
**日期：** 2026-05-20

## 背景

用户希望在导出 Excel 时能控制列的输出顺序，而不是完全依赖 SQL 的 SELECT 列序。

## 决策

在 `ProfileSpec.ExportSpec` 中增加 `QStringList columnOrder`，每个元素为 Excel header 名（`ColumnSpec.source`）。

## 关键选择与权衡

### 用 Excel header 名（source）而非 dbColumn 名

Excel 端可见的是 `source`（表头文字），`dbColumn` 是内部实现细节。用户理解和维护 profile 时更自然。`dbColumn` 与 `source` 通常不同（如下划线命名 vs 中文表头），以 `source` 命名避免混淆。

### 放在 exportSpec 而非 routes/columns 层

列顺序是"整张 sheet 的展示偏好"，不属于任何单个路由或列的映射规则。放在 `exportSpec` 与 `orderBy` 对称，语义清晰。

### 与 SqlBuilder 完全解耦

`columnOrder` 不改变 SQL 生成逻辑。SQL 仍按 routes 的自然顺序 SELECT；`ExportService` 在内存层做列重排。这样 `SqlBuilder` 和 `Mapper` 零改动，风险最低。

### 空 `columnOrder` = 维持现状（零迁移）

不声明时 ExportService 走原有流式路径，存量 profile 行为完全不变。

### 与 `explicitSql` 互斥

`explicitSql` 的列名由 SQL 字符串控制，无法在 validator 层提前知道列集合，故两者同时声明无意义，报 `E_EXPORT_ORDER_WITH_RAW_SQL`。

### Mixed 模式 classColumn 位置规则

- `classColumn` 不在 `columnOrder` 中 → prepend（旧行为兼容）  
- `classColumn` 在 `columnOrder` 中 → 按声明位置，不 prepend

## 被否决的方案

- **在 routes 层逐列声明排序序号**：侵入性强，跨 route 排序语义复杂。
- **在 SQL 层用 AS 别名控制**：需要用户写 SQL，失去 profile 驱动的抽象。
