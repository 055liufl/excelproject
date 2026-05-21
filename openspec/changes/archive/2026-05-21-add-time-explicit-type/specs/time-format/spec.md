## MODIFIED Requirements

### Requirement: Three independent temporal format slots

A profile SHALL be able to declare three independent temporal format objects at the profile root: `dateFormat`, `datetimeFormat`, `timeFormat`. Each object describes how values at the Excel boundary and the SQLite boundary are encoded.

Two equivalent JSON forms SHALL be supported for any slot:

**Legacy flat form** (existing, permanently preserved as a shorthand):

```jsonc
"datetimeFormat": {
  "excelFormat":         "yyyy-MM-dd HH:mm:ss",
  "dbFormat":            "yyyy-MM-dd HH:mm:ss",
  "excelFormatFallback": ["d/M/yyyy H:mm"]
}
```

The legacy flat form is semantically equivalent to:

```jsonc
"datetimeFormat": {
  "excel": { "type": "string", "format": "yyyy-MM-dd HH:mm:ss", "fallback": ["d/M/yyyy H:mm"] },
  "db":    { "type": "string", "format": "yyyy-MM-dd HH:mm:ss" }
}
```

**New side-object form**:

Each slot MAY declare `excel` and/or `db` sub-objects. Each sub-object has shape `{ type, format?, fallback? }`. `fallback` SHALL only appear on the `excel` side and SHALL only be honored on the Excel → in-memory parsing direction (import); export SHALL ignore it.

The three slots SHALL be processed independently and SHALL NOT share or fall back to each other's values.

#### Scenario: All three slots declared at profile root via legacy form
- **WHEN** `profile.json` declares `dateFormat: {excelFormat:"yyyy/M/d", dbFormat:"yyyy-MM-dd"}`, `datetimeFormat: {excelFormat:"yyyy/M/d HH:mm:ss", dbFormat:"yyyy-MM-dd HH:mm:ss"}`, and `timeFormat: {excelFormat:"HH:mm", dbFormat:"HH:mm:ss"}`
- **THEN** profile loading SHALL succeed and SHALL produce three independent in-memory configurations, each normalized to `{excel:{type:"string", format:...}, db:{type:"string", format:...}}`

#### Scenario: Slot declared via new side-object form
- **WHEN** a profile declares `datetimeFormat: {"excel": {"type":"string","format":"yyyy-MM-dd HH:mm:ss"}, "db": {"type":"epochSec"}}`
- **THEN** profile loading SHALL succeed and the effective slot SHALL carry `excel.type="string"` with the given format and `db.type="epochSec"` with no format

#### Scenario: Only one slot declared
- **WHEN** a profile declares only `dateFormat` and omits `datetimeFormat` / `timeFormat`
- **THEN** the absent slots SHALL remain unset and SHALL NOT inherit values from the declared slot

#### Scenario: excelFormatFallback in legacy form is array-only
- **WHEN** a slot declares `excelFormatFallback` as a non-array (e.g. `"yyyy-MM-dd"` as a bare string)
- **THEN** profile loading SHALL fail with a message identifying the offending slot and the expected array form

#### Scenario: fallback in side-object form is array-only
- **WHEN** a slot declares `excel.fallback` as a non-array
- **THEN** profile loading SHALL fail with a message identifying the offending slot and the expected array form

---

### Requirement: Per-column override with side-level integral overwrite

A `ColumnSpec` SHALL be able to redeclare any of `dateFormat`, `datetimeFormat`, `timeFormat`. When a column redeclares a slot, the effective slot for that column SHALL be computed by **side-level integral overwrite**:

- If the column-level slot declares the `excel` sub-object (or any of the legacy `excelFormat` / `excelFormatFallback`), the entire `excel` side SHALL replace the profile-level `excel` (the column-level `type` / `format` / `fallback` are taken as a unit; no field-level inheritance).
- The `db` side follows the same rule independently of `excel`.
- If the column-level slot omits a side entirely, that side SHALL be inherited verbatim from the profile-level slot.

If a column declares no temporal slot, the column SHALL use the profile-level slot verbatim.

If neither the column nor the profile declares a slot relevant to that column's type, the legacy compatibility rule (see Requirement "Compatibility with `date:fmt` validator") SHALL apply.

Field-level merge across `type` / `format` / `fallback` is explicitly NOT supported. Users wishing to change only one field on a side SHALL re-declare the other fields of that side as well.

#### Scenario: Column overrides only excel side; db inherits
- **WHEN** profile declares `datetimeFormat: {"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}, "db":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}}` and column X declares `datetimeFormat: {"excel":{"type":"string","format":"yyyy/M/d H:mm"}}`
- **THEN** the effective `datetimeFormat` for column X SHALL be `{excel:{type:"string", format:"yyyy/M/d H:mm"}, db:{type:"string", format:"yyyy-MM-dd HH:mm:ss"}}`

#### Scenario: Column overrides db.type to epochSec; excel inherits
- **WHEN** profile declares `datetimeFormat: {"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}, "db":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}}` and column X declares `datetimeFormat: {"db":{"type":"epochSec"}}`
- **THEN** the effective `datetimeFormat` for column X SHALL be `{excel:{type:"string", format:"yyyy-MM-dd HH:mm:ss"}, db:{type:"epochSec", format:""}}`; the inherited db.format SHALL NOT be copied (side-level replacement, not field-level)

#### Scenario: Mixed legacy and new across levels — profile legacy, column new
- **WHEN** profile declares `datetimeFormat: {"excelFormat":"yyyy-MM-dd HH:mm:ss", "dbFormat":"yyyy-MM-dd HH:mm:ss"}` and column X declares `datetimeFormat: {"db":{"type":"epochSec"}}`
- **THEN** the profile-level legacy form SHALL be normalized to side-object form first, then column X's `db` side SHALL replace it; the effective spec SHALL be `{excel:{type:"string", format:"yyyy-MM-dd HH:mm:ss"}, db:{type:"epochSec"}}`

#### Scenario: Column declares no temporal slot
- **WHEN** a column declares no `dateFormat` / `datetimeFormat` / `timeFormat`
- **THEN** the column SHALL use the profile-level slot verbatim

---

### Requirement: Format token validation per slot type

Profile loading SHALL reject formats whose tokens are incompatible with the slot's intrinsic type. Specifically, when the side's `type` is `"string"`:

- `dateFormat.excel.format` and `dateFormat.db.format` (or their legacy `excelFormat`/`dbFormat` equivalents) SHALL NOT contain time tokens (`H`, `h`, `m`, `s`, `z`, `t`, `a`, `A`, `AP`, `ap`). The check is structural (presence in the format string), not semantic.
- `timeFormat.excel.format` and `timeFormat.db.format` SHALL NOT contain date tokens (`y`, `M`, `d`).
- `datetimeFormat.excel.format` and `datetimeFormat.db.format` are unconstrained.
- `excel.fallback` (or legacy `excelFormatFallback`) entries SHALL be checked against the same rules as the slot's `excel.format`.

When a side's `type` is not `"string"` (e.g. `"epochSec"`), token validation SHALL be skipped for that side; `format` is required to be empty by a separate requirement.

#### Scenario: dateFormat string side contains hour token
- **WHEN** a profile declares `dateFormat: {"excel": {"type":"string","format":"yyyy-MM-dd HH:mm"}, "db":{"type":"string","format":"yyyy-MM-dd"}}`
- **THEN** profile loading SHALL fail with a message that names the offending slot (`dateFormat.excel.format`) and the offending token

#### Scenario: timeFormat string side contains date token
- **WHEN** a profile declares `timeFormat: {"excel":{"type":"string","format":"yyyy HH:mm"}, "db":{"type":"string","format":"HH:mm:ss"}}`
- **THEN** profile loading SHALL fail with a message that names the offending slot

#### Scenario: datetimeFormat string side is unconstrained
- **WHEN** a profile declares `datetimeFormat: {"excel":{"type":"string","format":"yyyy/M/d HH:mm:ss"}, "db":{"type":"string","format":"yyyyMMddHHmmss"}}`
- **THEN** profile loading SHALL succeed

#### Scenario: epochSec side bypasses token validation
- **WHEN** a profile declares `datetimeFormat.db: {"type":"epochSec"}` (no format)
- **THEN** profile loading SHALL succeed and token validation SHALL be skipped for `db`

---

### Requirement: Import parses incoming Excel cells per excel-side spec and binds DB per db-side spec

For each column governed by a temporal slot (whether profile-level or per-column override), the import path SHALL behave as follows.

**Excel → structured value (parse step)** — driven by the effective `excel` side spec:

1. If the source Excel cell is empty or null, the bound SQL value SHALL be NULL; no parsing SHALL be attempted; no row-level error SHALL be emitted.
2. If the source Excel cell is already a structured temporal value (`QVariant::Date` / `DateTime` / `Time`), it SHALL bypass the parse step (see Requirement "Native Excel date/time cells bypass U").
3. If `excel.type == "string"`, the system SHALL attempt to parse the cell as a string using `excel.format`. On failure, the system SHALL try each `excel.fallback` entry in declared order. The first format that succeeds determines the structured value.
4. (Reserved for future `excel.type` values; v1 only supports `"string"`.)

**Structured value → SQL bind (serialize step)** — driven by the effective `db` side spec:

5. If `db.type == "string"`, the structured value SHALL be serialized to a `QString` using `db.format` and bound to the SQL statement.
6. If `db.type == "epochSec"`, the structured value SHALL be converted to seconds since Unix epoch via `QDateTime::toSecsSinceEpoch()` and bound as `QVariant(qlonglong)`. The system SHALL rely on SQLite type affinity to store the value in an INTEGER column.

If any step (parse or serialize) fails to produce a usable value (e.g. all parse attempts fail, or `db.type == "epochSec"` but the structured value is not a `QDateTime`), the system SHALL emit a row-level error with code `E_TIME_PARSE`. The failing column's payload value SHALL be excluded from the route's UPSERT and the row SHALL be treated as failed for that route (consistent with existing row-level error semantics).

#### Scenario: Primary format succeeds, serialized via dbFormat string
- **WHEN** Excel cell is `"2025-03-14"` and the effective slot is `{excel:{type:"string", format:"yyyy-MM-dd"}, db:{type:"string", format:"yyyy-MM-dd"}}`
- **THEN** the value bound to SQL SHALL be the `QString` `"2025-03-14"`

#### Scenario: Primary format succeeds, serialized as epochSec
- **WHEN** Excel cell is `"2024-05-21 10:00:00"` and the effective slot is `datetimeFormat: {excel:{type:"string", format:"yyyy-MM-dd HH:mm:ss"}, db:{type:"epochSec"}}`
- **THEN** the value bound to SQL SHALL be a `QVariant(qlonglong)` equal to `QDateTime(2024-05-21 10:00:00 local).toSecsSinceEpoch()`

#### Scenario: Fallback rescues a non-primary format
- **WHEN** Excel cell is `"2025/3/14"`, `excel.format = "yyyy-MM-dd"`, `excel.fallback = ["yyyy/M/d","yyyy.M.d"]`
- **THEN** parsing SHALL succeed using `"yyyy/M/d"` from the fallback list and serialization SHALL proceed per the `db` side spec

#### Scenario: All formats fail produces row-level error
- **WHEN** Excel cell is `"abc"`, primary format and every fallback all fail to parse
- **THEN** a row-level error with code `E_TIME_PARSE` SHALL be emitted, the row SHALL be skipped for the declaring route, and the error message SHALL include the cell's original string value

#### Scenario: Empty / null cell propagates as NULL
- **WHEN** Excel cell is null or an empty string for a column governed by a temporal slot
- **THEN** the system SHALL NOT attempt parsing; the bound SQL value SHALL be NULL; no row-level error SHALL be emitted

---

### Requirement: Export parses DB per db-side spec and serializes Excel per excel-side spec

For each output column corresponding to a profile column governed by a temporal slot, the export path SHALL behave as follows.

**SQL → structured value (parse step)** — driven by the effective `db` side spec:

1. If the DB value is SQL NULL (`QVariant().isNull() == true`), the Excel cell SHALL be empty; no parsing SHALL be attempted; no row-level error SHALL be emitted.
2. If the DB value is already a structured temporal value (`QVariant::Date` / `DateTime` / `Time`), it SHALL bypass V-parsing and proceed to the serialize step.
3. If `db.type == "string"`, the system SHALL parse the value's string representation using `db.format`. On failure, the system SHALL emit a row-level error with code `E_TIME_PARSE_DB`, write NULL (empty cell), and continue with the rest of the row.
4. If `db.type == "epochSec"`, the system SHALL convert the value to `qlonglong` and call `QDateTime::fromSecsSinceEpoch(secs)`. If conversion fails (e.g. non-numeric value, or out-of-range yielding invalid `QDateTime`), the system SHALL emit `E_TIME_PARSE_DB`, write NULL, and continue. The value `qlonglong(0)` SHALL convert successfully to `1970-01-01T00:00:00` and SHALL NOT be treated as null.

**Structured value → Excel cell (serialize step)** — driven by the effective `excel` side spec:

5. If `excel.type == "string"`, the structured value SHALL be serialized to a `QString` using `excel.format` and written to the Excel cell.
6. (Reserved for future `excel.type` values; v1 only supports `"string"`.)

`excel.fallback` SHALL NOT be consulted on the export path. The export row SHALL NOT be aborted by `E_TIME_PARSE_DB`; other columns of the same row SHALL still be written.

#### Scenario: DB string parses via dbFormat, writes Excel string
- **WHEN** DB value is `"2025-03-14"`, effective `{db:{type:"string", format:"yyyy-MM-dd"}, excel:{type:"string", format:"yyyy/M/d"}}`
- **THEN** the Excel cell SHALL contain `"2025/3/14"`

#### Scenario: DB INTEGER epochSec converts via fromSecsSinceEpoch
- **WHEN** DB value is `QVariant(qlonglong, 1716286800)`, effective `datetimeFormat: {db:{type:"epochSec"}, excel:{type:"string", format:"yyyy-MM-dd HH:mm:ss"}}`
- **THEN** the Excel cell SHALL contain the string serialization of `QDateTime::fromSecsSinceEpoch(1716286800)` using `"yyyy-MM-dd HH:mm:ss"` (in local time)

#### Scenario: epochSec value 0 maps to Unix epoch start
- **WHEN** DB value is `QVariant(qlonglong, 0)`, effective `db.type=epochSec` and `excel.format="yyyy-MM-dd HH:mm:ss"`
- **THEN** the Excel cell SHALL contain `"1970-01-01 00:00:00"` (in local time); the value SHALL NOT be written as empty

#### Scenario: DB string fails V; cell NULL, row continues
- **WHEN** DB value is `"14/03/2025"` and `db.format = "yyyy-MM-dd"` (parse fails)
- **THEN** a row-level error with code `E_TIME_PARSE_DB` SHALL be emitted, the Excel cell SHALL be empty, and other columns of the same DB row SHALL still be written

#### Scenario: DB INTEGER non-parsable as epoch
- **WHEN** DB value is a `QString` `"not a number"` but `db.type=epochSec`
- **THEN** a row-level error with code `E_TIME_PARSE_DB` SHALL be emitted, the Excel cell SHALL be empty, and other columns SHALL still be written

#### Scenario: NULL DB value writes empty Excel cell
- **WHEN** the DB column value is SQL NULL (`QVariant().isNull() == true`)
- **THEN** the Excel cell SHALL be empty and no row-level error SHALL be emitted

---

### Requirement: Warning for orderBy on non-lexicographically-sortable dbFormat

At profile load time, for each entry in `exportSpec.orderBy` whose stripped column name (after dropping any `table.` prefix) matches a `ColumnSpec.dbColumn` governed by a temporal slot, the system SHALL inspect the effective `db` side spec:

- If the effective `db.type != "string"` (e.g. `"epochSec"`), no `W_TIME_ORDERBY_NONSORTABLE` warning SHALL be emitted. INTEGER columns are sorted numerically by SQLite, which preserves chronological order automatically.
- If the effective `db.type == "string"` and `db.format` does not begin with the literal token `yyyy` (heuristic for "year-leading, lexicographically time-ordered"), the system SHALL emit a warning with code `W_TIME_ORDERBY_NONSORTABLE`.

The warning SHALL NOT block profile loading. The warning message SHALL include the column name, the offending `db.format`, and the rationale "lexicographic order may differ from chronological order".

#### Scenario: orderBy on yyyy-MM-dd string column produces no warning
- **WHEN** `exportSpec.orderBy = ["created_at"]` and `created_at` has effective `{db:{type:"string", format:"yyyy-MM-dd"}}`
- **THEN** profile loading SHALL succeed with no warning for this column

#### Scenario: orderBy on dd/MM/yyyy string column produces warning
- **WHEN** `exportSpec.orderBy = ["created_at"]` and `created_at` has effective `{db:{type:"string", format:"dd/MM/yyyy"}}`
- **THEN** profile loading SHALL succeed and SHALL emit `W_TIME_ORDERBY_NONSORTABLE` mentioning `created_at` and `dd/MM/yyyy`

#### Scenario: orderBy on epochSec column produces no warning
- **WHEN** `exportSpec.orderBy = ["happen_at"]` and `happen_at` has effective `{db:{type:"epochSec"}}`
- **THEN** profile loading SHALL succeed and SHALL NOT emit `W_TIME_ORDERBY_NONSORTABLE` for `happen_at`

#### Scenario: orderBy column not under a temporal slot produces no warning
- **WHEN** `exportSpec.orderBy = ["order_no"]` and `order_no` is a plain string column (no temporal slot)
- **THEN** no warning of this kind SHALL be emitted

---

## ADDED Requirements

### Requirement: Per-side type field with enumerated values

Each side (`excel` or `db`) of a temporal slot MAY declare a `type` field. The `type` field SHALL hold one of an enumerated set of string values. v1 SHALL recognize:

| Value | Allowed on `excel` side | Allowed on `db` side |
|---|---|---|
| `"string"` (default) | ✓ | ✓ |
| `"epochSec"` | ✗ (rejected with `E_PROFILE_PARSE`) | ✓ (only under `datetimeFormat`; see separate requirement) |

If `type` is omitted from a declared side, the implicit value SHALL be `"string"`.

Unknown `type` values SHALL be rejected at profile load time with `E_PROFILE_PARSE` and a message naming the slot, side, and offending value. Lenient parsing (silently accepting unknown values as `"string"`) SHALL NOT be permitted; this preserves forward-compatible error behavior when older library versions load profiles authored against newer schemas.

#### Scenario: type defaults to string when omitted
- **WHEN** a profile declares `datetimeFormat: {"excel": {"format":"yyyy-MM-dd HH:mm:ss"}, "db": {"format":"yyyy-MM-dd HH:mm:ss"}}` (no explicit `type` on either side)
- **THEN** profile loading SHALL succeed and both sides SHALL carry `type="string"`

#### Scenario: Unknown type value rejected
- **WHEN** a profile declares `datetimeFormat.db: {"type":"foobar"}`
- **THEN** profile loading SHALL fail with `E_PROFILE_PARSE` and a message naming the slot (`datetimeFormat.db`) and the offending value (`"foobar"`)

#### Scenario: excel.type = epochSec is rejected in v1
- **WHEN** a profile declares `datetimeFormat.excel: {"type":"epochSec"}`
- **THEN** profile loading SHALL fail with `E_PROFILE_PARSE` and a message stating that `epochSec` is not supported on the Excel side in v1

---

### Requirement: epochSec restricted to datetime slot on db side

The `"epochSec"` type SHALL only be permitted on `datetimeFormat.db`. Other slot × side combinations SHALL be rejected at profile load time:

| Combination | Decision |
|---|---|
| `dateFormat.db.type = "epochSec"` | `E_PROFILE_PARSE` |
| `timeFormat.db.type = "epochSec"` | `E_PROFILE_PARSE` |
| `<any-slot>.excel.type = "epochSec"` | `E_PROFILE_PARSE` |
| `datetimeFormat.db.type = "epochSec"` | accepted |

Rationale: Unix epoch seconds semantically represent an instant in time (a `QDateTime`). Mapping epoch to `QDate` or `QTime` introduces timezone-extraction ambiguity that v1 explicitly defers.

#### Scenario: datetimeFormat.db.type = epochSec accepted
- **WHEN** a profile declares `datetimeFormat: {"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}, "db":{"type":"epochSec"}}`
- **THEN** profile loading SHALL succeed

#### Scenario: dateFormat.db.type = epochSec rejected
- **WHEN** a profile declares `dateFormat: {"db":{"type":"epochSec"}}`
- **THEN** profile loading SHALL fail with `E_PROFILE_PARSE` and a message that `epochSec` is restricted to `datetimeFormat` slot

#### Scenario: timeFormat.db.type = epochSec rejected
- **WHEN** a profile declares `timeFormat: {"db":{"type":"epochSec"}}`
- **THEN** profile loading SHALL fail with `E_PROFILE_PARSE` and a message that `epochSec` is restricted to `datetimeFormat` slot

---

### Requirement: Legacy and new schema coexistence rules

Within a single slot JSON object (a single `dateFormat`, `datetimeFormat`, or `timeFormat` instance), legacy flat-form fields (`excelFormat`, `dbFormat`, `excelFormatFallback`) and new side-object fields (`excel`, `db`) SHALL NOT both be present.

Across profile and column levels, mixing forms IS permitted: a profile-level slot MAY use legacy flat form while a column-level override uses new side-object form (or vice versa). When the levels mix, the loader SHALL first normalize the legacy form into side-object form (with `type="string"`) and then apply per-column side-level overwrite as defined in "Per-column override with side-level integral overwrite".

#### Scenario: Same-slot mixing rejected
- **WHEN** a profile declares `datetimeFormat: {"excelFormat":"yyyy-MM-dd HH:mm:ss", "db":{"type":"epochSec"}}`
- **THEN** profile loading SHALL fail with `E_PROFILE_PARSE` and a message stating that legacy `excelFormat`/`dbFormat`/`excelFormatFallback` SHALL NOT coexist with new `excel`/`db` sub-objects within the same slot object

#### Scenario: Cross-level mixing — profile legacy, column new
- **WHEN** profile declares `datetimeFormat: {"excelFormat":"yyyy-MM-dd HH:mm:ss", "dbFormat":"yyyy-MM-dd HH:mm:ss"}` and column X declares `datetimeFormat: {"db":{"type":"epochSec"}}`
- **THEN** profile loading SHALL succeed; the effective slot for column X SHALL be `{excel:{type:"string", format:"yyyy-MM-dd HH:mm:ss"}, db:{type:"epochSec"}}`

#### Scenario: Cross-level mixing — profile new, column legacy
- **WHEN** profile declares `datetimeFormat: {"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}, "db":{"type":"epochSec"}}` and column X declares `datetimeFormat: {"dbFormat":"yyyy-MM-dd HH:mm:ss"}`
- **THEN** profile loading SHALL succeed; column X's legacy `dbFormat` SHALL be normalized to `{db:{type:"string", format:"yyyy-MM-dd HH:mm:ss"}}` and SHALL replace the profile-level `db` side; the effective slot SHALL be `{excel:{type:"string", format:"yyyy-MM-dd HH:mm:ss"}, db:{type:"string", format:"yyyy-MM-dd HH:mm:ss"}}`

---

### Requirement: Empty object and default values

Within the new side-object form, partial declarations SHALL be permitted and behave as follows:

- `"<slot>": {}` (empty slot object) — the slot is `declared` (carries presence semantics for downstream logic such as `temporalSlotKindFor`), but neither `excel` nor `db` side is declared. Both sides SHALL inherit from any applicable profile-level slot (column-level case) or remain undeclared (profile-level case).
- `"<slot>": {"excel": {}}` (empty side object) — the `excel` side is `declared`, and its fields SHALL take their default values: `type = "string"`, `format = ""`, `fallback = []`. The same rule applies to `"db": {}`.
- A side's `type` SHALL default to `"string"` whenever the side is declared but `type` is omitted.

The legacy "dummy declaration" idiom `"timeFormat": {}` SHALL continue to mark the slot as declared without contributing concrete format values.

#### Scenario: Empty slot object is declared
- **WHEN** a column declares `"timeFormat": {}`
- **THEN** the slot SHALL be marked as `declared` for the purpose of slot-kind resolution; both `excel` and `db` sides SHALL inherit from the profile-level slot (if any)

#### Scenario: Empty side object defaults to string with empty format
- **WHEN** a profile declares `"datetimeFormat": {"excel": {}, "db": {}}`
- **THEN** both sides SHALL be `declared`; both SHALL carry `type="string"`; both SHALL carry empty `format`; effective spec validity SHALL be evaluated by the separate "Type × format consistency validation" requirement

---

### Requirement: Format and fallback empty value definition

For both `format` (string) and `fallback` (array of strings), and on both legacy flat fields and new side-object fields, "empty" SHALL be defined as:

- Field absent from JSON, OR
- Empty string `""` for `format`, OR
- Empty array `[]` for `fallback`

JSON explicit `null` SHALL NOT be treated as empty. A `null` value SHALL be rejected with `E_PROFILE_PARSE` and a message naming the offending field and the expected type.

This applies to `format`, `excelFormat`, `dbFormat`, `fallback`, and `excelFormatFallback`.

#### Scenario: Absent format treated as empty
- **WHEN** a profile declares `datetimeFormat.db: {"type":"epochSec"}` (no `format` field)
- **THEN** `db.format` SHALL be treated as empty

#### Scenario: Empty-string format treated as empty
- **WHEN** a profile declares `datetimeFormat.db: {"type":"epochSec", "format":""}`
- **THEN** `db.format` SHALL be treated as empty (equivalent to absent)

#### Scenario: JSON null format rejected
- **WHEN** a profile declares `datetimeFormat.db: {"type":"string", "format": null}`
- **THEN** profile loading SHALL fail with `E_PROFILE_PARSE` and a message stating that `format` must be a string, not `null`

#### Scenario: JSON null fallback rejected
- **WHEN** a profile declares `datetimeFormat.excel: {"fallback": null}`
- **THEN** profile loading SHALL fail with `E_PROFILE_PARSE` and a message stating that `fallback` must be an array, not `null`

---

### Requirement: Type × format consistency validation

After computing the effective spec for each column (i.e. after performing side-level integral overwrite between profile-level and column-level slots), the system SHALL validate the `type` and `format` combination on each side:

- When `type == "string"`, the effective `format` SHALL be non-empty. An empty format combined with `type=="string"` SHALL trigger `E_PROFILE_PARSE` naming the column, slot, side, and the absence of `format`.
- When `type == "epochSec"`, the effective `format` SHALL be empty. A non-empty format combined with `type=="epochSec"` SHALL trigger `E_PROFILE_PARSE` naming the column, slot, side, and the offending `format` value.

This validation operates on the **effective** spec (post-merge) rather than the raw JSON declaration. Side-level integral overwrite ensures that the column's declared `type` always travels with its own `format` / `fallback`, so a column that overrides only the `db.type` SHALL also override `db.format` (typically to empty) in the same declaration; otherwise the effective spec would inherit incompatible values from the profile level and trip this validation.

#### Scenario: type=string with no format anywhere rejected
- **WHEN** a profile declares `datetimeFormat: {"excel":{"type":"string"}, "db":{"type":"string"}}` (no format on either side, no profile-level legacy format either) and column X has no override
- **THEN** profile loading SHALL fail with `E_PROFILE_PARSE` naming column X and both sides where `format` is empty under `type=string`

#### Scenario: type=string with profile format only succeeds
- **WHEN** profile declares `datetimeFormat: {"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}, "db":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}}` and column X declares no override
- **THEN** profile loading SHALL succeed

#### Scenario: type=epochSec with non-empty format rejected
- **WHEN** a profile declares `datetimeFormat.db: {"type":"epochSec", "format":"yyyy-MM-dd"}`
- **THEN** profile loading SHALL fail with `E_PROFILE_PARSE` naming the slot and stating that `format` must be empty when `type=epochSec`

#### Scenario: Side-level overwrite makes effective spec valid
- **WHEN** profile declares `datetimeFormat: {"excel":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}, "db":{"type":"string","format":"yyyy-MM-dd HH:mm:ss"}}` and column X declares `datetimeFormat: {"db":{"type":"epochSec"}}` (no format)
- **THEN** column X's `db` side SHALL replace the profile-level `db` integrally; effective `db = {type:"epochSec", format:""}` SHALL pass validation; profile loading SHALL succeed

---

### Requirement: A column declares at most one temporal slot

A `ColumnSpec` SHALL declare zero or one temporal slot among `dateFormat`, `datetimeFormat`, `timeFormat`. Declaring two or more on the same column SHALL be rejected at profile load time with `E_PROFILE_PARSE` and a message naming the column and the conflicting slot names.

This rule preserves the existing implicit invariant in `temporalSlotKindFor` and makes it an explicit, loader-enforced constraint.

#### Scenario: Single slot declaration accepted
- **WHEN** a column declares only `datetimeFormat: {...}` (no other temporal slot)
- **THEN** profile loading SHALL succeed

#### Scenario: Two slots on same column rejected
- **WHEN** a column declares both `dateFormat: {...}` and `datetimeFormat: {...}`
- **THEN** profile loading SHALL fail with `E_PROFILE_PARSE` naming the column and the conflicting slots `dateFormat` and `datetimeFormat`

#### Scenario: No temporal slot declared
- **WHEN** a column declares no temporal slot
- **THEN** profile loading SHALL succeed and the column SHALL be subject to the profile-level slot rules (or to the `date:fmt` validator legacy path, per the existing compatibility requirement)

---

### Requirement: NULL preserved as empty Excel cell distinct from epochSec zero

On the export path, `QVariant().isNull()` and `qlonglong(0)` SHALL be treated as semantically distinct values for temporal columns whose `db.type == "epochSec"`:

- A SQL NULL value (driver-returned `QVariant().isNull() == true`) SHALL produce an empty Excel cell; no parsing or conversion SHALL be attempted; no `E_TIME_PARSE_DB` SHALL be emitted.
- A value of `qlonglong(0)` SHALL be converted via `QDateTime::fromSecsSinceEpoch(0)` to `1970-01-01T00:00:00` (local time per `QDateTime` defaults) and SHALL be written to the Excel cell using the effective `excel.format`. It SHALL NOT be treated as empty or null.

This distinction is required because both values toString as `""` or `"0"` under different code paths; the implementation SHALL gate on `QVariant::isNull()` rather than on value-equality.

#### Scenario: SQL NULL produces empty Excel cell
- **WHEN** export retrieves a row where the temporal column's value is SQL NULL (Qt returns `QVariant()` with `isNull() == true`)
- **THEN** the Excel cell SHALL be empty; no `E_TIME_PARSE_DB` SHALL be emitted

#### Scenario: epochSec zero produces 1970 Excel cell
- **WHEN** export retrieves a row where the temporal column's value is `QVariant(qlonglong, 0)` and `db.type=epochSec` with `excel.format="yyyy-MM-dd HH:mm:ss"`
- **THEN** the Excel cell SHALL contain `"1970-01-01 00:00:00"` (in local time)

#### Scenario: epochSec negative produces pre-1970 Excel cell
- **WHEN** export retrieves a row where the temporal column's value is `QVariant(qlonglong, -86400)` and `db.type=epochSec` with `excel.format="yyyy-MM-dd"`
- **THEN** the Excel cell SHALL contain `"1969-12-31"` (in local time); the value SHALL NOT be treated as invalid
