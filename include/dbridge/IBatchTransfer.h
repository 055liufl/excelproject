#pragma once
#include "dbridge/Export.h"
#include "dbridge/Types.h"

#include <QList>

#include <memory>

// ============================================================================
// IBatchTransfer.h — Excel 批量导入/导出的“异步、可轮询、可停止”接口
// ============================================================================
//
// 【与 DataBridge::importExcel / exportExcel 的区别】
//   DataBridge 上的 importExcel()/exportExcel() 是“同步阻塞”版本：调用即执行、
//   返回即完成，适合 CLI/脚本。
//   IBatchTransfer 则是“非阻塞 + 轮询”版本：startImport() 立即返回，真正的搬运在
//   后台进行；调用方周期性地用 progress()/errors()/state() 拉取快照（典型用于 GUI，
//   既不卡界面又能显示进度条）。
//
// 【典型用法】
//   auto t = createBatchTransfer(bridge);
//   t->startImport(opts, &err);                 // 立即返回
//   while (t->importState() == TransferState::Running) {
//       auto p = t->importProgress();           // 线程安全快照，刷新进度条
//       ... t->stop() ...                       // 可随时请求停止
//   }
//   auto result = t->importResult();            // 完成后取最终结果
//
// 【线程模型】
//   各 getter 返回的是“加锁拷贝的快照”，可在任意线程安全调用（见接口注释 ③–⑧）。
// 【DBRIDGE_EXPORT】
//   导出宏（来自 Export.h，由构建系统生成）：控制符号在动态库下的可见性；
//   本项目以静态库链接（定义了 DBRIDGE_STATIC_DEFINE）时它展开为空。
// ============================================================================

namespace dbridge {

class DataBridge;

// 传输进度快照。
struct TransferProgress {
    int percent = 0;        // 进度百分比 [0,100]
    qint64 rowsDone = 0;    // 已处理行数
    qint64 rowsTotal = -1;  // 总行数；-1 表示未知（尚未统计出总量）
};

// 传输状态机：Idle（空闲）→ Running（进行中）→ Stopping（收到停止请求、收尾中）
//            → 终态之一：Completed（正常完成）/ Stopped（被停止）/ Failed（失败）。
enum class TransferState { Idle, Running, Stopping, Completed, Stopped, Failed };

class DBRIDGE_EXPORT IBatchTransfer {
   public:
    virtual ~IBatchTransfer() = default;

    // ① 非阻塞地启动导入；成功后会重置导入侧的轮询状态（进度/错误/结果清零重来）。
    virtual bool startImport(const ImportOptions& options, QString* err = nullptr) = 0;

    // ② 非阻塞地启动导出。
    virtual bool startExport(const ExportOptions& options, QString* err = nullptr) = 0;

    // ③–⑧ 轮询用 getter（返回加锁拷贝的快照，线程安全，可在 GUI 线程随意调用）
    virtual TransferProgress importProgress() const = 0;  // ③ 导入进度
    virtual QList<RowError> importErrors() const = 0;     // ④ 导入累计错误
    virtual ImportResult importResult() const = 0;  // ⑤ 导入最终结果（完成后有效）
    virtual TransferProgress exportProgress() const = 0;  // ⑥ 导出进度
    virtual QList<RowError> exportErrors() const = 0;     // ⑦ 导出累计错误
    virtual ExportResult exportResult() const = 0;  // ⑧ 导出最终结果（完成后有效）

    // C9 对称补充：停止 + 分别查询导入/导出状态。
    virtual bool stop(QString* err = nullptr) = 0;  // 请求停止当前传输（协作式，进入 Stopping）
    virtual TransferState importState() const = 0;  // 导入状态机当前状态
    virtual TransferState exportState() const = 0;  // 导出状态机当前状态
};

// 工厂函数：基于已打开的 DataBridge 创建一个批量传输器。
// 用 unique_ptr 返回 → 调用方独占所有权，析构即自动释放。
DBRIDGE_EXPORT std::unique_ptr<IBatchTransfer> createBatchTransfer(DataBridge& bridge);

}  // namespace dbridge
