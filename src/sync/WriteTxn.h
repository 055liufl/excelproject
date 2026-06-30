#pragma once
#include <QSqlDatabase>
#include <QString>

// ============================================================================
// WriteTxn.h — 写事务的 RAII 封装（BEGIN IMMEDIATE / COMMIT / ROLLBACK）
// ============================================================================
//
// 【这个文件是什么】
//   对“一段写事务”的极简 RAII（资源获取即初始化）包装。它把 SQLite 事务的三态命令
//   （开始 / 提交 / 回滚）封进一个栈对象，最大价值在于：只要对象离开作用域而事务还
//   “开着”（未提交也未回滚），析构函数就自动 ROLLBACK——杜绝因 early-return / 抛异常
//   而把事务永久悬挂（进而把写连接锁死）的经典 bug。
//
// 【为什么用 BEGIN IMMEDIATE（关键不直观处）】
//   SQLite 默认的 BEGIN 是 DEFERRED：直到第一条写语句才真正取写锁，可能在事务中途
//   才发现“database is locked”，此时已做了一半工作，回滚成本高、且易产生死锁。
//   IMMEDIATE 则在 BEGIN 时就立刻取得 RESERVED 写锁：要么当场拿到写权、要么当场失败，
//   把“能不能写”的判定提前到事务起点。本子系统是【单写者】模型（写连接由 SyncWorker
//   独占），用 IMMEDIATE 让写权争用尽早暴露，语义最清晰。
//
// 【它在引擎中的位置】
//   被 CapturedWriteTemplate（三分支写模板：入站 changeset / 选择性推送 / 本地写）持有，
//   用来把“分配 origin_seq → session 捕获 → 写 __sync_changelog → 提交”这一串原子化。
//   一旦事务中途失败而回滚，调用方还须额外用 SyncWorker::rollbackOriginSeq() 把已预分配
//   的 origin_seq 退回，避免序列号出现空洞（gap）——回滚 DB 事务并不会自动退回那个计数器。
//
// 【线程模型】
//   仅在 SyncWorker 的写线程上、对该线程独占的写连接 wconn 使用（与连接同线程）。
//   不可拷贝（删除拷贝构造/赋值），因为它语义上“独占”着一个进行中的事务。
// ============================================================================

namespace dbridge::sync {

// WriteTxn —— 对写连接 wconn 上 BEGIN IMMEDIATE / COMMIT / ROLLBACK 的 RAII 封装。
// 由 CapturedWriteTemplate 持有；不可拷贝。
class WriteTxn {
   public:
    // 构造：仅保存连接引用，【不】立即开事务（须显式 begin()）。
    // db 必须在本对象整个生命周期内保持有效（持有的是引用，不延长其生命周期）。
    explicit WriteTxn(QSqlDatabase& db) : db_(db) {
    }
    // 析构：RAII 兜底——若事务仍处于 active_（已 begin 但未 commit/rollback），自动回滚。
    // 这保证任何 early-return / 异常路径都不会把事务悬挂、把写连接锁住。
    ~WriteTxn() {
        if (active_)
            rollback();
    }

    // 禁止拷贝：一个进行中的事务在语义上独占，复制会导致双重提交/回滚。
    WriteTxn(const WriteTxn&) = delete;
    WriteTxn& operator=(const WriteTxn&) = delete;

    // 开始事务（执行 BEGIN IMMEDIATE，立即取写锁）。成功置 active_=true 返回 true；
    // 失败（如写锁被占/语法异常）返回 false 并把 SQL 错误文本写入 *err。
    bool begin(QString* err = nullptr);
    // 提交事务（执行 COMMIT）。成功置 active_=false 返回 true。
    // 失败时会主动 rollback() 收尾（事务必须有明确结局），并返回 false + 写 *err。
    bool commit(QString* err = nullptr);
    // 回滚事务（执行 ROLLBACK）。未处于 active_ 时是 no-op，故可安全重复调用。
    void rollback();

    // 当前是否有进行中的事务（已 begin 且未 commit/rollback）。
    bool isActive() const {
        return active_;
    }

   private:
    QSqlDatabase& db_;  // 目标写连接（引用，须在本对象生命周期内有效）。
    bool active_ = false;  // 事务是否进行中：begin() 置 true，commit()/rollback() 置 false。
};

}  // namespace dbridge::sync
