#include "dbridge/DataBridge.h"
#include "dbridge/Types.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <iostream>

static QString readProfileName(const QString& path, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("Cannot open profile file: ") + path;
        return {};
    }
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (doc.isNull() || !doc.isObject()) {
        if (err)
            *err = QStringLiteral("Profile JSON invalid: ") + pe.errorString();
        return {};
    }
    return doc.object().value(QStringLiteral("profileName")).toString();
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    if (argc < 4) {
        std::cerr << "Usage: dbridge-cli <db_path> <profile_json> <xlsx_path> [import|export]\n";
        return 1;
    }

    QString dbPath = QString::fromLocal8Bit(argv[1]);
    QString profilePath = QString::fromLocal8Bit(argv[2]);
    QString xlsxPath = QString::fromLocal8Bit(argv[3]);
    QString mode = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("import");

    QString err;
    QString profileName = readProfileName(profilePath, &err);
    if (profileName.isEmpty()) {
        std::cerr << "Failed to read profileName: " << err.toStdString() << '\n';
        return 1;
    }

    dbridge::DataBridge bridge;
    dbridge::ConnectionSpec cs;
    cs.sqlitePath = dbPath;

    if (!bridge.open(cs, &err)) {
        std::cerr << "Failed to open DB: " << err.toStdString() << '\n';
        return 1;
    }

    if (!bridge.loadProfile(profilePath, &err)) {
        std::cerr << "Failed to load profile: " << err.toStdString() << '\n';
        return 1;
    }

    if (mode == QStringLiteral("import")) {
        dbridge::ImportOptions opts;
        opts.profileName = profileName;
        auto result = bridge.importExcel(xlsxPath, opts);
        if (result.ok) {
            std::cout << "Imported " << result.writtenRows << " rows\n";
        } else {
            std::cerr << "Import failed with " << result.errors.size() << " errors:\n";
            for (const auto& e : result.errors) {
                std::cerr << "  [" << e.code.toStdString() << "] row " << e.row << " "
                          << e.column.toStdString() << ": " << e.message.toStdString() << '\n';
            }
            return 1;
        }
    } else if (mode == QStringLiteral("export")) {
        dbridge::ExportOptions opts;
        opts.profileName = profileName;
        auto result = bridge.exportExcel(xlsxPath, opts);
        if (result.ok) {
            std::cout << "Exported " << result.writtenRows << " rows\n";
        } else {
            std::cerr << "Export failed with " << result.errors.size() << " errors:\n";
            for (const auto& e : result.errors) {
                std::cerr << "  [" << e.code.toStdString() << "] " << e.message.toStdString()
                          << '\n';
            }
            return 1;
        }
    } else {
        std::cerr << "Unknown mode: " << mode.toStdString() << " (expected import|export)\n";
        return 1;
    }

    bridge.close();
    return 0;
}
