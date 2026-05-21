# Export Reverse Lookup Spec

## Purpose

Defines how the export path performs reverse-lookup substitution symmetrically with the import path. Covers symmetric declaration reuse, `exportRoundtrip` and `exportOnMissing` controls, batch prefetch with identity merging, H-to-A column substitution, naming-conflict precedence, coercion semantics, Mixed-mode scoping, row resilience, coexistence with `columnOrder` and `time-format`, and no-cascading constraints.

## Requirements

### Requirement: Reverse-lookup reuses the same `lookups[]` declaration

The export path SHALL consume the existing per-route `lookups[]` declaration symmetrically with the import path. There SHALL NOT be a separate `reverseLookups` or `exportLookups` array. For each declared lookup in a route, the export direction SHALL read each row's `select[].dbColumn` values from the DB result, query the parameter table `G` using those values as match keys, and write the corresponding `match[].Excel_header` values back into the export row.

#### Scenario: Symmetric declaration drives both directions
- **WHEN** a route declares `lookups: [{ name:"customer_info", from:"ref_customers", match:[["c_no","客户编号"]], select:[["c_name","customer_name"]] }]`
- **AND** import previously stored `customer_name` into the route's DB table
- **THEN** export SHALL query `ref_customers` using each row's `customer_name` value (the `select` target), retrieve the corresponding `c_no` (the `match` G_column), and write that `c_no` value into the Excel cell whose header is `客户编号` (the `match` Excel_header)

#### Scenario: No separate reverseLookups array exists
- **WHEN** a profile declares an `exportSpec.reverseLookups` or `exportSpec.exportLookups` array
- **THEN** profile loading SHALL fail with a message indicating that reverse lookups are declared via the existing `lookups[]` field with `exportRoundtrip` controls

### Requirement: `exportRoundtrip` switch per lookup

Each `LookupSpec` SHALL accept an optional `exportRoundtrip: bool` field. The default value SHALL be `true`. When `true`, the export path SHALL perform reverse-lookup substitution as specified in this capability. When `false`, the export path SHALL skip reverse-lookup for that lookup and SHALL include the lookup's `select[].dbColumn` columns (the H columns) in the Excel output verbatim, with their column names equal to the `select[].dbColumn` names.

#### Scenario: Default exportRoundtrip is true
- **WHEN** a lookup declares no `exportRoundtrip` field
- **THEN** export SHALL behave as if `exportRoundtrip: true`

#### Scenario: exportRoundtrip false retains H columns
- **WHEN** a lookup declares `exportRoundtrip: false` with `select: [["c_name","customer_name"]]`
- **THEN** export SHALL emit a column `customer_name` in the Excel output (rather than the `match` Excel header), with the value taken directly from the DB row's `customer_name` column

#### Scenario: Mixed exportRoundtrip across lookups in one route
- **WHEN** route R has `lookups: [{ name:"L1", exportRoundtrip:true, ... }, { name:"L2", exportRoundtrip:false, ... }]`
- **THEN** L1's H columns SHALL be replaced by L1's match Excel headers; L2's H columns SHALL appear as-is

### Requirement: `exportOnMissing` controls cardinality miss behaviour

Each `LookupSpec` SHALL accept an optional `exportOnMissing` field with exactly three allowed string values: `"error"`, `"null"`, `"skip"`. The default SHALL be `"error"`. The field SHALL govern reverse-lookup behaviour when zero rows in `G` match a DB row's H values:

- `"error"` → emit a row-level error with code `E_REVERSE_LOOKUP_NOT_FOUND`; the row SHALL be skipped (not written to Excel) for this route; other routes contributing to the same Excel row are unaffected (consistent with the existing route-skip-row behaviour).
- `"null"` → write the lookup's match Excel header cells as empty (NULL); other cells of the row SHALL be written; no row-level error is emitted.
- `"skip"` → identical to `"null"` for output behaviour; differs only by NOT counting toward error totals (used for known legacy data). No row-level error is emitted.

Regardless of `exportOnMissing`, when MORE THAN ONE row in `G` matches a DB row's H values, the system SHALL emit a row-level error with code `E_REVERSE_LOOKUP_AMBIGUOUS`. Multi-match SHALL NEVER be silenced.

`exportOnMissing` SHALL have no effect when `exportRoundtrip` is `false`; profile loading SHALL emit an info-level diagnostic for that combination (not an error).

#### Scenario: Default exportOnMissing is "error"
- **WHEN** a lookup declares no `exportOnMissing` and a DB row has no matching G row
- **THEN** export SHALL emit `E_REVERSE_LOOKUP_NOT_FOUND` for that row and SHALL NOT write that row to Excel for the declaring route

#### Scenario: exportOnMissing "null" writes empty cell, row continues
- **WHEN** lookup `L1` has `exportOnMissing: "null"` and a DB row has no matching G row
- **THEN** the Excel cells corresponding to L1's match Excel headers SHALL be empty; other cells of the row SHALL be written normally; no row-level error SHALL be emitted

#### Scenario: exportOnMissing "skip" writes empty cell silently
- **WHEN** lookup `L1` has `exportOnMissing: "skip"` and a DB row has no matching G row
- **THEN** the Excel cells corresponding to L1's match Excel headers SHALL be empty and no row-level error SHALL be emitted; the row's other cells SHALL be written normally

#### Scenario: Ambiguous multi-match always errors
- **WHEN** a DB row's H values map to two or more rows in G, regardless of `exportOnMissing` value
- **THEN** export SHALL emit `E_REVERSE_LOOKUP_AMBIGUOUS` and the message SHALL include the lookup's `name`, the offending H values, and the count of matching G rows

#### Scenario: Invalid exportOnMissing value rejected at load
- **WHEN** a lookup declares `exportOnMissing: "ignore"` (or any value other than the three allowed)
- **THEN** profile loading SHALL fail with a message naming the offending value and listing the three allowed values

### Requirement: Reverse-lookup batch prefetch with identity merging

Reverse-lookup G queries SHALL use per-identity batch prefetch, NOT per-row queries. A reverse-lookup identity is the tuple `(from, match-pairs in array order, select-pairs in array order)` — identical to the import-direction identity. Two lookups sharing this identity (potentially across routes or across classes in Mixed mode) SHALL share a single prefetch result.

The system SHALL collect every distinct H-value tuple from the main SELECT's result rows for each identity, issue `ceil(K / chunk_limit)` SELECTs against G per identity (chunking when SQL parameter limit would be exceeded), and build an in-memory map keyed by H-value tuple. When K = 0, zero SELECTs SHALL be executed for that identity.

The export-direction prefetch SQL SHALL select both the `match[].G_column` columns and the `select[].G_column` columns from `G`, filtered by the H-value `IN` list keyed on `select[].G_column`.

#### Scenario: Batch prefetch is used (not per-row)
- **WHEN** export's main SELECT returns N rows with K distinct H-value tuples for some lookup-identity (K > 0)
- **THEN** the system SHALL execute exactly `ceil(K / chunk_limit)` SELECTs against G for that identity, regardless of N

#### Scenario: Zero distinct H values skips prefetch
- **WHEN** for a given reverse-lookup identity, every main-SELECT row has NULL or empty H values (K = 0 after empty-filtering)
- **THEN** zero SELECTs SHALL be executed for that identity

#### Scenario: Identity merging across routes and classes
- **WHEN** two routes (or two routes in different classes under Mixed mode) declare lookups with the same `(from, match-pairs, select-pairs)` identity, all with `exportRoundtrip: true`
- **THEN** the system SHALL execute one combined set of SELECTs and SHALL build one shared in-memory map

#### Scenario: Prefetch failure is row-level decorated, sheet aborts
- **WHEN** a reverse-lookup prefetch SELECT fails (e.g. transient IO error)
- **THEN** the system SHALL emit a table-level error with code `E_REVERSE_LOOKUP_QUERY_FAILED` and SHALL abort export for that sheet (consistent with import-side `E_LOOKUP_QUERY_FAILED` semantics)

### Requirement: H columns are removed from Excel output; A headers appear

For every lookup with `exportRoundtrip: true`, the Excel output SHALL NOT include columns whose names equal the lookup's `select[].dbColumn` values (the H columns). Instead, the Excel output SHALL include columns whose names equal the lookup's `match[].Excel_header` values (the A columns), with each cell's value populated from the reverse-lookup result for that row.

When a lookup's `match` declares multiple pairs (composite match key), ALL of the corresponding Excel headers SHALL appear in the output.

#### Scenario: Single-pair match restores one Excel column
- **WHEN** lookup has `match: [["c_no","客户编号"]]`, `select: [["c_name","customer_name"]]`, `exportRoundtrip: true`
- **THEN** Excel output SHALL include a column `客户编号` (populated from reverse-lookup) and SHALL NOT include a column `customer_name`

#### Scenario: Composite match restores multiple Excel columns
- **WHEN** lookup has `match: [["region","区域"],["tier","等级"]]`, `select: [["price","price"]]`, `exportRoundtrip: true`
- **THEN** Excel output SHALL include columns `区域` and `等级` (both populated from reverse-lookup) and SHALL NOT include a column `price`

#### Scenario: exportRoundtrip false leaves H, omits A
- **WHEN** lookup has `match: [["c_no","客户编号"]]`, `select: [["c_name","customer_name"]]`, `exportRoundtrip: false`
- **THEN** Excel output SHALL include a column `customer_name` (populated from DB) and SHALL NOT include a column `客户编号` from this lookup (other unrelated sources for `客户编号` are governed independently)

### Requirement: Naming-conflict precedence for restored A headers

When the Excel header restored by reverse-lookup (the A header) is also declared as `ColumnSpec.source` on some route active in the export (i.e. another route also projects a column to that same Excel header), the **`ColumnSpec.source`-driven value SHALL win** for that Excel cell. The reverse-lookup result SHALL serve as a fallback only when no `ColumnSpec.source` for the same header is contributing a value for the row.

This rule SHALL apply per row, not per profile: a row that has a non-NULL `ColumnSpec.source` value for that header uses the source value; a row that has NULL/missing from that source uses the reverse-lookup value (subject to `exportOnMissing` if also missing there).

#### Scenario: ColumnSpec.source wins when both contribute
- **WHEN** a route declares `columns: { client_id: { source: "客户编号", ... } }` AND another route declares `lookups: [{ match:[["c_no","客户编号"]], select:[["c_name","customer_name"]], exportRoundtrip:true, ... }]`
- **AND** a particular export row has both a non-NULL value from the `client_id`-mapped projection and a non-NULL reverse-lookup result for `客户编号`
- **THEN** the Excel cell for `客户编号` SHALL contain the `ColumnSpec.source`-projected value

#### Scenario: Reverse-lookup fills in when source absent
- **WHEN** for the same configuration as above, a particular export row has NULL from the `client_id` projection but a non-NULL reverse-lookup result
- **THEN** the Excel cell for `客户编号` SHALL contain the reverse-lookup result

### Requirement: Strict equality and coercion mirror the import side

The reverse-lookup equality between DB H values and G's `select[].G_column` values SHALL be strict (the same rule as import's "Strict equality at match time"). Specifically, coercion of DB H values to G column affinity SHALL follow the same affinity-coercion table as import-side lookup match-keys. No case-folding, whitespace trimming, or normalisation SHALL occur at equality time.

If a DB H value is NULL, the reverse-lookup MAY skip the row's lookup (no query against G is required), and the `exportOnMissing` policy SHALL apply as if K = 0.

#### Scenario: Coercion preserves equality semantics
- **WHEN** DB column `customer_name` has TEXT affinity and returns `"Acme"`, and G's `select` G_column has TEXT affinity
- **THEN** equality SHALL be exact string equality (no trim, no case-fold)

#### Scenario: NULL H value treated as miss
- **WHEN** a DB row has NULL in the H column used as reverse-lookup match
- **THEN** the row SHALL be treated as having no G match (K = 0); `exportOnMissing` SHALL govern the outcome

### Requirement: Mixed mode reverse-lookup is class-scoped at resolution

In Mixed mode, reverse-lookup prefetch SHALL run once at sheet scope (collecting H-value tuples across all classes' rows), but per-row resolution SHALL be scoped to the class that produced the row. Errors emitted by reverse-lookup resolution SHALL include the resolved `classId` in addition to the route table and lookup `name`.

#### Scenario: Cross-class prefetch merge
- **WHEN** classes A and B both contain routes whose lookups share the same `(from, match-pairs, select-pairs)` identity, with `exportRoundtrip: true`
- **THEN** prefetch SHALL merge them per the identity-merging rule

#### Scenario: Per-class resolution
- **WHEN** a class-A row's reverse-lookup misses in G with `exportOnMissing: "error"`
- **THEN** the row-level error SHALL include A's `classId`, the offending route's `table`, and the lookup's `name`

### Requirement: Export remains row-resilient

A single `E_REVERSE_LOOKUP_NOT_FOUND` or `E_REVERSE_LOOKUP_AMBIGUOUS` SHALL NEVER abort the entire sheet's export. Affected rows are skipped (for "error" mode) or written with empty A cells (for "null"/"skip" modes); the rest of the sheet SHALL be processed. Only `E_REVERSE_LOOKUP_QUERY_FAILED` (a table-level error during prefetch) aborts export for the sheet.

#### Scenario: One bad row, sheet completes
- **WHEN** out of 100 rows, row 5 triggers `E_REVERSE_LOOKUP_NOT_FOUND` and `exportOnMissing: "error"`
- **THEN** export SHALL emit one row-level error referring to row 5, SHALL NOT write row 5 to Excel, and SHALL write rows 1-4 and 6-100 normally

#### Scenario: Prefetch failure is table-level
- **WHEN** the prefetch SELECT against G fails for any reason
- **THEN** export SHALL emit a single table-level `E_REVERSE_LOOKUP_QUERY_FAILED`, abort the sheet, and SHALL NOT write any rows

### Requirement: Coexistence with `columnOrder` and `time-format`

Reverse-lookup SHALL interact with the `export-column-order` capability and the `time-format` capability via the restored Excel header names (A columns), following the rules below:

- `columnOrder` MAY list A headers (since they appear in the output) and SHALL NOT list H dbColumn names (since they no longer appear). Profile validation against `columnOrder` SHALL evaluate the **post-substitution** header set: A headers are part of the accepted set; H dbColumns whose `exportRoundtrip` is `true` SHALL NOT be in the accepted set.
- For each restored A header that corresponds to a `ColumnSpec.source` governed by a temporal slot (`dateFormat` / `datetimeFormat` / `timeFormat`), the time-format export-direction rules SHALL apply to the reverse-lookup result value before it is written to Excel.

#### Scenario: columnOrder lists restored A header
- **WHEN** lookup with `exportRoundtrip: true` restores `客户编号` to the output and `columnOrder = ["客户编号","Amount"]`
- **THEN** profile loading SHALL succeed and the Excel column order SHALL be `客户编号, Amount, ...`

#### Scenario: columnOrder lists H dbColumn that has disappeared
- **WHEN** lookup with `exportRoundtrip: true` has `select: [["c_name","customer_name"]]` AND `columnOrder = ["customer_name", ...]`
- **THEN** profile loading SHALL fail with `E_EXPORT_UNKNOWN_HEADER` (from the `export-column-order` capability), because `customer_name` is not in the post-substitution header set

#### Scenario: Reverse-lookup A column is a date column
- **WHEN** `dateFormat = {excelFormat:"yyyy/M/d", dbFormat:"yyyy-MM-dd"}` applies to the `ColumnSpec` whose `source` equals a restored A header (e.g. `订单日期`), and reverse-lookup produces the value `"2025-03-14"` for a row
- **THEN** the export-side time-format rule SHALL parse `"2025-03-14"` via `dbFormat` and write `"2025/3/14"` to Excel

### Requirement: No reverse-lookup cascading

A lookup's `select` columns SHALL NOT be reused as another lookup's `select`-key for reverse purposes in the same route. The "no lookup cascading in this iteration" requirement from the `row-lookup` capability SHALL continue to apply in the export direction.

#### Scenario: Attempt to chain reverse-lookups
- **WHEN** within the same route, lookup A's `select[].dbColumn` is also referenced as lookup B's `select[].dbColumn` (or transitively forms a chain)
- **THEN** profile loading SHALL fail; the failure mode SHALL be identical to the import-side cascading-lookup failure (the existing `dbColumn naming uniqueness within a route` check or its equivalent)
