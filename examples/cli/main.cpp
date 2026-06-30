/**
 * dbridge-cli — DataBridge 命令行示例：按 Profile 对 SQLite 表做 Excel 批量导入/导出。
 *
 * ──────────────────────────────────────────────────────────────────────────────
 * 运行方式
 *   构建产物：build_qmake_demos/examples/cli/dbridge-cli
 *
 *   # 环境变量：本机 shell 的 LD_LIBRARY_PATH 指向 QtCreator 自带的 Qt 5.15.2，
 *   # 与项目使用的 Qt 5.12.12 冲突，运行前需把 5.12.12 的库路径前置（否则 abort：
 *   # "Cannot mix incompatible Qt library"）。本 demo 为控制台程序（QCoreApplication），
 *   # 不加载平台插件，故无需设置 QT_QPA_PLATFORM。
 *   export LD_LIBRARY_PATH=/opt/Qt5.12.12/5.12.12/gcc_64/lib:$LD_LIBRARY_PATH
 *
 *   cd build_qmake_demos/examples/cli
 *   ./dbridge-cli <db_path> <profile_json> <xlsx_path> [import|export]
 *   # 例（导入）：./dbridge-cli ./demo.db ./profile.json ./data.xlsx import
 *   # 例（导出）：./dbridge-cli ./demo.db ./profile.json ./out.xlsx  export
 * ──────────────────────────────────────────────────────────────────────────────
 */

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
            if (!result.errors.empty()) {
                std::cerr << "  (with " << result.errors.size() << " row-level errors)\n";
                for (const auto& e : result.errors)
                    std::cerr << "  [" << e.code.toStdString() << "] row " << e.row << " "
                              << e.column.toStdString() << ": " << e.message.toStdString() << '\n';
            }
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
            if (!result.errors.empty()) {
                std::cerr << "  (with " << result.errors.size() << " row-level errors)\n";
                for (const auto& e : result.errors)
                    std::cerr << "  [" << e.code.toStdString() << "] row " << e.row << " "
                              << e.column.toStdString() << ": " << e.message.toStdString() << '\n';
            }
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
