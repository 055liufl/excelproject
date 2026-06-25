#pragma once
#include "dbridge/Export.h"
#include "dbridge/Types.h"

#include <QSqlDatabase>

#include <memory>

namespace dbridge {

namespace detail {
class DataBridgePrivate;
}  // namespace detail

class DBRIDGE_EXPORT DataBridge {
   public:
    DataBridge();
    ~DataBridge();
    DataBridge(const DataBridge&) = delete;
    DataBridge& operator=(const DataBridge&) = delete;

    bool open(const ConnectionSpec& spec, QString* err = nullptr);
    void close();

    bool loadProfile(const QString& jsonPath, QString* err = nullptr);
    bool loadProfileFromString(const QString& json, QString* err = nullptr);

    // Auto-generate a single-table Profile based on SQLite introspection.
    // Returns JSON string; caller may save/edit then call loadProfileFromString.
    QString generateAutoProfileJson(const QString& table, QString* err = nullptr);

    ImportResult importExcel(const QString& xlsxPath, const ImportOptions& options);
    ExportResult exportExcel(const QString& xlsxPath, const ExportOptions& options);

    // Returns the SQLite file path (empty if not open). Used by sync layer to route imports.
    QString dbPath() const;

    // Run import using stored profiles/catalog but writing to the provided db connection.
    // Called by SyncWorker to run ImportService on wconn with session capture (I-04).
    ImportResult runImportOnDb(const QString& xlsxPath, const ImportOptions& options,
                               QSqlDatabase& db);

   private:
    std::unique_ptr<detail::DataBridgePrivate> d_;
};

}  // namespace dbridge
