#pragma once
#include "dbridge/Export.h"
#include "dbridge/Types.h"

#include <QSqlDatabase>

#include <memory>

namespace dbridge {

namespace detail {
class DataBridgePrivate;
struct ProfileSpec;
class SchemaCatalog;
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
    ExportResult runExportOnDb(const QString& xlsxPath, const ExportOptions& options,
                               QSqlDatabase& db);

    // Copy profile/catalog state on the owner thread before dispatching work to another thread.
    bool snapshotProfileCatalog(const QString& profileName, detail::ProfileSpec* profile,
                                detail::SchemaCatalog* catalog, QString* err = nullptr);

    // Copy only the current schema catalog. Used by sync paths that do not depend on an ETL
    // profile.
    bool snapshotCatalog(detail::SchemaCatalog* catalog, QString* err = nullptr);

    // J-09: Called by SyncEngine::initialize() / ~SyncEngine() to guard importExcel()
    // against direct writes while sync is active.
    // syncTables: the canonical sync-monitored table list; importExcel() allows profiles
    // that target only non-sync tables to proceed even while sync is active.
    void setSyncActive(bool active, const QStringList& syncTables = {});

   private:
    std::unique_ptr<detail::DataBridgePrivate> d_;
};

}  // namespace dbridge
