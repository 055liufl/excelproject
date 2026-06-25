#pragma once

namespace dbridge::err {

inline constexpr const char* E_OPEN_DB = "E_OPEN_DB";
inline constexpr const char* E_OPEN_XLSX = "E_OPEN_XLSX";
inline constexpr const char* E_PROFILE_PARSE = "E_PROFILE_PARSE";
inline constexpr const char* E_PROFILE_TABLE_NOT_FOUND = "E_PROFILE_TABLE_NOT_FOUND";
inline constexpr const char* E_PROFILE_COLUMN_NOT_FOUND = "E_PROFILE_COLUMN_NOT_FOUND";
inline constexpr const char* E_PROFILE_NO_CONFLICT_KEY = "E_PROFILE_NO_CONFLICT_KEY";
inline constexpr const char* E_PROFILE_TOPOLOGY_CYCLE = "E_PROFILE_TOPOLOGY_CYCLE";
inline constexpr const char* E_HEADER_NOT_FOUND = "E_HEADER_NOT_FOUND";
inline constexpr const char* E_ROUTE_UNMATCHED = "E_ROUTE_UNMATCHED";
inline constexpr const char* E_VALIDATE_NULL = "E_VALIDATE_NULL";
inline constexpr const char* E_VALIDATE_TYPE = "E_VALIDATE_TYPE";
inline constexpr const char* E_VALIDATE_REGEX = "E_VALIDATE_REGEX";
inline constexpr const char* E_VALIDATE_DUPLICATE = "E_VALIDATE_DUPLICATE";
inline constexpr const char* E_VALIDATE_FK = "E_VALIDATE_FK";
inline constexpr const char* E_LOOKUP_KEY_EMPTY = "E_LOOKUP_KEY_EMPTY";
inline constexpr const char* E_LOOKUP_KEY_INVALID = "E_LOOKUP_KEY_INVALID";
inline constexpr const char* E_LOOKUP_NOT_FOUND = "E_LOOKUP_NOT_FOUND";
inline constexpr const char* E_LOOKUP_AMBIGUOUS = "E_LOOKUP_AMBIGUOUS";
inline constexpr const char* E_LOOKUP_QUERY_FAILED = "E_LOOKUP_QUERY_FAILED";
inline constexpr const char* E_DB_UPSERT = "E_DB_UPSERT";
inline constexpr const char* E_EXPORT_QUERY = "E_EXPORT_QUERY";
inline constexpr const char* E_WRITE_XLSX = "E_WRITE_XLSX";

// add-time-format-profile: temporal format I/O failures.
// E_TIME_PARSE     — Excel→memory parse failure (import direction); row-level, row skipped for
// route. E_TIME_PARSE_DB  — DB→memory parse failure (export direction); row-level, offending cell
// written NULL, row continues.
inline constexpr const char* E_TIME_PARSE = "E_TIME_PARSE";
inline constexpr const char* E_TIME_PARSE_DB = "E_TIME_PARSE_DB";
// Non-blocking warning — orderBy hits a temporal column whose dbFormat does not begin with `yyyy`
// (heuristic for "lexicographic order == chronological order"). Emitted by ProfileValidator.
inline constexpr const char* W_TIME_ORDERBY_NONSORTABLE = "W_TIME_ORDERBY_NONSORTABLE";

// add-export-column-order: columnOrder validation failures.
inline constexpr const char* E_EXPORT_UNKNOWN_HEADER = "E_EXPORT_UNKNOWN_HEADER";
inline constexpr const char* E_EXPORT_DUPLICATE_ORDER = "E_EXPORT_DUPLICATE_ORDER";
inline constexpr const char* E_EXPORT_ORDER_WITH_RAW_SQL = "E_EXPORT_ORDER_WITH_RAW_SQL";

// add-export-reverse-lookup: reverse-lookup export-direction failures.
// E_REVERSE_LOOKUP_NOT_FOUND   — row-level; zero G matches; governed by exportOnMissing.
// E_REVERSE_LOOKUP_AMBIGUOUS   — row-level; multiple G matches; always an error.
// E_REVERSE_LOOKUP_QUERY_FAILED — table-level; prefetch SELECT failed; sheet export aborted.
inline constexpr const char* E_REVERSE_LOOKUP_NOT_FOUND = "E_REVERSE_LOOKUP_NOT_FOUND";
inline constexpr const char* E_REVERSE_LOOKUP_AMBIGUOUS = "E_REVERSE_LOOKUP_AMBIGUOUS";
inline constexpr const char* E_REVERSE_LOOKUP_QUERY_FAILED = "E_REVERSE_LOOKUP_QUERY_FAILED";

// ── Sync engine error / fatal codes (v0.5) ───────────────────────────────────
inline constexpr const char* E_SYNC_INIT = "E_SYNC_INIT";
inline constexpr const char* E_SYNC_SESSION_UNAVAILABLE = "E_SYNC_SESSION_UNAVAILABLE";
inline constexpr const char* E_SYNC_SCHEMA_MISMATCH = "E_SYNC_SCHEMA_MISMATCH";
inline constexpr const char* E_SYNC_PAYLOAD_CORRUPT = "E_SYNC_PAYLOAD_CORRUPT";
inline constexpr const char* E_SYNC_TRANSPORT = "E_SYNC_TRANSPORT";
inline constexpr const char* E_SYNC_APPLY_FK = "E_SYNC_APPLY_FK";
inline constexpr const char* E_SYNC_APPLY_CONSTRAINT = "E_SYNC_APPLY_CONSTRAINT";
inline constexpr const char* E_SYNC_NODE_UNKNOWN = "E_SYNC_NODE_UNKNOWN";
inline constexpr const char* E_SYNC_GAP = "E_SYNC_GAP";
inline constexpr const char* E_SYNC_STAGE_STALE = "E_SYNC_STAGE_STALE";
inline constexpr const char* E_SYNC_STAGE_CONFLICT = "E_SYNC_STAGE_CONFLICT";
inline constexpr const char* E_SYNC_PEER_DEAD = "E_SYNC_PEER_DEAD";
inline constexpr const char* E_SYNC_SELECTION_EMPTY = "E_SYNC_SELECTION_EMPTY";
inline constexpr const char* E_SYNC_FK_CLOSURE_MISSING = "E_SYNC_FK_CLOSURE_MISSING";
inline constexpr const char* E_SYNC_FK_CYCLE_UNSUPPORTED = "E_SYNC_FK_CYCLE_UNSUPPORTED";
inline constexpr const char* E_SYNC_SELECTION_TOO_LARGE = "E_SYNC_SELECTION_TOO_LARGE";
inline constexpr const char* E_SYNC_PUSH_SCHEMA_MOVED = "E_SYNC_PUSH_SCHEMA_MOVED";
inline constexpr const char* E_BUSY = "E_BUSY";
inline constexpr const char* E_SYNC_WRITE_BLOCKED = "E_SYNC_WRITE_BLOCKED";
inline constexpr const char* E_SYNC_UNSUPPORTED_SCHEMA = "E_SYNC_UNSUPPORTED_SCHEMA";
inline constexpr const char* E_SYNC_ACK_TIMEOUT = "E_SYNC_ACK_TIMEOUT";
inline constexpr const char* E_SYNC_REBASE_FAILED = "E_SYNC_REBASE_FAILED";
inline constexpr const char* E_SYNC_BASELINE_FAILED = "E_SYNC_BASELINE_FAILED";

// ── Sync engine warning codes ────────────────────────────────────────────────
inline constexpr const char* W_SYNC_CONFLICT_REPLACED = "W_SYNC_CONFLICT_REPLACED";
inline constexpr const char* W_SYNC_BASELINE_LARGE = "W_SYNC_BASELINE_LARGE";
inline constexpr const char* W_SYNC_PAYLOAD_LARGE = "W_SYNC_PAYLOAD_LARGE";
inline constexpr const char* W_SYNC_UNTRACKED_CHANGE = "W_SYNC_UNTRACKED_CHANGE";
inline constexpr const char* W_SYNC_PEER_LAGGING = "W_SYNC_PEER_LAGGING";
inline constexpr const char* W_SYNC_PUSH_ROW_DRIFTED = "W_SYNC_PUSH_ROW_DRIFTED";
inline constexpr const char* W_SYNC_CONCURRENT_MANUAL_PUSH = "W_SYNC_CONCURRENT_MANUAL_PUSH";

}  // namespace dbridge::err
