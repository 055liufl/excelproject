#pragma once
#include "dbridge/RowPayload.h"

#include <QList>
#include <QString>
#include <QVector>

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
    bool dryRun = false;       // skip UPSERT; populate ImportResult::dryRunPayloads
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
    // Non-blocking diagnostics (profile load warnings, orderBy heuristic, etc.).
    QList<RowError> warnings;
    // Populated only when ImportOptions::dryRun == true.
    // Contains the fully-constructed RowContexts (after lookup + fkInject, before UPSERT).
    QVector<dbridge::detail::RowContext> dryRunPayloads;
};

struct ExportResult {
    bool ok = false;
    int writtenRows = 0;
    QList<RowError> errors;
    QList<RowError> warnings;
};

}  // namespace dbridge
