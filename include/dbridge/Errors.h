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

}  // namespace dbridge::err
