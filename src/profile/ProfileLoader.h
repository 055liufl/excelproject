#pragma once
#include <QJsonDocument>
#include <QJsonObject>

#include "ProfileSpec.h"

namespace dbridge::detail {

class ProfileLoader {
   public:
    bool load(const QJsonDocument& doc, ProfileSpec* out, QString* err);

   private:
    bool readSingleTable(const QJsonObject& o, ProfileSpec* out, QString* err);
    bool readMultiTable(const QJsonObject& o, ProfileSpec* out, QString* err);
    bool readMixed(const QJsonObject& o, ProfileSpec* out, QString* err);
    bool readRoute(const QJsonObject& o, RouteSpec* out, QString* err);
    bool readColumn(const QString& dbCol, const QJsonObject& o, ColumnSpec* out, QString* err);
    bool validateToken(const QString& token, QString* err);
};

}  // namespace dbridge::detail
