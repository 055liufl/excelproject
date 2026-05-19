## ADDED Requirements

### Requirement: Route-level lookup declaration

A route SHALL be able to declare zero or more lookups in a `lookups` array. Each lookup specifies a parameter table `G` (which lives in the same SQLite database file as the import target), an equality match between `G`'s columns and Excel header values, and a set of `G` columns to project as new route-local dbColumns on the importing payload. Each lookup SHALL include a non-empty `name` (used in error messages), and `name` SHALL be unique within the declaring route. Both `match` and `select` SHALL be declared as JSON arrays of `[G_column, target]` two-element pairs (NOT as objects), so that pair order is explicit and stable across parses.

#### Scenario: Single lookup with one match key and multiple select columns
- **WHEN** a route declares `lookups: [{ name:"customer_info", from:"ref_customers", match:[["c_no","客户编号"]], select:[["c_name","customer_name"],["c_tier","tier"]] }]`
- **AND** an Excel row carries `客户编号 = "C001"` and `ref_customers` contains exactly one row with `c_no="C001"`, `c_name="Acme"`, `c_tier="gold"`
- **THEN** the route payload SHALL include dbColumns `customer_name="Acme"` and `tier="gold"` in addition to whatever `columns:` maps from the Excel row

#### Scenario: Multiple lookups on the same route
- **WHEN** a route declares two lookups against different parameter tables, each with a distinct `name`
- **THEN** each lookup SHALL be processed independently and both result sets SHALL be merged into the same route payload

#### Scenario: Composite match key
- **WHEN** a lookup's `match` declares more than one pair (e.g. `[["region","区域"],["tier","等级"]]`)
- **THEN** the match SHALL be equivalent to an AND of equality predicates over all pairs, in the array order

#### Scenario: name is required
- **WHEN** a lookup omits `name` or sets it to an empty / whitespace-only string
- **THEN** profile validation SHALL fail

#### Scenario: name is unique within a route
- **WHEN** two lookups on the same route share the same `name` value
- **THEN** profile validation SHALL fail

#### Scenario: match or select declared as JSON object is rejected
- **WHEN** a lookup declares `match: {"c_no":"客户编号"}` (object form) or `select: {"c_name":"customer_name"}` (object form)
- **THEN** profile loading SHALL fail with a message that indicates the expected array-of-pairs form

### Requirement: Lookup batch prefetch with cross-lookup identity merging

Lookup values SHALL be obtained via a per-lookup-identity batch SELECT executed once before row-level routing, not via per-row queries. A "lookup identity" is the tuple `(from, match-pairs in array order, select-pairs in array order)`; two lookups (potentially on different routes or different classes in Mixed mode) that share an identity SHALL share a single prefetch result. The system SHALL scan the Excel sheet once to collect all distinct match-key tuples per identity, issue `ceil(K / chunk_limit)` SELECTs per identity where K is the count of distinct keys (chunking when SQL parameter limit would be exceeded), and build an in-memory map keyed by match-key tuple. When K = 0, zero SELECTs SHALL be executed for that identity.

#### Scenario: Batch prefetch is used
- **WHEN** an import declares a lookup and the Excel sheet has N rows referencing K distinct match keys (K > 0)
- **THEN** the system SHALL execute exactly `ceil(K / chunk_limit)` SELECTs for that lookup-identity, regardless of N (verifiable via the prefetch query counter; see implementation tasks)

#### Scenario: Zero distinct keys skips prefetch
- **WHEN** for a given lookup-identity, every Excel row's match-key is empty or every row is excluded by coercion failure (K = 0 after empty-filtering and coercion-filtering)
- **THEN** zero SELECTs SHALL be executed for that lookup-identity

#### Scenario: Identity merging across routes / classes
- **WHEN** two routes (or two routes in different classes under Mixed mode) declare lookups with the same `(from, match-pairs, select-pairs)` identity
- **THEN** the system SHALL execute one combined set of SELECTs for that identity (verifiable via the prefetch query counter)
- **AND** both routes SHALL consume the same in-memory map

#### Scenario: Prefetch SQL failure is fatal
- **WHEN** a prefetch SELECT fails (syntax, missing table at query time, IO error)
- **THEN** the system SHALL emit a table-level error with code `E_LOOKUP_QUERY_FAILED` and abort the import for that sheet
- **AND** no row-level errors SHALL be emitted for this lookup

### Requirement: Mixed mode lookup scoping

In Mixed mode (`mode: "Mixed"` with `classes: [...]`), prefetch SHALL run once at sheet scope (collecting keys from all rows regardless of which class they will resolve to), but per-row resolution SHALL be scoped to the class the row resolves to. Row-level errors emitted by lookup resolution SHALL include the resolved `classId` in addition to the route table and lookup `name`.

#### Scenario: Same lookup declared in two classes
- **WHEN** classes A and B both contain routes with a lookup of the same identity `(from, match-pairs, select-pairs)`
- **THEN** prefetch SHALL merge them per the identity-merging rule
- **AND** at row-time, a row resolved to class A SHALL consult only A's lookup declaration; B's lookup SHALL NOT be consulted for that row

#### Scenario: Lookup miss reported with class context
- **WHEN** a row is classified as class A and a lookup in one of A's routes misses
- **THEN** the row-level error SHALL include A's `classId`, the offending route's `table`, and the lookup's `name`

### Requirement: Excel value coercion to G column type before equality

Match-key values read from Excel SHALL be coerced to the SQLite affinity of the corresponding `G.column` before prefetch SELECT binding and before in-memory map lookup. Coercion rules:

- G column affinity `TEXT` → `QVariant::toString()`
- G column affinity `INTEGER` → `QVariant::toLongLong(&ok)`; if `!ok`, row-level `E_LOOKUP_KEY_INVALID`
- G column affinity `REAL` → `QVariant::toDouble(&ok)`; if `!ok`, row-level `E_LOOKUP_KEY_INVALID`
- G column affinity `BLOB`, `NUMERIC`, or no declared affinity → original `QVariant` passes through unchanged

Coercion failure on any one match-key column SHALL abort that row's resolution for the lookup-declaring route. The failing key SHALL NOT contribute to the prefetch IN-list (so prefetch SHALL NOT fail solely because some rows have un-coercible keys).

#### Scenario: Excel double matches G TEXT key
- **WHEN** Excel reads `1001` as `QVariant(double)` and G's match column is TEXT containing `"1001"`
- **THEN** the system SHALL coerce the Excel value to `"1001"` before both prefetch binding and in-memory equality, and the row SHALL successfully resolve

#### Scenario: Coercion failure produces row-level error and is excluded from prefetch
- **WHEN** Excel value is `"abc"` and G's match column has INTEGER affinity
- **THEN** the system SHALL emit a row-level error with code `E_LOOKUP_KEY_INVALID` for the affected row
- **AND** `"abc"` SHALL NOT appear in the prefetch IN-list (and SHALL NOT cause prefetch to fail)

### Requirement: Strict cardinality semantics

Per-row lookup resolution SHALL be strict: exactly one matching row in `G` is required for success. Zero matches, more than one match, and any empty match-key in the Excel row SHALL each produce a row-level error and SHALL cause the lookup-declaring route to be skipped for that row (other routes on the same row continue independently, subject to the error-cascade rule in the fk-injection spec). A match-key value SHALL be considered empty if `QVariant::isNull()` is true OR if `QVariant::toString().trimmed().isEmpty()` is true. Numeric zero (`0`, `"0"`, `0.0`) is NOT empty.

#### Scenario: Zero matches
- **WHEN** the Excel row's match-key tuple (after coercion) is absent from the prefetched map
- **THEN** the system SHALL emit a row-level error with code `E_LOOKUP_NOT_FOUND` and SHALL NOT append the lookup's select columns to the payload

#### Scenario: Multiple matches
- **WHEN** the prefetch finds more than one row in `G` for the same match-key tuple
- **THEN** the system SHALL emit a row-level error with code `E_LOOKUP_AMBIGUOUS` for every Excel row that resolves to that tuple
- **AND** the error message SHALL suggest checking `G` for duplicate rows on the match columns

#### Scenario: Empty match key
- **WHEN** any column listed in `lookup.match` is null or whitespace-only for the current Excel row
- **THEN** the system SHALL emit a row-level error with code `E_LOOKUP_KEY_EMPTY` and SHALL NOT attempt to query the prefetch map

#### Scenario: Numeric zero is not empty
- **WHEN** the Excel match-key cell is `0`, `"0"`, or `0.0`
- **THEN** the system SHALL treat it as a valid key and proceed to lookup (no `E_LOOKUP_KEY_EMPTY`)

### Requirement: Strict equality at match time

Match-key comparison (after coercion) SHALL be strict `QVariant` equality. No case-folding, whitespace trimming, full-/half-width normalization, or other implicit transformations SHALL occur during equality comparison. Note: trimming IS applied during the empty-detection step (preceding requirement), but NOT during equality. Users who need normalization SHALL clean data upstream in Excel or in `G`.

#### Scenario: Case difference does not match
- **WHEN** Excel has `"abc"` and G has `"ABC"` for a TEXT match column
- **THEN** the row SHALL produce `E_LOOKUP_NOT_FOUND` (no implicit case-fold)

#### Scenario: Trailing whitespace does not match (but does NOT trigger empty)
- **WHEN** Excel has `"C001 "` (trailing space) and G has `"C001"` for a TEXT match column
- **THEN** the row SHALL produce `E_LOOKUP_NOT_FOUND` (no trim during equality); it SHALL NOT produce `E_LOOKUP_KEY_EMPTY` (the trimmed value is non-empty)

### Requirement: Lookup outputs may be NULL; downstream rules apply

When `G`'s matched row contains NULL in one or more `select` columns, the lookup SHALL succeed and propagate NULL into the route payload's corresponding dbColumn. Lookup itself SHALL NOT treat NULL select values as an error. Downstream behavior (fkInject NULL handling, database NOT NULL constraints, etc.) governs the eventual fate of NULL-bearing rows.

#### Scenario: NULL in G's select column propagates
- **WHEN** G's matched row has `c_tier=NULL` and the lookup selects `[["c_tier","tier"]]`
- **THEN** the route payload SHALL contain `tier=NULL` and lookup SHALL NOT emit a row-level error
- **AND** if `tier` is later referenced as a `parent_column` by a fkInject pair, the fk-injection spec's NULL rule (row-level `E_VALIDATE_FK`) SHALL apply at that point, not here

### Requirement: Lookup outputs bypass column-level validators

Columns produced by a lookup SHALL be appended to the route payload directly, without passing through any `columns:` `validatorTokens` (e.g. `len<=32`, `regex:...`). `G` is treated as a trusted source. Users who need additional constraints on lookup-derived columns SHALL use database-level constraints (`CHECK`, `NOT NULL`, etc.) or clean `G` upstream.

#### Scenario: validatorTokens on a colliding name is moot at runtime
- **WHEN** a route's `columns:` declares dbColumn `customer_name` with `validatorTokens:["len<=32"]` and a lookup also produces `customer_name`
- **THEN** profile validation SHALL reject the conflict at load time (per the dbColumn uniqueness requirement), so the validator/lookup interaction never arises at runtime

### Requirement: Lookup outputs may participate in conflict.columns

A lookup-produced dbColumn MAY appear in `conflict.columns`. UPSERT conflict resolution SHALL treat it identically to Excel-mapped or fkInject-injected dbColumns. There is no source-of-origin distinction at SqlBuilder.

#### Scenario: lookup output is part of composite UPSERT key
- **WHEN** a route's `conflict.columns = ["tenant_id","line_no"]`, `tenant_id` is produced by a lookup, and `line_no` comes from Excel
- **THEN** UPSERT SHALL successfully use `(tenant_id, line_no)` as the conflict resolution key

### Requirement: Route-local visibility of lookup outputs

Columns produced by a lookup SHALL be visible only inside the route that declared the lookup. Other routes on the same Excel row SHALL NOT see these columns through any implicit mechanism. Cross-route propagation SHALL be possible only by explicit `fkInject` declaration on the consuming child route, treating the lookup-declaring route as the parent. (See also fk-injection spec, requirement "Preflight is suppressed at group level for fully lookup-derived groups".)

#### Scenario: Sibling route does not see lookup output
- **WHEN** route A declares a lookup producing `customer_name`, and a sibling route B (not a child of A) is also active for the same Excel row
- **THEN** route B's payload SHALL NOT contain `customer_name` unless B independently sources it — that is, either B's own `columns:` mapping reads an unrelated Excel cell into a dbColumn that happens to also be named `customer_name`, OR B's own `fkInject` from a different parent route happens to inject a `customer_name`. The two routes' `customer_name` values are independent and may differ.

#### Scenario: Child route consumes lookup output via fkInject
- **WHEN** route A declares a lookup producing column `tenant_id` and child route B (with `parent: A`) declares `fkInject: [{ from: A.table, pairs: [["tenant_id","tenant_id"]] }]`
- **THEN** B's payload SHALL receive the lookup-derived `tenant_id` value from A

### Requirement: dbColumn naming uniqueness within a route

Within a single route, dbColumn names SHALL be unique across all three sources:

1. Excel-mapped columns from `columns:`
2. Lookup output names — i.e. the second element of each pair in any `lookups[].select`
3. fkInject child columns — i.e. the second element of each pair in any `fkInject[].pairs`

Additionally, within a single lookup's `select` array, the target names (second element of each pair) SHALL also be unique. This entire uniqueness check is the sole responsibility of profile validation against this requirement; the fk-injection spec's "no duplicate child_column" rule only addresses duplicates WITHIN fkInject groups and does NOT cross-check against lookup or Excel sources.

#### Scenario: Lookup output collides with Excel-mapped column
- **WHEN** a route maps Excel header `客户名` to dbColumn `customer_name` and also declares a lookup with `select: [["c_name","customer_name"]]`
- **THEN** profile validation SHALL fail with an error that includes the substring `customer_name`

#### Scenario: Lookup output collides with fkInject child_column
- **WHEN** a route declares a lookup producing `tenant_id` and also declares `fkInject: [{ from:"X", pairs:[["tenant_id","tenant_id"]] }]`
- **THEN** profile validation SHALL fail with an error that includes the substring `tenant_id`

#### Scenario: Duplicate target within a single lookup's select
- **WHEN** a lookup declares `select: [["c_name","customer_name"], ["c_alias","customer_name"]]`
- **THEN** profile validation SHALL fail

### Requirement: Lookup validation against schema and sheet header

Profile validation SHALL verify all of the following for each lookup:

- `from` table exists in the schema catalog
- Every `match[].G_column` (first element of each pair) exists as a column of `from`
- Every `select[].G_column` (first element of each pair) exists as a column of `from`
- Every `match[].excel_header` (second element of each pair) exists in the Excel sheet header row
- `name` is non-empty (per the declaration requirement)

#### Scenario: Lookup references non-existent G table
- **WHEN** a lookup's `from` table does not exist in the SQLite schema
- **THEN** profile validation SHALL fail before any rows are read

#### Scenario: Lookup references non-existent match column on G
- **WHEN** `lookup.match` contains a pair whose first element is not a column of `G`
- **THEN** profile validation SHALL fail with an error that includes the lookup's `name` and the offending column name as substrings

#### Scenario: Lookup references non-existent Excel header
- **WHEN** any second element in `lookup.match` is not present in the sheet's header row
- **THEN** profile validation SHALL fail before row processing begins

#### Scenario: Excel header may serve both as match key and as a columns: source
- **WHEN** the same Excel header is listed in `lookup.match` AND mapped via `columns:` to a dbColumn
- **THEN** profile validation SHALL succeed (one Excel column may have multiple consumers)

### Requirement: No lookup cascading in this iteration

A lookup's `match` keys SHALL be Excel header values only. The output columns of one lookup SHALL NOT be usable as the match key of another lookup in the same route. Cross-lookup or cross-route data flow requires `fkInject`. This requirement collapses to the same validator failure mode as "match references non-existent Excel header"; no new error class is introduced.

#### Scenario: Attempt to chain lookups
- **WHEN** lookup B's `match` pair has a second element that names a dbColumn produced by lookup A (and is not present as an Excel sheet header)
- **THEN** profile validation SHALL fail because that value does not match any Excel sheet header
