## MODIFIED Requirements

### Requirement: Route-level lookup declaration

A route SHALL be able to declare zero or more lookups in a `lookups` array. Each lookup specifies a parameter table `G` (which lives in the same SQLite database file as the import target), an equality match between `G`'s columns and Excel header values, and a set of `G` columns to project as new route-local dbColumns on the importing payload. Each lookup SHALL include a non-empty `name` (used in error messages), and `name` SHALL be unique within the declaring route. Both `match` and `select` SHALL be declared as JSON arrays of `[G_column, target]` two-element pairs (NOT as objects), so that pair order is explicit and stable across parses.

A lookup MAY additionally declare two **export-direction-only** fields:

- `exportRoundtrip: bool` — default `true`. When `true`, the export path treats this lookup symmetrically (see the `export-reverse-lookup` capability). When `false`, the export path SHALL NOT perform reverse-lookup for this lookup; the lookup's `select[].dbColumn` columns are exported as-is.
- `exportOnMissing: "error" | "null" | "skip"` — default `"error"`. Governs export-direction behaviour when zero rows in G match a DB row's H values; see the `export-reverse-lookup` capability for full semantics. Any value outside the three allowed strings SHALL cause profile loading to fail.

Both export-direction fields SHALL NOT influence import-direction behaviour in any way. Import-direction semantics (match-key reading from Excel, payload injection, strict cardinality, NULL propagation, validator bypass) remain governed solely by the other requirements in this spec.

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

#### Scenario: exportRoundtrip defaults to true when omitted
- **WHEN** a lookup omits `exportRoundtrip`
- **THEN** profile loading SHALL succeed and the lookup SHALL behave as if `exportRoundtrip: true`

#### Scenario: exportOnMissing defaults to "error" when omitted
- **WHEN** a lookup omits `exportOnMissing`
- **THEN** profile loading SHALL succeed and the lookup SHALL behave as if `exportOnMissing: "error"`

#### Scenario: exportOnMissing must be one of three strings
- **WHEN** a lookup declares `exportOnMissing: "ignore"` (any value other than `"error"`, `"null"`, `"skip"`)
- **THEN** profile loading SHALL fail with a message naming the offending value and listing the three allowed values

#### Scenario: Both export-direction fields do not change import behaviour
- **WHEN** a lookup declares `exportRoundtrip: false, exportOnMissing: "skip"`
- **AND** an import row's match-key matches multiple rows in G
- **THEN** the import path SHALL emit `E_LOOKUP_AMBIGUOUS` (the existing import-direction strict cardinality), unaffected by the export-direction fields

#### Scenario: exportOnMissing with exportRoundtrip false yields info diagnostic
- **WHEN** a lookup declares `exportRoundtrip: false` AND `exportOnMissing: "null"` (i.e. exportOnMissing has no effect because reverse-lookup is disabled)
- **THEN** profile loading SHALL succeed and SHALL emit an info-level diagnostic noting the redundancy
