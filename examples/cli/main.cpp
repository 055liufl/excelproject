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

// 头文件依赖：DataBridge 是库的对外门面（open/loadProfile/importExcel/exportExcel），
// Types.h 提供 ConnectionSpec / ImportOptions / ExportOptions / ImportResult 等值类型。
#include "dbridge/DataBridge.h"
#include "dbridge/Types.h"

#include <QCoreApplication>  // 控制台程序的事件循环宿主（本例不进 exec()，仅为初始化 Qt 环境）
#include <QFile>          // 读取 profile JSON 文件
#include <QJsonDocument>  // 解析 profile JSON
#include <QJsonObject>    // 取 JSON 对象里的 profileName 字段
#include <QString>

#include <iostream>  // std::cout / std::cerr 打印结果与错误（控制台程序用标准流而非 Qt 日志）

// ── readProfileName —— 从 profile JSON 文件中读出 "profileName" 字段 ─────────────
// 【做什么】打开磁盘上的 profile JSON → 解析为 JSON 对象 → 取顶层 "profileName" 字符串。
// 【为什么单独抽出】DataBridge::loadProfile 按「Profile 内部的 name」索引存放各 Profile；
//   后续 importExcel/exportExcel 通过 ImportOptions/ExportOptions::profileName 指定用哪一份。
//   故 main 需要先知道这份 JSON 里声明的 profileName 是什么，才能在导入/导出时引用它。
//   这里只「窥探」名字一次，真正的载入仍由下面的 bridge.loadProfile 完成（重复读文件，
//   但 CLI 示例追求简单清晰，不在意这点开销）。
// 【参数】path 文件路径；err 失败原因出参（可空）。
// 【返回】成功返回 profileName 字符串；任一步失败返回空 QString 并写 *err。
// 【错误模式】文件打不开 / JSON 非法 / 不是对象 → 返回空串。
//   注意：JSON 合法但缺 "profileName" 字段时返回空串但「不」写 err——调用方据「空串」判失败。
static QString readProfileName(const QString& path, QString* err) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {  // 只读打开；失败多因路径不存在/无权限
        if (err)
            *err = QStringLiteral("Cannot open profile file: ") + path;
        return {};
    }
    QJsonParseError pe;
    // 一次性读入全文并解析；pe 在语法错误时携带出错位置/原因。
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (doc.isNull() || !doc.isObject()) {  // 解析失败或根不是对象（Profile 必须是 JSON 对象）
        if (err)
            *err = QStringLiteral("Profile JSON invalid: ") + pe.errorString();
        return {};
    }
    // 取顶层 "profileName"；字段缺失时 value() 返回 Undefined，toString() 得到空串。
    return doc.object().value(QStringLiteral("profileName")).toString();
}

// ── main —— CLI 主流程：解析参数 → 打开库 → 载入 Profile → 按 mode 导入或导出 ─────
// 【整体流程（与文件头「运行方式」对应）】
//   ① 校验/解析命令行参数（db 路径、profile 路径、xlsx 路径、可选 mode）；
//   ② 从 profile JSON 里读出 profileName；
//   ③ 打开 SQLite 库（DataBridge::open，内部还会做一次 schema 自省）；
//   ④ 载入 Profile（DataBridge::loadProfile，若库已开还会立刻按当前 schema 校验）；
//   ⑤ 据 mode 调 importExcel 或 exportExcel，打印写入/导出行数与逐行错误；
//   ⑥ 关闭连接并返回退出码（0 成功，非 0 失败）。
// 【退出码约定】任一致命步骤失败立即 return 1；导入/导出「整体失败」也 return 1；
//   「整体成功但含行级错误」仍视为成功（return 0），只把行级错误打到 stderr 供排查
//   ——这与库的语义一致：行级错误是非阻断的部分失败，整体结果 ok 仍为 true。
int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);  // 初始化 Qt 运行环境；本例为一次性命令，不进入事件循环

    // ① 参数个数校验：至少需要 db / profile / xlsx 三个必填参数（mode 可选，默认 import）。
    if (argc < 4) {
        std::cerr << "Usage: dbridge-cli <db_path> <profile_json> <xlsx_path> [import|export]\n";
        return 1;
    }

    // 用 fromLocal8Bit 按当前系统区域编码解释命令行实参（兼顾非 ASCII 路径）。
    QString dbPath = QString::fromLocal8Bit(argv[1]);
    QString profilePath = QString::fromLocal8Bit(argv[2]);
    QString xlsxPath = QString::fromLocal8Bit(argv[3]);
    // 第 4 个参数（mode）可选：未给则默认 "import"。
    QString mode = argc > 4 ? QString::fromLocal8Bit(argv[4]) : QStringLiteral("import");

    QString err;  // 贯穿全流程复用的错误文本出参
    // ② 先从 JSON 里取 profileName（导入/导出时要靠它指定用哪份 Profile）。
    QString profileName = readProfileName(profilePath, &err);
    if (profileName.isEmpty()) {  // 空串=读取/解析失败或字段缺失，无法继续
        std::cerr << "Failed to read profileName: " << err.toStdString() << '\n';
        return 1;
    }

    dbridge::DataBridge bridge;  // 库门面：本程序与整个 dbridge 库交互的唯一入口
    dbridge::ConnectionSpec cs;  // 连接规格（此处仅设路径，其余用默认 PRAGMA）
    cs.sqlitePath = dbPath;

    // ③ 打开数据库连接（内部会校验 SQLite 版本、设 PRAGMA、做 schema 自省）。
    if (!bridge.open(cs, &err)) {
        std::cerr << "Failed to open DB: " << err.toStdString() << '\n';
        return 1;
    }

    // ④ 载入 Profile（库已打开，故此处还会用当前 schema 立即做一次结构性校验）。
    if (!bridge.loadProfile(profilePath, &err)) {
        std::cerr << "Failed to load profile: " << err.toStdString() << '\n';
        return 1;
    }

    // ⑤ 据 mode 分流：导入 / 导出 / 未知。
    if (mode == QStringLiteral("import")) {
        // —— 导入分支：把 .xlsx 灌进数据库 ——
        dbridge::ImportOptions opts;
        opts.profileName = profileName;  // 指定使用上面载入的那份 Profile
        auto result = bridge.importExcel(xlsxPath, opts);  // 同步阻塞执行整条导入管线
        if (result.ok) {
            // 整体成功：打印写入行数。即便成功，仍可能存在「行级、非阻断」错误（脏行被跳过等），
            // 这里把它们补打到 stderr，但不改变退出码（仍按成功 return 0）。
            std::cout << "Imported " << result.writtenRows << " rows\n";
            if (!result.errors.empty()) {
                std::cerr << "  (with " << result.errors.size() << " row-level errors)\n";
                // 逐条打印：错误码、行号、列名、说明——便于定位是哪一行哪一列出问题。
                for (const auto& e : result.errors)
                    std::cerr << "  [" << e.code.toStdString() << "] row " << e.row << " "
                              << e.column.toStdString() << ": " << e.message.toStdString() << '\n';
            }
        } else {
            // 整体失败（表级错误，如表不存在/Profile 不匹配）：打印全部错误并以失败码退出。
            std::cerr << "Import failed with " << result.errors.size() << " errors:\n";
            for (const auto& e : result.errors) {
                std::cerr << "  [" << e.code.toStdString() << "] row " << e.row << " "
                          << e.column.toStdString() << ": " << e.message.toStdString() << '\n';
            }
            return 1;
        }
    } else if (mode == QStringLiteral("export")) {
        // —— 导出分支：从数据库查询并写出 .xlsx ——
        dbridge::ExportOptions opts;
        opts.profileName = profileName;
        auto result = bridge.exportExcel(xlsxPath, opts);  // 同步阻塞执行整条导出管线
        if (result.ok) {
            std::cout << "Exported " << result.writtenRows << " rows\n";
            if (!result.errors.empty()) {
                std::cerr << "  (with " << result.errors.size() << " row-level errors)\n";
                for (const auto& e : result.errors)
                    std::cerr << "  [" << e.code.toStdString() << "] row " << e.row << " "
                              << e.column.toStdString() << ": " << e.message.toStdString() << '\n';
            }
        } else {
            // 导出整体失败：表级错误（如 SELECT 失败、写文件失败）通常无行/列定位，
            // 故只打印错误码与消息。
            std::cerr << "Export failed with " << result.errors.size() << " errors:\n";
            for (const auto& e : result.errors) {
                std::cerr << "  [" << e.code.toStdString() << "] " << e.message.toStdString()
                          << '\n';
            }
            return 1;
        }
    } else {
        // mode 既不是 import 也不是 export：参数非法，提示并失败退出。
        std::cerr << "Unknown mode: " << mode.toStdString() << " (expected import|export)\n";
        return 1;
    }

    bridge.close();  // ⑥ 释放连接（幂等）。进程随即结束，析构也会兜底，但显式关闭更清晰。
    return 0;
}
