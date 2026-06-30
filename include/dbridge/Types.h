#pragma once
#include "dbridge/RowPayload.h"

#include <QList>
#include <QString>
#include <QVector>

// ============================================================================
// Types.h — dbridge 对外公共数据类型（连接配置、导入/导出选项与结果、行级错误）
// ============================================================================
//
// 【这个文件是什么】
//   DataBridge 公共 API（见 DataBridge.h）在“调用入参”和“返回结果”上用到的
//   值类型集合。它们都是简单的 POD 风格 struct：只装数据、无行为，便于跨层传递。
//
// 【一次导入/导出的数据流（建立直觉）】
//   open(ConnectionSpec)              // 打开 SQLite 连接
//   loadProfile(...)                  // 载入 ETL 映射配置（Profile）
//   importExcel(xlsx, ImportOptions)  // 执行导入 → 返回 ImportResult
//                                     //   其中逐行/逐单元格错误装进 RowError 列表
//   exportExcel(xlsx, ExportOptions)  // 执行导出 → 返回 ExportResult
//
//   错误码字符串见 Errors.h；行/表级错误的“载体”就是这里的 RowError。
// ============================================================================

namespace dbridge {

// 数据库连接规格：打开 SQLite 时的参数。
struct ConnectionSpec {
    QString sqlitePath;  // SQLite 数据库文件路径
    int busyTimeoutMs = 5000;  // 数据库忙等待超时（毫秒）；并发占用时重试此时长再报错
    bool enableWal = true;  // 是否启用 WAL 日志模式（提升并发读写；同步子系统通常需要）
};

// 导入选项：控制 importExcel() 的行为。
struct ImportOptions {
    QString xlsxPath;  // 文件路径，供 IBatchTransfer 使用；DataBridge::importExcel 仍会显式再传一次
    QString profileName;  // 使用哪个已载入的 Profile（按名选择）
    QString sheetName;    // 指定工作表名；留空则用 Profile 中配置的 sheet
    bool abortOnError = true;  // 出错即中止；MVP 阶段必须为 true（保证“全有或全无”语义）
    bool dryRun =
        false;  // 试运行：跳过 UPSERT，只把构造好的载荷填入 ImportResult::dryRunPayloads 供检查
};

// 导出选项：控制 exportExcel() 的行为。
struct ExportOptions {
    QString xlsxPath;  // 文件路径，供 IBatchTransfer 使用；DataBridge::exportExcel 仍会显式再传一次
    QString profileName;  // 使用哪个 Profile
    QString sheetName;    // 指定工作表名；留空则用 Profile 中配置的 sheet
};

// RowError —— 行级/表级/单元格级 错误或警告的统一载体。
// 导入导出过程中每发现一处问题，就追加一条 RowError 到结果的 errors/warnings 列表。
struct RowError {
    QString sheet;  // 出错所在工作表名
    int row = 0;  // Excel 行号（1 基）；为 0 表示“表级错误”（与具体某一行无关）
    QString column;    // 出错的列（表头名）；为空表示“表级错误”
    QString rawValue;  // 触发错误的原始单元格文本（便于用户排查）
    QString code;      // 错误码字符串，取值见 Errors.h
    QString message;   // 人类可读的详细描述
};

// ImportResult —— importExcel() 的完整结果。
struct ImportResult {
    bool ok = false;         // 整体是否成功
    int readRows = 0;        // 从 Excel 实际读取的行数
    int writtenRows = 0;     // 实际写入数据库的行数
    QList<RowError> errors;  // 阻断性错误（导致行被跳过/操作中止）
    // 非阻断诊断信息（Profile 载入告警、orderBy 启发式提示等）。
    QList<RowError> warnings;
    // 仅当 ImportOptions::dryRun == true 时填充。
    // 内含完整构造好的 RowContext（经过 外键查找 + fkInject、但在 UPSERT 之前）。
    QVector<dbridge::detail::RowContext> dryRunPayloads;
};

// ExportResult —— exportExcel() 的完整结果。
struct ExportResult {
    bool ok = false;           // 整体是否成功
    int writtenRows = 0;       // 写入 Excel 的行数
    QList<RowError> errors;    // 阻断性错误
    QList<RowError> warnings;  // 非阻断诊断
};

}  // namespace dbridge
