#pragma once
#include <QList>
#include <QString>

namespace dbridge {

struct ConnectionSpec {
    QString sqlitePath;
    int busyTimeoutMs = 5000;
    bool enableWal = true;
};

struct ImportOptions {
    QString profileName;
    QString sheetName;         // empty = use Profile's sheet
    bool abortOnError = true;  // MVP must be true
};

struct ExportOptions {
    QString profileName;
    QString sheetName;  // empty = use Profile's sheet
};

struct RowError {
    QString sheet;
    int row = 0;     // Excel 1-based row; 0 for table-level errors
    QString column;  // header name; empty for table-level errors
    QString rawValue;
    QString code;  // see Errors.h
    QString message;
};

struct ImportResult {
    bool ok = false;
    int readRows = 0;
    int writtenRows = 0;
    QList<RowError> errors;
};

struct ExportResult {
    bool ok = false;
    int writtenRows = 0;
    QList<RowError> errors;
};

}  // namespace dbridge
