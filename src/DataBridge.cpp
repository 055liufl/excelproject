#include "dbridge/DataBridge.h"

#include "dbridge/Errors.h"

#include <QFile>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "DataBridgePrivate.h"
#include "profile/ProfileLoader.h"
#include "profile/ProfileValidator.h"
#include "schema/SchemaCatalog.h"
#include "service/ErrorCollector.h"

namespace dbridge {

// ---------- DataBridgePrivate implementation ----------

namespace detail {

bool DataBridgePrivate::openDb(const ConnectionSpec& spec, QString* err) {
    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connName_);
    db_.setDatabaseName(spec.sqlitePath);
    if (!db_.open()) {
        if (err)
            *err =
                QString::fromLatin1(err::E_OPEN_DB) + QStringLiteral(": ") + db_.lastError().text();
        return false;
    }

    // Check SQLite version >= 3.24.0
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("SELECT sqlite_version()")) || !q.next()) {
        if (err)
            *err = QStringLiteral("Failed to query SQLite version");
        db_.close();
        return false;
    }
    QString ver = q.value(0).toString();
    QStringList parts = ver.split('.');
    int major = parts.value(0).toInt();
    int minor = parts.value(1).toInt();
    int patch = parts.value(2).toInt();
    if (major < 3 || (major == 3 && minor < 24) || (major == 3 && minor == 24 && patch < 0)) {
        if (err)
            *err = QString::fromLatin1(err::E_OPEN_DB) + QStringLiteral(": SQLite version ") + ver +
                   QStringLiteral(" < 3.24.0, ON CONFLICT DO UPDATE not supported");
        db_.close();
        return false;
    }

    // Configure pragmas
    q.exec(QStringLiteral("PRAGMA busy_timeout = ") + QString::number(spec.busyTimeoutMs));
    if (spec.enableWal) {
        q.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    }
    q.exec(QStringLiteral("PRAGMA foreign_keys = ON"));

    dbOpen_ = true;
    return true;
}

void DataBridgePrivate::closeDb() {
    if (dbOpen_) {
        db_.close();
        dbOpen_ = false;
    }
    // Drop our QSqlDatabase reference BEFORE removeDatabase, otherwise Qt
    // logs "connection is still in use" because db_ itself still holds a
    // QExplicitlySharedDataPointer to the connection.
    db_ = QSqlDatabase();
    if (QSqlDatabase::contains(connName_)) {
        QSqlDatabase::removeDatabase(connName_);
    }
}

bool DataBridgePrivate::loadProfileDoc(const QJsonDocument& doc, QString* err) {
    ProfileLoader loader;
    ProfileSpec spec;
    if (!loader.load(doc, &spec, err))
        return false;
    profiles_[spec.name] = spec;
    return true;
}

bool DataBridgePrivate::refreshCatalog(QString* err) {
    SchemaIntrospector introspector;
    return introspector.load(db_, &catalog_, err);
}

}  // namespace detail

// ---------- DataBridge public API ----------

DataBridge::DataBridge() : d_(std::make_unique<detail::DataBridgePrivate>()) {
}
DataBridge::~DataBridge() = default;

bool DataBridge::open(const ConnectionSpec& spec, QString* err) {
    if (d_->dbOpen_)
        close();
    if (!d_->openDb(spec, err))
        return false;

    // Initial schema load
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {
        if (err)
            *err = QStringLiteral("Schema introspection failed: ") + schErr;
        d_->closeDb();
        return false;
    }
    return true;
}

void DataBridge::close() {
    d_->closeDb();
}

bool DataBridge::loadProfile(const QString& jsonPath, QString* err) {
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("Cannot open profile file: ") + jsonPath;
        return false;
    }
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
    if (doc.isNull()) {
        if (err)
            *err = QString::fromLatin1(err::E_PROFILE_PARSE) + QStringLiteral(": ") +
                   parseErr.errorString();
        return false;
    }
    return d_->loadProfileDoc(doc, err);
}

bool DataBridge::loadProfileFromString(const QString& json, QString* err) {
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &parseErr);
    if (doc.isNull()) {
        if (err)
            *err = QString::fromLatin1(err::E_PROFILE_PARSE) + QStringLiteral(": ") +
                   parseErr.errorString();
        return false;
    }
    return d_->loadProfileDoc(doc, err);
}

QString DataBridge::generateAutoProfileJson(const QString& table, QString* err) {
    if (!d_->dbOpen_) {
        if (err)
            *err = QStringLiteral("Database not open");
        return {};
    }
    // Refresh schema
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {
        if (err)
            *err = schErr;
        return {};
    }

    if (!d_->catalog_.hasTable(table)) {
        if (err)
            *err =
                QString::fromLatin1(err::E_PROFILE_TABLE_NOT_FOUND) + QStringLiteral(": ") + table;
        return {};
    }

    detail::ProfileSpec spec;
    if (!d_->autoBuilder_.build(*d_->catalog_.table(table), &spec, err))
        return {};
    return d_->autoBuilder_.toJson(spec);
}

void DataBridge::setSyncActive(bool active, const QStringList& syncTables) {
    d_->syncTables_ = syncTables;
    d_->syncActive_ = active;  // written last so reader sees syncTables_ first
}

// M-02 helper: collect all route-level table names from a profile (all modes).
static QStringList profileRouteTables(const detail::ProfileSpec& spec) {
    QStringList tables;
    for (const auto& r : spec.routes)
        tables.append(r.table);
    for (const auto& cls : spec.classes)
        for (const auto& r : cls.routes)
            tables.append(r.table);
    return tables;
}

ImportResult DataBridge::importExcel(const QString& xlsxPath, const ImportOptions& options) {
    ImportResult result;
    // J-09: Block direct imports while sync is active; callers must use IBatchTransfer instead.
    // M-02 fix: if the profile's routes do not touch any sync-monitored table the import is safe
    // to proceed (e.g. auxiliary/reference tables not included in the sync table list).
    if (d_->syncActive_) {
        // Look up profile first so we can inspect its routes.
        auto profIt = d_->profiles_.find(options.profileName);
        bool blocked = true;  // conservative default: block if we can't determine safety
        if (profIt != d_->profiles_.end() && !d_->syncTables_.isEmpty()) {
            const QStringList routeTables = profileRouteTables(profIt.value());
            blocked = false;
            for (const QString& t : routeTables) {
                if (d_->syncTables_.contains(t)) {
                    blocked = true;
                    break;
                }
            }
        }
        if (blocked) {
            RowError e;
            e.code = QString::fromLatin1(err::E_SYNC_WRITE_BLOCKED);
            e.message = QStringLiteral("Sync is active; use IBatchTransfer for imports");
            result.errors.append(e);
            return result;
        }
    }
    if (!d_->dbOpen_) {
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Database not open");
        result.errors.append(e);
        return result;
    }

    auto it = d_->profiles_.find(options.profileName);
    if (it == d_->profiles_.end()) {
        RowError e;
        e.code = QString::fromLatin1(err::E_PROFILE_PARSE);
        e.message = QStringLiteral("Profile not loaded: ") + options.profileName;
        result.errors.append(e);
        return result;
    }

    // Refresh catalog before import
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Schema refresh failed: ") + schErr;
        result.errors.append(e);
        return result;
    }

    return d_->importSvc_.run(it.value(), d_->catalog_, xlsxPath, options, d_->db_);
}

QString DataBridge::dbPath() const {
    return d_->dbOpen_ ? d_->db_.databaseName() : QString();
}

ImportResult DataBridge::runImportOnDb(const QString& xlsxPath, const ImportOptions& options,
                                       QSqlDatabase& db) {
    ImportResult result;
    if (!d_->dbOpen_) {
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Database not open");
        result.errors.append(e);
        return result;
    }
    auto it = d_->profiles_.find(options.profileName);
    if (it == d_->profiles_.end()) {
        RowError e;
        e.code = QString::fromLatin1(err::E_PROFILE_PARSE);
        e.message = QStringLiteral("Profile not loaded: ") + options.profileName;
        result.errors.append(e);
        return result;
    }
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Schema refresh failed: ") + schErr;
        result.errors.append(e);
        return result;
    }
    return d_->importSvc_.run(it.value(), d_->catalog_, xlsxPath, options, db);
}

ExportResult DataBridge::runExportOnDb(const QString& xlsxPath, const ExportOptions& options,
                                       QSqlDatabase& db) {
    ExportResult result;
    if (!d_->dbOpen_) {
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Database not open");
        result.errors.append(e);
        return result;
    }
    auto it = d_->profiles_.find(options.profileName);
    if (it == d_->profiles_.end()) {
        RowError e;
        e.code = QString::fromLatin1(err::E_PROFILE_PARSE);
        e.message = QStringLiteral("Profile not loaded: ") + options.profileName;
        result.errors.append(e);
        return result;
    }
    return d_->exportSvc_.run(it.value(), d_->catalog_, xlsxPath, options, db);
}

bool DataBridge::snapshotProfileCatalog(const QString& profileName, detail::ProfileSpec* profile,
                                        detail::SchemaCatalog* catalog, QString* err) {
    if (!d_->dbOpen_) {
        if (err)
            *err = QStringLiteral("Database not open");
        return false;
    }
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {
        if (err)
            *err = QStringLiteral("Schema refresh failed: ") + schErr;
        return false;
    }
    auto it = d_->profiles_.find(profileName);
    if (it == d_->profiles_.end()) {
        if (err)
            *err = QStringLiteral("Profile not loaded: ") + profileName;
        return false;
    }
    if (profile)
        *profile = it.value();
    if (catalog)
        *catalog = d_->catalog_;
    return true;
}

bool DataBridge::snapshotCatalog(detail::SchemaCatalog* catalog, QString* err) {
    if (!d_->dbOpen_) {
        if (err)
            *err = QStringLiteral("Database not open");
        return false;
    }
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {
        if (err)
            *err = QStringLiteral("Schema refresh failed: ") + schErr;
        return false;
    }
    if (catalog)
        *catalog = d_->catalog_;
    return true;
}

ExportResult DataBridge::exportExcel(const QString& xlsxPath, const ExportOptions& options) {
    ExportResult result;
    if (!d_->dbOpen_) {
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Database not open");
        result.errors.append(e);
        return result;
    }

    auto it = d_->profiles_.find(options.profileName);
    if (it == d_->profiles_.end()) {
        RowError e;
        e.code = QString::fromLatin1(err::E_PROFILE_PARSE);
        e.message = QStringLiteral("Profile not loaded: ") + options.profileName;
        result.errors.append(e);
        return result;
    }

    // Refresh catalog before export
    QString schErr;
    if (!d_->refreshCatalog(&schErr)) {
        RowError e;
        e.code = QString::fromLatin1(err::E_OPEN_DB);
        e.message = QStringLiteral("Schema refresh failed: ") + schErr;
        result.errors.append(e);
        return result;
    }

    // H-03 fix: run export-mode profile validation before export so columnOrder, rawSql,
    // and table/column existence errors surface early.
    {
        detail::ErrorCollector valErrors;
        detail::ProfileValidator validator;
        if (!validator.validateForExport(it.value(), d_->catalog_, &valErrors)) {
            ExportResult valResult;
            valResult.errors = valErrors.list();
            return valResult;
        }
    }

    return d_->exportSvc_.run(it.value(), d_->catalog_, xlsxPath, options, d_->db_);
}

}  // namespace dbridge
