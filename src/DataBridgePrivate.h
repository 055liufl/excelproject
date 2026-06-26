#pragma once
#include <QHash>
#include <QSqlDatabase>
#include <QString>
#include <QUuid>

#include "profile/AutoProfileBuilder.h"
#include "profile/ProfileLoader.h"
#include "profile/ProfileSpec.h"
#include "schema/SchemaCatalog.h"
#include "schema/SchemaIntrospector.h"
#include "service/ExportService.h"
#include "service/ImportService.h"
#include <atomic>

namespace dbridge::detail {

class DataBridgePrivate {
   public:
    DataBridgePrivate() : connName_(QStringLiteral("dbridge_") + QUuid::createUuid().toString()) {
    }

    ~DataBridgePrivate() {
        closeDb();
    }

    bool openDb(const ConnectionSpec& spec, QString* err);
    void closeDb();

    bool loadProfileDoc(const QJsonDocument& doc, QString* err);
    bool refreshCatalog(QString* err);

    QSqlDatabase db_;
    QString connName_;
    bool dbOpen_ = false;
    std::atomic<bool> syncActive_{
        false};  // L-01 fix: atomic to avoid data race between SyncEngine init and importExcel
    SchemaCatalog catalog_;
    QHash<QString, ProfileSpec> profiles_;

    // Services (stateless, can be reused)
    ImportService importSvc_;
    ExportService exportSvc_;
    AutoProfileBuilder autoBuilder_;
};

}  // namespace dbridge::detail
