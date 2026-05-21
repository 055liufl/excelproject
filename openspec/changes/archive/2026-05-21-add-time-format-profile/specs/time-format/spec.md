## ADDED Requirements

### Requirement: Three independent temporal format slots

A profile SHALL be able to declare three independent temporal format objects at the profile root: `dateFormat`, `datetimeFormat`, `timeFormat`. Each object SHALL contain at minimum `excelFormat` (a Qt date/time format string used at the Excel boundary) and `dbFormat` (a Qt date/time format string used at the SQLite boundary). The three slots SHALL be processed independently and SHALL NOT share or fall back to each other's values.

Each slot MAY additionally declare `excelFormatFallback` as a JSON array of Qt format strings. The fallback list is consulted only on the Excel → in-memory parsing direction (import); export and the DB direction SHALL ignore it.

#### Scenario: All three slots declared at profile root
- **WHEN** `profile.json` declares `dateFormat: {excelFormat:"yyyy/M/d", dbFormat:"yyyy-MM-dd"}`, `datetimeFormat: {excelFormat:"yyyy/M/d HH:mm:ss", dbFormat:"yyyy-MM-dd HH:mm:ss"}`, and `timeFormat: {excelFormat:"HH:mm", dbFormat:"HH:mm:ss"}`
- **THEN** profile loading SHALL succeed and SHALL produce three independent in-memory configurations, one per slot

#### Scenario: Only one slot declared
- **WHEN** a profile declares only `dateFormat` and omits `datetimeFormat` / `timeFormat`
- **THEN** the absent slots SHALL remain unset and SHALL NOT inherit values from the declared slot

#### Scenario: excelFormatFallback is array-only
- **WHEN** a slot declares `excelFormatFallback` as a non-array (e.g. `"yyyy-MM-dd"` as a bare string)
- **THEN** profile loading SHALL fail with a message identifying the offending slot and the expected array form

### Requirement: Per-column override with field-level merge

A `ColumnSpec` SHALL be able to redeclare any of `dateFormat`, `datetimeFormat`, `timeFormat`. When a column redeclares a slot, the effective slot for that column SHALL be the result of **field-level merge**: each field present in the column-level declaration overrides the corresponding profile-level field; each field absent at column level SHALL inherit from the profile-level slot.

If a column declares no temporal slot, the column SHALL use the profile-level slot verbatim.

If neither the column nor the profile declares a slot relevant to that column's type, the legacy compatibility rule (see Requirement "Compatibility with `date:fmt` validator") SHALL apply.

#### Scenario: Column overrides excelFormat only, inherits dbFormat
- **WHEN** profile declares `dateFormat: {excelFormat:"yyyy-MM-dd", dbFormat:"yyyy-MM-dd"}` and column X declares `dateFormat: {excelFormat:"yyyy/M/d"}` (no `dbFormat`)
- **THEN** the effective `dateFormat` for column X SHALL be `{excelFormat:"yyyy/M/d", dbFormat:"yyyy-MM-dd"}`

#### Scenario: Column overrides excelFormatFallback
- **WHEN** profile declares no `excelFormatFallback` and column X declares `dateFormat.excelFormatFallback: ["yyyy/M/d","yyyy.M.d"]`
- **THEN** column X SHALL use the fallback list and other columns SHALL NOT receive any fallback

### Requirement: Format token validation per slot type

Profile loading SHALL reject formats whose tokens are incompatible with the slot's intrinsic type. Specifically:

- `dateFormat.excelFormat` and `dateFormat.dbFormat` SHALL NOT contain time tokens (`H`, `h`, `m`, `s`, `z`, `t`, `a`, `A`, `AP`, `ap`). The check is structural (presence in the format string), not semantic.
- `timeFormat.excelFormat` and `timeFormat.dbFormat` SHALL NOT contain date tokens (`y`, `M`, `d`).
- `datetimeFormat.excelFormat` and `datetimeFormat.dbFormat` are unconstrained.

`excelFormatFallback` entries SHALL be checked against the same rules as the slot's `excelFormat`.

#### Scenario: dateFormat contains hour token
- **WHEN** a profile declares `dateFormat: {excelFormat:"yyyy-MM-dd HH:mm", dbFormat:"yyyy-MM-dd"}`
- **THEN** profile loading SHALL fail with a message that names the offending slot (`dateFormat.excelFormat`) and the offending token

#### Scenario: timeFormat contains date token
- **WHEN** a profile declares `timeFormat: {excelFormat:"yyyy HH:mm", dbFormat:"HH:mm:ss"}`
- **THEN** profile loading SHALL fail with a message that names the offending slot

#### Scenario: datetimeFormat is unconstrained
- **WHEN** a profile declares `datetimeFormat: {excelFormat:"yyyy/M/d HH:mm:ss", dbFormat:"yyyyMMddHHmmss"}`
- **THEN** profile loading SHALL succeed

### Requirement: Import parses Excel string cells via U then serializes to V

For each column governed by a temporal slot (whether profile-level or per-column override), the import path SHALL parse incoming Excel string cells using `excelFormat` (with `excelFormatFallback` as ordered alternates) and then re-serialize the resulting structured value via `dbFormat` before binding to SQL. Specifically:

1. If the source Excel cell is a string, the system SHALL attempt to parse it using `excelFormat`. On success, the parsed `QDate` / `QDateTime` / `QTime` SHALL proceed to step 3.
2. If parsing using `excelFormat` fails AND `excelFormatFallback` is declared, the system SHALL try each fallback format in declared order. The first format that succeeds determines the parsed value; control proceeds to step 3.
3. The parsed value SHALL be serialized to a `QString` using `dbFormat` and bound to the SQL statement in place of the original `QVariant`.

If all parsing attempts (primary + all fallback) fail, the system SHALL emit a row-level error with code `E_TIME_PARSE`. The failing column's payload value SHALL be excluded from the route's UPSERT and the row SHALL be treated as failed for that route (consistent with existing row-level error semantics; other routes on the same row continue subject to the existing error-cascade rule).

#### Scenario: Primary format succeeds
- **WHEN** Excel cell is `"2025-03-14"` and the effective `dateFormat.excelFormat = "yyyy-MM-dd"`, `dbFormat = "yyyy-MM-dd"`
- **THEN** the value bound to SQL SHALL be the string `"2025-03-14"` (parsed then re-serialized via dbFormat)

#### Scenario: Fallback rescues a non-primary format
- **WHEN** Excel cell is `"2025/3/14"`, `excelFormat = "yyyy-MM-dd"`, and `excelFormatFallback = ["yyyy/M/d","yyyy.M.d"]`
- **THEN** parsing SHALL succeed using `"yyyy/M/d"` from the fallback list and the value SHALL be serialized via `dbFormat`

#### Scenario: All formats fail produces row-level error
- **WHEN** Excel cell is `"abc"`, primary format and every fallback all fail to parse
- **THEN** a row-level error with code `E_TIME_PARSE` SHALL be emitted, the row SHALL be skipped for the declaring route, and the error message SHALL include the cell's original string value

#### Scenario: Empty / null cell propagates as NULL
- **WHEN** Excel cell is null or an empty string for a column governed by a temporal slot
- **THEN** the system SHALL NOT attempt parsing; the bound SQL value SHALL be NULL; no row-level error SHALL be emitted (column-level NOT NULL validators, if any, run independently)

### Requirement: Native Excel date/time cells bypass U

When the Excel cell read by `ExcelReader` is already a structured temporal value (`QVariant::type()` equals `QVariant::Date`, `QVariant::DateTime`, or `QVariant::Time`), the system SHALL NOT apply `excelFormat` parsing. Instead, the structured value SHALL proceed directly to V serialization (import) or directly to the equivalent step in the export path. `excelFormatFallback` SHALL also be bypassed in this case.

#### Scenario: Native QDate cell goes straight to dbFormat
- **WHEN** Excel returns a `QVariant(QDate(2025,3,14))` for a column governed by `dateFormat: {excelFormat:"yyyy-MM-dd", dbFormat:"yyyy/M/d"}`
- **THEN** the value bound to SQL SHALL be `"2025/3/14"` (no `fromString` invoked)

#### Scenario: Native QDateTime cell goes straight to dbFormat
- **WHEN** Excel returns a `QVariant(QDateTime)` for a column governed by `datetimeFormat`
- **THEN** the structured value SHALL be serialized via `datetimeFormat.dbFormat` and bound to SQL

#### Scenario: Native QTime cell goes straight to dbFormat
- **WHEN** Excel returns a `QVariant(QTime(14,30))` for a column governed by `timeFormat: {dbFormat:"HH:mm:ss"}`
- **THEN** the value bound to SQL SHALL be `"14:30:00"`

### Requirement: Export parses DB via V then serializes to Excel via U

For each output column corresponding to a profile column governed by a temporal slot, the export path SHALL parse the DB string value using `dbFormat` and then re-serialize the structured value using `excelFormat` before writing to the Excel cell. Specifically:

1. The DB value (received as `QVariant` from `QSqlQuery::value`) SHALL be inspected. If it is already a structured temporal value (`QVariant::Date` / `DateTime` / `Time`), it SHALL bypass V-parsing and proceed to step 3.
2. Otherwise (typically a `QString` from a TEXT-affinity column), the system SHALL parse it using `dbFormat`. On failure, the system SHALL emit a row-level error with code `E_TIME_PARSE_DB`, write `NULL` (empty cell) to the Excel output for that single cell, and continue processing the rest of the row.
3. The parsed (or natively structured) value SHALL be serialized to a `QString` using `excelFormat` and written to the Excel cell.

`excelFormatFallback` SHALL NOT be consulted on the export path.

The export row SHALL NOT be aborted by `E_TIME_PARSE_DB`; other columns of the same row SHALL still be written.

#### Scenario: DB string parses via V then writes via U
- **WHEN** DB value is `"2025-03-14"`, `dbFormat = "yyyy-MM-dd"`, `excelFormat = "yyyy/M/d"`
- **THEN** the Excel cell SHALL contain `"2025/3/14"`

#### Scenario: DB string fails V; cell NULL, row continues
- **WHEN** DB value is `"14/03/2025"` and `dbFormat = "yyyy-MM-dd"` (parse fails)
- **THEN** a row-level error with code `E_TIME_PARSE_DB` SHALL be emitted, the Excel cell for that column SHALL be written as NULL (empty), and other columns of the same DB row SHALL still be written

#### Scenario: DB returns native QDateTime
- **WHEN** DB returns a `QVariant(QDateTime)` (some Qt drivers do this) for a column governed by `datetimeFormat: {excelFormat:"yyyy/M/d HH:mm"}`
- **THEN** V parsing SHALL be skipped and the value SHALL be serialized via `excelFormat` directly

#### Scenario: NULL DB value writes empty Excel cell
- **WHEN** the DB column value is NULL
- **THEN** the Excel cell SHALL be empty and no row-level error SHALL be emitted

### Requirement: Compatibility with `date:fmt` validator

The pre-existing `validators: ["date:fmt"]` token SHALL continue to function. The interaction with the new `dateFormat` slot SHALL follow these rules:

1. **Both declared on the same column** → the new `dateFormat` SHALL take precedence. The `date:fmt` validator SHALL still run but SHALL behave as a pass-through for values that are already `QDate` / `QDateTime` (its existing fast-path at `Validators.cpp:133`); the `excelFormat` of `dateFormat` SHALL be the actual parser. Profile loading SHALL emit an info-level diagnostic noting that `dateFormat` overrides `date:fmt`.
2. **Only `validators: ["date:fmt"]`, no `dateFormat`** → legacy behavior: parsing uses `fmt`; the DB-side format is the implicit ISO `"yyyy-MM-dd"`; no export-side V parsing occurs (current behavior); no `excelFormatFallback` available.
3. **Only `dateFormat`, no `date:fmt`** → the system SHALL behave **as if** an implicit `date:dateFormat.excelFormat` validator were attached for purposes of type assertion in downstream code paths that key off validator presence; this implicit validator SHALL NOT introduce additional failure modes beyond those already specified by this capability.
4. `datetimeFormat` and `timeFormat` SHALL NOT have any validator-compat handling (no `datetime:` / `time:` validators exist).

#### Scenario: Legacy validator-only column unchanged
- **WHEN** a column declares only `validators: ["date:yyyy-MM-dd"]` and the profile declares no `dateFormat`
- **THEN** import SHALL parse Excel strings via `yyyy-MM-dd` and DB-side storage SHALL fall back to the implicit ISO format (current behavior); export SHALL NOT apply any V-parsing

#### Scenario: Both declared; new slot wins
- **WHEN** a column declares `dateFormat: {excelFormat:"yyyy/M/d", dbFormat:"yyyy-MM-dd"}` AND `validators: ["date:yyyy-MM-dd"]`
- **THEN** import SHALL parse Excel strings using `"yyyy/M/d"` (from dateFormat), store via `"yyyy-MM-dd"` (from dateFormat), and profile loading SHALL emit an info-level diagnostic mentioning both declarations

#### Scenario: Only new slot; downstream type assertion still works
- **WHEN** a column declares `dateFormat: {excelFormat:"yyyy-MM-dd", dbFormat:"yyyy-MM-dd"}` and no `date:` validator
- **THEN** downstream code that previously keyed off the validator's presence (e.g. for schema-aware coercion) SHALL behave as if `validators: ["date:yyyy-MM-dd"]` were present

### Requirement: Warning for orderBy on non-lexicographically-sortable dbFormat

At profile load time, for each entry in `exportSpec.orderBy` whose stripped column name (after dropping any `table.` prefix) matches a `ColumnSpec.dbColumn` governed by a temporal slot, the system SHALL inspect the effective `dbFormat`. If the `dbFormat` does not begin with the literal token `yyyy` (heuristic for "year-leading, lexicographically time-ordered"), the system SHALL emit a warning with code `W_TIME_ORDERBY_NONSORTABLE`.

The warning SHALL NOT block profile loading. The warning message SHALL include the column name, the offending `dbFormat`, and the rationale "lexicographic order may differ from chronological order".

#### Scenario: orderBy on yyyy-MM-dd column produces no warning
- **WHEN** `exportSpec.orderBy = ["created_at"]` and `created_at` has `dateFormat.dbFormat = "yyyy-MM-dd"`
- **THEN** profile loading SHALL succeed with no warning for this column

#### Scenario: orderBy on dd/MM/yyyy column produces warning
- **WHEN** `exportSpec.orderBy = ["created_at"]` and `created_at` has `dateFormat.dbFormat = "dd/MM/yyyy"`
- **THEN** profile loading SHALL succeed and SHALL emit `W_TIME_ORDERBY_NONSORTABLE` mentioning `created_at` and `dd/MM/yyyy`

#### Scenario: orderBy column not under a temporal slot produces no warning
- **WHEN** `exportSpec.orderBy = ["order_no"]` and `order_no` is a plain string column (no `dateFormat` / `datetimeFormat` / `timeFormat`)
- **THEN** no warning of this kind SHALL be emitted

### Requirement: Failure semantics are row-level and decoupled by direction

Time-format failures SHALL NEVER abort an entire sheet's import or export. The system SHALL use two distinct error codes:

- `E_TIME_PARSE` — import-direction parse failure (Excel string did not match `excelFormat` or any fallback). The row SHALL be skipped for the declaring route; the existing fk-injection error-cascade rule SHALL apply for child routes.
- `E_TIME_PARSE_DB` — export-direction parse failure (DB string did not match `dbFormat`). The single offending cell SHALL be written as NULL and the rest of the row SHALL continue.

Both codes SHALL include the column name, the offending raw value, and the format string that was being applied.

#### Scenario: Import E_TIME_PARSE skips row but continues sheet
- **WHEN** row 5 of a 100-row import has a date cell that fails parsing
- **THEN** the import SHALL emit one `E_TIME_PARSE` row-level error referring to row 5, SHALL skip row 5 for the declaring route, and SHALL process rows 1-4 and 6-100 normally

#### Scenario: Export E_TIME_PARSE_DB writes NULL but completes row
- **WHEN** the 5th DB row has a malformed date value
- **THEN** the export SHALL emit one `E_TIME_PARSE_DB` row-level error referring to that row, SHALL write the offending cell as empty, and SHALL write the row's other cells (and subsequent rows) normally

#### Scenario: Empty / null at boundary is never an E_TIME_* error
- **WHEN** an Excel cell or DB value is null/empty for a temporal column
- **THEN** neither `E_TIME_PARSE` nor `E_TIME_PARSE_DB` SHALL be emitted; the value SHALL pass through as NULL
