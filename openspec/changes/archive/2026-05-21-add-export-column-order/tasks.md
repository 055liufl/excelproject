## 1. Profile data model

- [x] 1.1 在 `src/profile/ProfileSpec.h` 的 `ExportSpec` 增加 `QStringList columnOrder;`
- [x] 1.2 维持 `orderBy / explicitSql / classColumn` 三字段不变（spec 已明确正交 / 互斥关系）

## 2. Profile loading

- [x] 2.1 在 `src/profile/ProfileLoader.cpp` 解析 `exportSpec.columnOrder`：必须是数组，每个元素是非空字符串
- [x] 2.2 非数组（如字符串或对象）→ 加载期报错并指出形状不匹配

## 3. Profile validation

- [x] 3.1 在 `src/profile/ProfileValidator.cpp` 构造"全部可接受 header 集合"：
  - 遍历 profile.routes（SingleTable/MultiTable）或 profile.classes[*].routes（Mixed）的每个 `ColumnSpec.source`；
  - Mixed 模式下若 `exportSpec.classColumn` 非空也加入此集合
- [x] 3.2 校验 `columnOrder` 每个元素都在集合中，否则 `E_EXPORT_UNKNOWN_HEADER`，错误消息附"至少 5 个已知 header"
- [x] 3.3 校验 `columnOrder` 无重复元素，否则 `E_EXPORT_DUPLICATE_ORDER`
- [x] 3.4 校验若 `explicitSql` 非空且 `columnOrder` 非空 → `E_EXPORT_ORDER_WITH_RAW_SQL`
- [x] 3.5 添加新错误码常量到 `include/dbridge/Errors.h`：`E_EXPORT_UNKNOWN_HEADER`、`E_EXPORT_DUPLICATE_ORDER`、`E_EXPORT_ORDER_WITH_RAW_SQL`

## 4. Export ordering core helper

- [x] 4.1 在 ExportService 内（或新建一个内部 helper 命名空间）添加：
  ```
  QStringList reorderHeaders(const QStringList& natural,
                              const QStringList& columnOrder);
  ```
  实现"列入头部按声明顺序、未列入按 natural 顺序追加"的纯函数。
- [x] 4.2 helper 必须单元测试覆盖（无 SQL / 无 DB，纯字符串变换）

## 5. ExportService — SingleTable / MultiTable 分支

- [x] 5.1 在 `src/service/ExportService.cpp` 的"否则分支"（非 Mixed）里检查 `profile.exportSpec.columnOrder.isEmpty()`：
  - 为空 → 维持当前流式 `execAndWrite` 路径（零开销）；
  - 非空 → 走新路径
- [x] 5.2 新路径：执行 SQL；获取 `record()` 与所有行（全量加载到 `QVector<QVector<QVariant>> rows`）
- [x] 5.3 用 `reorderHeaders(naturalHeaders, columnOrder)` 计算 `finalHeaders` 与索引重排（用 `QHash<QString,int>` 把 finalHeaders 中每个 header 映射到 natural 索引）
- [x] 5.4 `writer.writeHeader(finalHeaders)`，逐行按重排索引取值写出

## 6. ExportService — Mixed 分支

- [x] 6.1 在 Mixed 分支的"`allHeaders` 构造结束、`classColumn` 处理之前"插入"对 `allHeaders` 重排"步骤
- [x] 6.2 处理 `classColumn` 的两条规则：
  - `classColumn` 非空且**不在** `columnOrder` 中 → 走今日 `prepend` 路径（在重排后的 finalHeaders 最前补上）
  - `classColumn` 在 `columnOrder` 中 → finalHeaders 直接来自重排结果（已含正确位置），不再 prepend
- [x] 6.3 行投影逻辑维持现状（按 finalHeaders 名字查 `mr.data` / 注入 classId），仅替换之前的 `allHeaders` 引用

## 7. Tests

- [x] 7.1 helper 纯函数测试：完全重排、部分重排、空 columnOrder、columnOrder 含全部 / 超集（超集应在 validator 层拦截，此处保证 helper 自身鲁棒）
- [x] 7.2 ProfileValidator 测试：未知 header、重复、`explicitSql + columnOrder` 同时声明
- [x] 7.3 SingleTable 集成：`columnOrder` 重排后导出列顺序符合声明；未列入列按 SQL 顺序追加
- [x] 7.4 MultiTable 集成：跨表列名（route.column 全展开）的重排正确
- [x] 7.5 Mixed 集成：
  - 默认 classColumn 仍为首列；
  - classColumn 出现在 `columnOrder` 中按显式位置；
  - 跨 class 未贡献列在该 class 行的相应位置写空
- [x] 7.6 与 `orderBy` 共存：`orderBy` 与 `columnOrder` 完全独立、可同时声明
- [x] 7.7 流式回归：未声明 `columnOrder` 时，行为与今日完全一致（既有导出测试集全部通过）
- [x] 7.8 添加一个示例 profile 至 `tests/data/profiles/`，CLI 烟测可见到列顺序生效

## 8. Docs

- [x] 8.1 README.md / FAQ 增补一节"导出列顺序"，覆盖：
  - 声明语法与示例
  - 未列入列追加规则
  - Mixed 模式下 classColumn 的位置规则
  - 与 `explicitSql` 互斥、与 `orderBy` 正交
  - 未声明时维持现有行为，存量 profile 零迁移

## 9. Wrap-up

- [x] 9.1 跑全量测试套件 + 既有 examples 烟测
- [x] 9.2 `openspec validate add-export-column-order`
- [x] 9.3 ADR 摘要进 `docs/adr/`：记录"列顺序声明放在 exportSpec、用 Excel header 命名、与 SqlBuilder 解耦"的决策
