#pragma once
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

#include "InboxLedger.h"  // 用台账判断 artifact 是否已处理（去重）

// ============================================================================
// InboxWatcher.h — inbox 目录监视器：扫描 .ready 哨兵，挑出待处理的新 artifact
// ============================================================================
//
// 【在管线中的位置】
//   对端的 OutboxWriter 把 artifact 原子发布到本节点 inbox（主文件 + 同名 .ready 哨兵），
//   InboxWatcher 负责「扫描 inbox」这一环：找出新出现且尚未处理的 artifact，登记进
//   InboxLedger（台账去重），再把它们的完整路径交给上层（SyncWorker）去解码/应用。
//   它只“发现并登记”，不解码、不应用、不删除文件——职责单一。
//
// 【为什么只认 .ready，而不直接扫主文件】
//   发布协议规定：主文件先以 .tmp 写、原子改名就位、最后才创建空的 .ready 哨兵
//   （见 OutboxWriter）。因此「.ready 存在」才等价于「同名主文件数据齐全可消费」。
//   只扫 .ready 就天然避开了“读到半截主文件”的竞态。
//
// 【I-10 fix：为什么是同步 scan() 而不是信号驱动】
//   I-10 fix: SyncWorker uses QWaitCondition::wait() (not exec()), so QTimer and
//   QFileSystemWatcher signals would never fire on the worker thread.  The previous
//   signal-driven design has been replaced with a synchronous scan() method that
//   the worker calls directly in its main loop.
//   译：SyncWorker 的事件循环是用 QWaitCondition::wait() 阻塞等待（而非 QEventLoop::exec()），
//     这意味着依赖 Qt 事件循环投递的 QTimer / QFileSystemWatcher 信号在该工作线程上
//     根本不会触发。所以旧的「信号驱动」设计被改成「同步 scan()」：由 worker 在自己的
//     主循环里直接调用一次扫描，结果立即返回，完全不依赖事件循环。
//
//   The artifactReady signal is retained as a forward-compatible hook for future
//   event-loop-based workers, but it is NOT emitted by the current implementation.
//   译：artifactReady 信号被保留为「面向未来、基于事件循环的 worker」的兼容钩子，
//     当前实现并不发射它（scan() 只返回列表，不 emit）。
//
// 【协作者】
//   · OutboxWriter（对端）—— 产出 .ready + 主文件；
//   · InboxLedger —— 去重台账（status/markSeen）；
//   · SyncWorker —— 周期性调用 scan() 并处理返回的路径，处理后更新台账。
//
// 【线程模型】scan() 必须在 worker 线程上调用，且传入的 db 必须是「该线程自己的」
//   QSqlDatabase 连接（QSqlDatabase 不可跨线程共享，这是硬约束）。
// ============================================================================

namespace dbridge::sync {

// InboxWatcher —— 监视一个 inbox 目录里的 .ready 哨兵文件（见上方设计说明）。
class InboxWatcher : public QObject {
    Q_OBJECT
   public:
    // 构造：绑定 inbox 目录、一个 db 引用（遗留用途，见下）、去重台账 ledger。
    explicit InboxWatcher(const QString& inboxDir, QSqlDatabase& db, InboxLedger& ledger,
                          QObject* parent = nullptr);

    // Synchronous scan: called directly on the worker thread.
    // Scans the inbox directory for *.ready files, updates the ledger for newly-seen
    // artifacts, and returns a list of full artifact file paths ready for processing.
    // db must be the worker thread's own QSqlDatabase.
    // 译：同步扫描，由 worker 线程直接调用。
    //   扫描 inbox 下的 *.ready 文件，为「新发现」的 artifact 更新台账（markSeen），
    //   返回一批「可供处理」的 artifact 完整路径（不是仅文件名）。
    //   db 必须是该 worker 线程自己的 QSqlDatabase（不可跨线程）。
    // 【返回】本轮新发现、已登记、且主文件确实存在的 artifact 路径列表（可能为空）。
    // 【副作用】对未登记的 artifact 写一条 'seen' 台账记录。
    QStringList scan(QSqlDatabase& db);

   signals:
    // Reserved for future event-loop-based workers.  Not emitted by scan().
    // 译：为未来「基于事件循环的 worker」预留的钩子；当前 scan() 并不发射它。
    void artifactReady(const QString& artifactPath);

   private:
    QString dir_;       // 被监视的 inbox 目录路径
    QSqlDatabase& db_;  // kept for legacy reference; scan() accepts an explicit db arg
                        // 译：保留作遗留引用；实际查询一律用 scan() 显式传入的 db 参数
                        // （以确保使用的是「调用线程自己的」连接，而非构造时那一个）。
    InboxLedger& ledger_;  // 去重台账（引用持有，外部拥有其生命周期）
};

}  // namespace dbridge::sync
