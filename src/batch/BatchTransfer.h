#pragma once
#include "dbridge/DataBridge.h"
#include "dbridge/IBatchTransfer.h"

#include <QFuture>
#include <QMutex>
#include <QSqlDatabase>
#include <QtConcurrent/QtConcurrent>

#include "profile/ProfileSpec.h"
#include "schema/SchemaCatalog.h"
#include <atomic>
#include <memory>

// ============================================================================
// BatchTransfer.h —— IBatchTransfer 的具体实现（异步批量传输引擎，声明部分）
// ============================================================================
//
// 【这个类是什么 / 与 IBatchTransfer 的关系】
//   IBatchTransfer（见 include/dbridge/IBatchTransfer.h）只规定了「契约」：
//   非阻塞 startImport/startExport、可轮询 progress/errors/result/state、可
//   协作式停止 stop。BatchTransfer 就是这份契约的唯一落地实现——它把真正的
//   ETL 搬运工作（ImportService / ExportService）丢到后台线程执行，主调用线程
//   （通常是 GUI 线程）立即拿回控制权，之后周期性轮询快照刷新进度条。
//   工厂函数 createBatchTransfer()（见 .cpp 末尾）负责 new 出本类并以
//   unique_ptr<IBatchTransfer> 形式交给调用方（调用方只见接口、不见实现）。
//
// 【线程模型（本类的核心，务必读懂）】
//   存在两类线程，它们通过下面的成员交汇：
//     · 「控制线程」：调用 startImport/startExport/stop 以及所有 getter 的线程
//       （一般是 GUI 主线程）。它从不亲自搬数据，只下指令、读快照。
//     · 「工作线程」：QtConcurrent::run 从全局线程池借来的后台线程，跑
//       runImport()/runExport()。真正的读 Excel、查库、写库都发生在这里。
//   导入与导出各自独立：两条工作线程可同时存在、互不干扰，各有各的状态机、
//   各有各的停止标志。
//   线程安全的实现手段有两条：
//     ① 互斥量 mutex_ 保护所有「可变的可轮询状态」（state/progress/errors/result）。
//        工作线程写、控制线程读，全部在锁内进行 → getter 返回的是一致快照。
//     ② std::atomic<bool> 停止标志：跨线程单向通知，无需加锁即可安全读写
//        （控制线程置 true，工作线程轮询读）。详见停止协议注释。
//
// 【加锁快照原则】
//   所有 getter 在锁内「整体拷贝一份」再返回（值语义），调用方拿到的是与某一
//   瞬间一致的独立副本，绝不会读到工作线程写到一半的中间态。
//   反过来工作线程每推进一步，也是先抢锁、再批量改这几个字段、再放锁。
//
// 【停止协议（协作式，非抢占）】
//   stop() 不会强杀工作线程——它只是把原子停止标志置 true，并把 Running 态
//   推进到 Stopping 态，然后 waitForFinished() 等工作线程自己跑到下一个检查点
//   读到标志、收尾退出。工作线程在若干「停止检查点」轮询该标志，命中则把状态
//   落到 Stopped（保留已完成的部分结果）后返回。好处：永远不会在持有数据库
//   句柄、写到一半时被打断 → 数据一致性有保障。
//
// 【状态机（每个方向各一份，见 IBatchTransfer.h 的 TransferState）】
//   Idle ──startImport()──▶ Running ──正常跑完──▶ Completed
//                              │                 └─失败──▶ Failed
//                              │                 └─收到停止──▶ Stopped
//                              └──stop()──▶ Stopping ──工作线程收尾──▶ Stopped
//   （Completed/Failed/Stopped 为终态；再次 startImport() 会把它重置回 Running。）
//
// 【协作者一览】
//   · DataBridge          —— 提供 dbPath()、snapshotProfileCatalog()（在拥有者
//                            线程上拷一份 Profile+catalog 给工作线程用，杜绝跨线程
//                            访问主库对象）。
//   · ImportService /     —— 真正的逐行 ETL 引擎；BatchTransfer 只是它们的
//     ExportService          异步外壳，自己开后台连接后委托其 run()。
//   · sync::SyncContext / —— 同步子系统。当某物理库正在同步时，导入需经
//     SyncContextRegistry    ForegroundGate 门控（前台互斥），并改走 SyncWorker
//     / ForegroundGate       的写连接（importFn）以便 session 捕获变更。详见 .cpp。
//   · ProfileValidator    —— 导出前校验 Profile（H-03 修复）。
// ============================================================================

namespace dbridge {

// BatchTransfer —— IBatchTransfer 的实现：把同步的 ImportService/ExportService
// 包装成「后台执行 + 加锁快照轮询 + 协作式停止」的异步传输器。
// 线程安全：所有 public 方法均可从控制线程安全调用；getter 返回加锁快照。
class BatchTransfer : public IBatchTransfer {
   public:
    // 构造：仅保存 DataBridge 引用（不取所有权），不启动任何线程。
    // explicit 防止 DataBridge 隐式转换成 BatchTransfer。
    explicit BatchTransfer(DataBridge& bridge);
    // 析构：尽力优雅停机——置两个停止标志并 waitForFinished() 等后台任务收尾，
    // 避免对象销毁后工作线程仍回写已失效的 this（见 .cpp 析构实现）。
    ~BatchTransfer() override;

    // —— 非阻塞启动（立即返回，真正搬运在工作线程）——
    // 返回 true=已成功排程后台任务；false=拒绝启动（已在运行 / 缺参 / 门控失败），
    // 失败原因写入 *err（若非空）。副作用：成功时重置本方向的轮询状态并置 Running。
    bool startImport(const ImportOptions& options, QString* err = nullptr) override;
    bool startExport(const ExportOptions& options, QString* err = nullptr) override;

    // —— 可轮询 getter（③–⑧，均为 const、均返回加锁拷贝的快照，线程安全）——
    TransferProgress importProgress() const override;  // 导入进度快照
    QList<RowError> importErrors() const override;     // 导入累计错误快照
    ImportResult importResult() const override;        // 导入最终结果（终态后有效）
    TransferProgress exportProgress() const override;  // 导出进度快照
    QList<RowError> exportErrors() const override;     // 导出累计错误快照
    ExportResult exportResult() const override;        // 导出最终结果（终态后有效）

    // —— 停止 + 状态查询 ——
    // stop：协作式请求停止「两个方向」（置原子标志 + Running→Stopping），并阻塞
    // 等待两条工作线程收尾。恒返回 true（当前实现无失败路径，Q_UNUSED(err)）。
    bool stop(QString* err = nullptr) override;
    TransferState importState() const override;  // 导入状态机当前态（加锁读）
    TransferState exportState() const override;  // 导出状态机当前态（加锁读）

   private:
    // —— 工作线程入口（仅由 QtConcurrent::run 在后台线程调用）——
    // 注意参数全部「按值快照」：dbPath/profile/catalog 都是控制线程预先拷好的副本，
    // 工作线程据此独立干活，绝不回触 DataBridge 拥有的可变对象（跨线程安全的根本）。
    void runImport(const ImportOptions& opts, const QString& dbPath,
                   const detail::ProfileSpec& profile, const detail::SchemaCatalog& catalog);
    void runExport(const ExportOptions& opts, const QString& dbPath,
                   const detail::ProfileSpec& profile, const detail::SchemaCatalog& catalog);

    // 持有 DataBridge 的引用（非拥有；其生命周期须长于本对象）。
    // 仅在控制线程的 start*() 里用来取 dbPath/快照，工作线程不直接碰它。
    DataBridge& bridge_;

    // 保护下面「所有可变可轮询状态」的互斥量。
    // mutable：使 const getter 也能加锁（加锁不改变对象逻辑状态）。
    mutable QMutex mutex_;

    // ── 受 mutex_ 保护的状态（工作线程写 / 控制线程读，均须持锁）─────────────────
    TransferState importState_ = TransferState::Idle;  // 导入状态机
    TransferState exportState_ = TransferState::Idle;  // 导出状态机
    TransferProgress importProgress_;                  // 导入进度
    TransferProgress exportProgress_;                  // 导出进度
    QList<RowError> importErrors_;                     // 导入累计错误
    QList<RowError> exportErrors_;                     // 导出累计错误
    ImportResult importResult_;                        // 导入最终结果
    ExportResult exportResult_;                        // 导出最终结果

    // 后台任务句柄：用于 isRunning()/waitForFinished() 探测与等待收尾。
    // 仅在控制线程访问（start*() 赋值、stop()/析构里等待），无需 mutex_ 保护。
    QFuture<void> importFuture_;
    QFuture<void> exportFuture_;

    // 独立的停止标志，使导入与导出可被分别停止。
    // 用 std::atomic：控制线程写 true、工作线程在检查点读，无锁即跨线程安全。
    // Separate stop flags so import and export can be stopped independently.
    std::atomic<bool> importStopRequested_{false};
    std::atomic<bool> exportStopRequested_{false};
};

}  // namespace dbridge
