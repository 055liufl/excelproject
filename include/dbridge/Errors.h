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
inline constexpr const char* E_DB_UPSERT = "E_DB_UPSERT";
inline constexpr const char* E_EXPORT_QUERY = "E_EXPORT_QUERY";
inline constexpr const char* E_WRITE_XLSX = "E_WRITE_XLSX";

}  // namespace dbridge::err
