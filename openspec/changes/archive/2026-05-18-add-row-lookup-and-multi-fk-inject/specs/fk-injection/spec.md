## ADDED Requirements

### Requirement: fkInject is an array of per-parent injection groups

A route's `fkInject` SHALL be declared as a JSON array of injection groups. Each group SHALL name exactly one parent table in `from` and SHALL list one or more column pairs in `pairs`, where each pair is `[parent_column, child_column]`. A route MAY contain multiple groups referencing different parent tables. An absent `fkInject` key, an empty array `[]`, and a `null` value SHALL all be treated as "no injection on this route" (silent no-op, no validation error).

#### Scenario: Single-column injection in array form
- **WHEN** a route declares `fkInject: [{ from:"orders", pairs:[["order_no","order_no"]] }]`
- **THEN** the child payload SHALL receive `order_no` from the parent payload of `orders`

#### Scenario: Composite-key injection from one parent
- **WHEN** a route declares `fkInject: [{ from:"orders", pairs:[["order_no","order_no"],["tenant_id","tenant_id"]] }]`
- **THEN** both `order_no` and `tenant_id` SHALL be copied from the parent payload of `orders` to the child payload

#### Scenario: Multi-parent injection on the same child
- **WHEN** a route declares two groups, e.g. `fkInject: [{ from:"orders", pairs:[["order_no","order_no"]] }, { from:"ref_tenants", pairs:[["tenant_id","tenant_id"]] }]`
- **THEN** after `ImportService::run` returns successfully, the child payload SHALL contain `order_no` (sourced from `orders`) and `tenant_id` (sourced from `ref_tenants`)

#### Scenario: Empty pairs in a group is rejected
- **WHEN** any group's `pairs` array is empty
- **THEN** profile validation SHALL fail

#### Scenario: Absent / empty / null fkInject is a no-op
- **WHEN** a route omits the `fkInject` key, or declares `fkInject: []`, or declares `fkInject: null`
- **THEN** profile loading SHALL succeed and no injection SHALL occur for that route

### Requirement: Injection updates conflict values by name

When an injected child column is also part of the route's `conflict.columns`, the injected value SHALL be written into the corresponding slot of `conflictVals`, located by matching column name against `conflict.columns` (not by positional index into the `pairs` array). UPSERT path correctness SHALL be preserved when the conflict key spans multiple columns including injected ones.

#### Scenario: Composite conflict key partially supplied by injection
- **WHEN** `conflict.columns = ["order_no","line_no"]`, `line_no` comes from Excel mapping, and `order_no` comes from `fkInject`
- **THEN** after injection both slots of `conflictVals` SHALL be populated with the values matching the position of each column in `conflict.columns`

### Requirement: Legacy single-object fkInject form is removed

The legacy `fkInject: { from:"t.c", to:"t.c" }` object form SHALL NOT be accepted by the profile loader. Profile loading SHALL fail with an error that includes the affected route's `table` value as a substring.

#### Scenario: Loading a profile in the old format
- **WHEN** a profile JSON contains `"fkInject": { "from": "orders.order_no", "to": "items.order_no" }` on a route whose `table` is `items`
- **THEN** `ProfileLoader` SHALL reject the profile
- **AND** the error message SHALL include the substring `items`

### Requirement: Composite foreign-key preflight

`ForeignKeyPreflight` SHALL verify the existence of injected parent rows using a composite predicate that AND-joins every pair in a group. The in-batch hit cache SHALL compare tuples whose values are read from the parent payload's bind slots at the column positions named by each pair's `parent_column` (i.e. `parentPayload.binds[parentPayload.indexOf(pair.parent_column)]`), NOT from `parentPayload.conflictVals`. Misses SHALL fall back to a single bounded SELECT against the parent table.

#### Scenario: Multi-column probe against the database
- **WHEN** a child payload requires `(order_no="O1", tenant_id="T1")` and that tuple is not present among the in-batch parent payloads
- **THEN** the preflight SHALL execute `SELECT 1 FROM orders WHERE order_no=? AND tenant_id=? LIMIT 1` with bound values `("O1","T1")`

#### Scenario: Composite hit served from in-batch cache
- **WHEN** a parent payload of `orders` exists in the current batch with `binds` containing `order_no="O1"` and `tenant_id="T1"` (regardless of whether those columns are part of `orders`'s own conflict key)
- **AND** a child payload requires the tuple `("O1","T1")`
- **THEN** no SQL probe SHALL be executed for that child row (verifiable via the preflight query counter; see implementation tasks)

#### Scenario: Composite parent missing
- **WHEN** the tuple is found neither in batch nor in the parent table
- **THEN** the system SHALL emit a row-level error with code `E_VALIDATE_FK` referencing the child row and the offending tuple

### Requirement: Preflight is suppressed at group level for fully lookup-derived groups

When **every** pair in a `fkInject` group references a `parent_column` that the named `from` route produced via its `lookups` (i.e. the column name appears in some lookup's `select.target` set on that route), the entire group SHALL skip the SQL existence probe in `ForeignKeyPreflight`. Mixed groups (some pairs lookup-derived, others not) SHALL NOT occur at preflight time because they are rejected by profile validation; see the validator requirement below.

#### Scenario: All-lookup-derived group skips preflight
- **WHEN** parent route A produces `tenant_id` and `region` via lookups on `ref_tenants` and `ref_regions`, and child route B declares `fkInject: [{ from:"A", pairs:[["tenant_id","tenant_id"], ["region","region"]] }]`
- **THEN** preflight for that group SHALL execute zero SQL probes (verifiable via the preflight query counter)
- **AND** absent or ambiguous lookup values on A SHALL have been reported earlier by the lookup phase, not by FK preflight

#### Scenario: Group with one Excel-derived parent column probes normally
- **WHEN** parent route A produces `tenant_id` via a lookup but `order_no` comes from A's Excel `columns` mapping, and a child B declares a SINGLE group with both pairs together
- **THEN** profile validation SHALL fail before any import runs (mixed groups are illegal; the child must split into two separate groups)

### Requirement: NULL parent value is a row-level error

If, at injection time, the parent payload's `parent_column` slot contains a NULL / null QVariant, the system SHALL emit a row-level error with code `E_VALIDATE_FK` for the child row and SHALL NOT inject NULL into the child payload. This applies whether the NULL originated from a missing Excel cell on the parent, a NULL in a parent's lookup result, or any other upstream cause.

#### Scenario: Parent column is NULL when injection runs
- **WHEN** a parent payload of `orders` has `order_no = NULL` in its `binds` slot
- **AND** a child route B declares `fkInject: [{ from:"orders", pairs:[["order_no","order_no"]] }]`
- **THEN** the child row SHALL produce a row-level `E_VALIDATE_FK` error
- **AND** the child payload's `order_no` slot SHALL NOT be set to NULL by the injection step

### Requirement: Error cascade — child routes drop when their parent route errors on the same row

When any route reports a row-level error for a given Excel row (whether via lookup phase, fkInject NULL, FK preflight miss, or any other row-level error), every route in that row whose `parent` chain transitively reaches the failing route SHALL have its payload dropped for that row, and SHALL NOT itself emit additional row-level errors derived from the same root cause. Errors from unrelated sibling routes on the same row are unaffected.

#### Scenario: Lookup failure on parent drops child silently
- **WHEN** route A's lookup produces `E_LOOKUP_NOT_FOUND` for Excel row 42
- **AND** route B has `parent: A`
- **THEN** route B SHALL produce no row-level error for row 42 (its payload is dropped, not separately reported)
- **AND** `ErrorCollector` SHALL contain exactly one error for row 42 attributable to the lookup root cause (no cascade duplicates)

#### Scenario: Unrelated sibling continues
- **WHEN** route A and route C are both top-level routes (neither is `parent` of the other), A errors on row 42, but C succeeds on row 42
- **THEN** C's payload for row 42 SHALL still be processed and persisted

### Requirement: Validator checks for multi-pair, multi-parent fkInject

Profile validation SHALL verify, for every `fkInject` group on every route, all of the following. Any violation SHALL fail validation with an error that names the affected route, the offending group's `from`, and (where applicable) the offending pair.

1. The group's `from` value SHALL match the `table` field of another route in the same profile. Tables that exist in the schema catalog but are not declared as routes SHALL be rejected (use `lookups` instead).
2. Every `parent_column` in `pairs` SHALL exist either as an Excel-mapped column on the named parent route or as a lookup-output column produced by the parent route's `lookups`.
3. Every `child_column` in `pairs` SHALL exist as an actual column of the route's target table in the schema catalog.
4. Within a single group, the pairs SHALL NOT mix lookup-derived `parent_column`s with non-lookup-derived `parent_column`s. A group SHALL be either entirely lookup-derived or entirely not.
5. Across all `fkInject` groups on a single route, no `child_column` SHALL appear in more than one pair. Each child column SHALL be injection-targeted at most once. **Scope note**: this rule only catches duplicates WITHIN `fkInject` groups. Collisions between a `child_column` and an Excel-mapped column or a lookup output column are checked by the row-lookup spec's "dbColumn naming uniqueness within a route" requirement, which is the sole place that cross-checks all three sources.

#### Scenario: Parent column missing on the parent route
- **WHEN** a pair names a `parent_column` that does not exist in the named `from` route (neither in `columns` nor in any of its lookups' outputs)
- **THEN** profile validation SHALL fail with an error naming the route, the group's `from`, and the offending pair

#### Scenario: Child column missing in target table
- **WHEN** a pair names a `child_column` that does not exist in the route's target table
- **THEN** profile validation SHALL fail with an error naming the route and the offending pair

#### Scenario: from table is not a route in this profile
- **WHEN** a group's `from` names a table that exists in the schema catalog but is not declared as a route in the current profile
- **THEN** profile validation SHALL fail
- **AND** the error message SHALL suggest using `lookups` if the user intends to read existing rows from that table

#### Scenario: Mixed lookup-derived and Excel-derived pairs in one group
- **WHEN** a group on child B has pairs where `parent_column` X is lookup-derived on parent A and `parent_column` Y is Excel-derived on parent A
- **THEN** profile validation SHALL fail
- **AND** the error message SHALL instruct the author to split the group into two separate groups (one all-lookup-derived, one not)

#### Scenario: Duplicate child_column across groups
- **WHEN** child route B has `fkInject: [{ from:"A", pairs:[["tenant_id","tenant_id"]] }, { from:"C", pairs:[["tenant_id","tenant_id"]] }]` (both groups target the same child column `tenant_id`)
- **THEN** profile validation SHALL fail with an error naming the duplicated `child_column`
