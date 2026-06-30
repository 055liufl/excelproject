// ============================================================================
// WriteTxn.cpp — 写事务 RAII 封装的实现（声明见 WriteTxn.h）
// ============================================================================
// 本文件只是把三条事务命令（BEGIN IMMEDIATE / COMMIT / ROLLBACK）落成最朴素的
// QSqlQuery::exec()，并维护 active_ 这一状态位（供析构 RAII 与 isActive() 判断）。
// 设计背景、为何用 IMMEDIATE、与 origin_seq 回退的配合等，详见头文件类注释。
// ============================================================================
#include "WriteTxn.h"

#include <QSqlError>
#include <QSqlQuery>

namespace dbridge::sync {

// begin —— 开启写事务。
// 做什么：在写连接上执行 "BEGIN IMMEDIATE"，立即取得 RESERVED 写锁。
// 为什么用 IMMEDIATE：把“能否写”的判定提前到事务起点（见头文件）；单写者模型下
//   写锁争用应当尽早、确定地暴露，而非拖到中途某条写语句才报 locked。
// 参数：err 失败原因输出（可空）。返回：成功 true；失败 false（并写 *err）。
// 副作用：成功后 active_=true（此后若不 commit，析构会自动 rollback）。
bool WriteTxn::begin(QString* err) {
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        if (err)
            *err = q.lastError().text();  // 透传底层 SQLite 错误文本（如 "database is locked"）
        return false;  // 注意：失败时 active_ 保持 false，无悬挂事务。
    }
    active_ = true;
    return true;
}

// commit —— 提交事务。
// 做什么：执行 "COMMIT" 把本事务的全部改动落盘。
// 错误模式：COMMIT 失败（极少见，如 I/O 错误）时，主动 rollback() 给事务一个明确结局，
//   避免事务半开半闭把连接锁住；随后返回 false 并写 *err。
// 副作用：成功 active_=false（事务正常结束）；失败经 rollback() 后 active_ 同样为 false。
bool WriteTxn::commit(QString* err) {
    QSqlQuery q(db_);
    if (!q.exec(QStringLiteral("COMMIT"))) {
        if (err)
            *err = q.lastError().text();
        rollback();  // 兜底收尾：保证事务不会停留在“已 BEGIN 未结束”的悬挂态。
        return false;
    }
    active_ = false;
    return true;
}

// rollback —— 回滚事务。
// 做什么：若当前确有进行中的事务，执行 "ROLLBACK" 丢弃其全部改动。
// 幂等：active_ 为 false 时直接返回（no-op），故重复调用、或在 commit 失败路径里再调都安全。
// 注意：刻意忽略 ROLLBACK 的返回值——回滚阶段已无更优的补救手段，无论成败都将事务视为结束
//   并把 active_ 复位（避免析构再次尝试回滚）。DB 事务回滚【不会】自动退回已预分配的
//   origin_seq，调用方须另行 SyncWorker::rollbackOriginSeq()（见头文件“引擎中的位置”）。
void WriteTxn::rollback() {
    if (!active_)
        return;
    QSqlQuery q(db_);
    q.exec(QStringLiteral("ROLLBACK"));
    active_ = false;
}

}  // namespace dbridge::sync
