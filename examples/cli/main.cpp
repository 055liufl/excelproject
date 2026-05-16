#include "dbridge/DataBridge.h"
#include "dbridge/Types.h"

#include <QCoreApplication>
#include <QDebug>
#include <QString>

#include <iostream>

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

    dbridge::DataBridge bridge;
    dbridge::ConnectionSpec cs;
    cs.sqlitePath = dbPath;

    QString err;
    if (!bridge.open(cs, &err)) {
        std::cerr << "Failed to open DB: " << err.toStdString() << '\n';
        return 1;
    }

    if (!bridge.loadProfile(profilePath, &err)) {
        std::cerr << "Failed to load profile: " << err.toStdString() << '\n';
        return 1;
    }

    // For simplicity, use the profile name from the file (assume it was loaded)
    // In real usage, you'd parse the JSON to get the profileName
    if (mode == QStringLiteral("import")) {
        dbridge::ImportOptions opts;
        opts.profileName = QStringLiteral("profile");  // placeholder
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
        opts.profileName = QStringLiteral("profile");  // placeholder
        auto result = bridge.exportExcel(xlsxPath, opts);
        if (result.ok) {
            std::cout << "Exported " << result.writtenRows << " rows\n";
        } else {
            std::cerr << "Export failed\n";
            return 1;
        }
    }

    bridge.close();
    return 0;
}
