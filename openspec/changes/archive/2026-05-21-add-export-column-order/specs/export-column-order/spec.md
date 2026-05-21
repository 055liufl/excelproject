## ADDED Requirements

### Requirement: columnOrder declaration

A profile's `exportSpec` SHALL be able to declare an optional `columnOrder` as a JSON array of strings. Each string SHALL be an Excel header name (i.e. matches some route's `ColumnSpec.source`) OR the value of `exportSpec.classColumn` when that field is non-empty. The declaration is optional; when absent, export SHALL use the historical column ordering (SQL projection order for SingleTable/MultiTable; first-appearance order across classes for Mixed).

#### Scenario: columnOrder absent preserves historical behavior
- **WHEN** a profile declares no `columnOrder` (or `columnOrder: []`)
- **THEN** export SHALL produce columns in the order they have always been produced (SingleTable/MultiTable: SqlBuilder projection order; Mixed: first-appearance order across classes), and `classColumn` (if declared) SHALL be prepended as the first column

#### Scenario: columnOrder declared as non-array is rejected
- **WHEN** a profile declares `exportSpec.columnOrder` as a non-array value (e.g. a single string)
- **THEN** profile loading SHALL fail with a message identifying the offending shape

### Requirement: Identifiers are Excel header names

Every entry in `columnOrder` SHALL match either (a) the `source` field of at least one `ColumnSpec` reachable from the profile's active routes, or (b) the value of `exportSpec.classColumn` (only when `classColumn` is declared and non-empty in Mixed mode). Identifiers are compared as case-sensitive exact strings.

Profile validation SHALL reject any entry that matches neither. The error code SHALL be `E_EXPORT_UNKNOWN_HEADER` and the message SHALL include the offending string and a hint listing at least 5 known headers (or all known headers if fewer than 5 exist).

#### Scenario: Unknown header is rejected
- **WHEN** `columnOrder = ["OrderNo", "OrderNoo"]` (a typo) and "OrderNoo" matches no route's ColumnSpec.source and no classColumn
- **THEN** profile loading SHALL fail with code `E_EXPORT_UNKNOWN_HEADER` and the message SHALL contain the string "OrderNoo"

#### Scenario: classColumn-valued entry is accepted
- **WHEN** Mixed mode profile declares `exportSpec.classColumn = "Type"` and `columnOrder` contains `"Type"`
- **THEN** profile loading SHALL succeed and `"Type"` in `columnOrder` SHALL refer to the synthetic class column

#### Scenario: Case sensitivity
- **WHEN** a route's ColumnSpec.source is `"OrderNo"` and `columnOrder` contains `"orderno"`
- **THEN** profile loading SHALL fail with `E_EXPORT_UNKNOWN_HEADER` (case-mismatched)

### Requirement: No duplicates in columnOrder

`columnOrder` SHALL NOT contain duplicate entries. Profile validation SHALL reject any duplicate with `E_EXPORT_DUPLICATE_ORDER` and the message SHALL include the duplicated value.

#### Scenario: Duplicate string in columnOrder
- **WHEN** `columnOrder = ["OrderNo", "Amount", "OrderNo"]`
- **THEN** profile loading SHALL fail with `E_EXPORT_DUPLICATE_ORDER` and the message SHALL contain `"OrderNo"`

### Requirement: Listed headers come first in declared order

Headers listed in `columnOrder` SHALL appear in the export's Excel sheet in the exact order declared, ahead of any unlisted headers.

#### Scenario: Full re-ordering
- **WHEN** the route's natural projection order is `["A","B","C","D"]` and `columnOrder = ["C","A","D","B"]`
- **THEN** the Excel header row SHALL be `C, A, D, B` in that order

#### Scenario: Partial re-ordering with subset
- **WHEN** the natural projection order is `["A","B","C","D"]` and `columnOrder = ["C","A"]`
- **THEN** the Excel header row SHALL be `C, A` followed by the unlisted headers in their natural order (see next requirement)

### Requirement: Unlisted headers append in natural order

Headers reachable from the profile but not present in `columnOrder` SHALL be appended after the listed headers, preserving their natural ordering:

- **SingleTable / MultiTable**: natural order is the SQL projection order produced by `SqlBuilder` (i.e. route-declaration order × ColumnSpec-declaration order within each route).
- **Mixed**: natural order is the first-appearance order across classes (the existing `allHeaders` build order in `ExportService`).

The relative order among unlisted headers SHALL be preserved as a stable suffix of the natural order, with listed headers removed from that natural order to form the suffix.

#### Scenario: Unlisted suffix preserves natural order (SingleTable)
- **WHEN** SqlBuilder would produce projection `["A","B","C","D","E"]` and `columnOrder = ["D","B"]`
- **THEN** the Excel header row SHALL be `D, B, A, C, E`

#### Scenario: Unlisted suffix preserves first-appearance order (Mixed)
- **WHEN** Mixed mode produces `allHeaders = ["A","B","C","D","E"]` after first-appearance merge across classes and `columnOrder = ["E","A"]` and no `classColumn` is declared
- **THEN** the Excel header row SHALL be `E, A, B, C, D`

#### Scenario: All headers listed leaves no suffix
- **WHEN** `columnOrder` contains every reachable header in some order
- **THEN** the Excel header row SHALL be exactly `columnOrder`, with no additional appended columns

### Requirement: classColumn default position vs explicit position

The placement of the synthetic class column SHALL follow either a default prepend rule or an explicit position in `columnOrder`, depending on whether the user listed `classColumn` in `columnOrder`. In Mixed mode with non-empty `exportSpec.classColumn`:

- If `classColumn` is **NOT present** in `columnOrder` (whether `columnOrder` is empty or non-empty), the `classColumn` SHALL be prepended as the first column of the Excel header row (current behavior).
- If `classColumn` **is present** in `columnOrder`, it SHALL be placed at the position dictated by `columnOrder`; the `prepend` default SHALL NOT apply.

In SingleTable / MultiTable modes, `classColumn` has no meaning; its presence in `columnOrder` (which would only be possible if it accidentally matched a ColumnSpec.source — see "Identifiers are Excel header names" requirement) is governed solely by the Excel-header matching rule.

#### Scenario: Mixed with classColumn, columnOrder omits classColumn
- **WHEN** `classColumn = "Type"`, `columnOrder = ["OrderNo","Amount"]`, natural unlisted order is `["LineNo","Sku","Qty"]`
- **THEN** Excel header row SHALL be `Type, OrderNo, Amount, LineNo, Sku, Qty`

#### Scenario: Mixed with classColumn, columnOrder includes classColumn mid-list
- **WHEN** `classColumn = "Type"`, `columnOrder = ["OrderNo","Type","Amount"]`, natural unlisted order is `["LineNo","Sku","Qty"]`
- **THEN** Excel header row SHALL be `OrderNo, Type, Amount, LineNo, Sku, Qty`

#### Scenario: Mixed with classColumn empty
- **WHEN** `classColumn = ""` (or absent) and `columnOrder = ["OrderNo","Amount"]`
- **THEN** no synthetic class column is emitted; Excel header row begins with `OrderNo, Amount, ...`

### Requirement: Mutually exclusive with explicitSql

When `exportSpec.sql` is non-empty (user-provided raw SELECT) AND `exportSpec.columnOrder` is non-empty, profile loading SHALL fail with code `E_EXPORT_ORDER_WITH_RAW_SQL`. The two settings represent contradictory authorship of column ordering (user-supplied SQL fully owns column order vs profile-declared override) and cannot coexist.

#### Scenario: Both declared is rejected
- **WHEN** `exportSpec.sql = "SELECT ..."` and `exportSpec.columnOrder = ["A","B"]`
- **THEN** profile loading SHALL fail with `E_EXPORT_ORDER_WITH_RAW_SQL`

#### Scenario: Only one of them declared is accepted
- **WHEN** `exportSpec.sql` is set and `columnOrder` is empty (or unset)
- **THEN** profile loading SHALL succeed; export SHALL use the raw SQL's column order without any re-ordering

#### Scenario: Only columnOrder declared, no raw SQL
- **WHEN** `exportSpec.sql` is empty and `columnOrder = ["A","B"]`
- **THEN** profile loading SHALL succeed; export SHALL apply columnOrder to the auto-generated SQL's result

### Requirement: Orthogonality with orderBy

`exportSpec.orderBy` and `exportSpec.columnOrder` SHALL be independent settings. `orderBy` SHALL continue to influence only SQL `ORDER BY` (row ordering); `columnOrder` SHALL influence only Excel column projection (column ordering). They MAY be declared together; neither SHALL constrain the other's set of values.

#### Scenario: Both declared work together
- **WHEN** `orderBy = ["amount"]` and `columnOrder = ["OrderNo","Amount"]`
- **THEN** rows SHALL be sorted by `amount` ascending (current orderBy behavior) AND the Excel columns SHALL be `OrderNo, Amount` followed by the unlisted-suffix
- **AND** profile loading SHALL NOT require entries in `orderBy` to appear in `columnOrder` (or vice versa)

#### Scenario: orderBy column not listed in columnOrder still sorts rows
- **WHEN** `orderBy = ["created_at"]` and `columnOrder = ["OrderNo"]` (and `created_at` is reachable but not in columnOrder)
- **THEN** rows SHALL be sorted by `created_at` and the `created_at` column SHALL appear in the unlisted suffix

### Requirement: Reordering preserves row data identity

The re-ordering SHALL NOT drop, duplicate, or alter any column's data values. For each output row, every column produced by the underlying SQL (SingleTable/MultiTable) or by the per-class projection (Mixed) SHALL appear exactly once in the final Excel row, with its value preserved verbatim.

In Mixed mode where some classes contribute a header that other classes do not, rows from those other classes SHALL render that cell as empty (`QVariant()` → empty Excel cell), consistent with the current behavior of the Mixed projection.

#### Scenario: Cross-class header presents empty cells for non-contributing classes
- **WHEN** Mixed mode has class A contributing `["OrderNo","Amount"]` and class B contributing `["ShipmentNo","Carrier"]`, with `columnOrder = ["OrderNo","ShipmentNo"]`
- **THEN** rows from class A SHALL show `OrderNo` populated and `ShipmentNo` empty; rows from class B SHALL show `ShipmentNo` populated and `OrderNo` empty
- **AND** all other unlisted columns SHALL appear in the suffix following the same emptiness rule

#### Scenario: No value mutation
- **WHEN** a row's natural-order value for column `Amount` is `123.45`
- **THEN** the same row's `Amount` value in the re-ordered Excel output SHALL be `123.45` (no string conversion or rounding introduced by the re-ordering step)
