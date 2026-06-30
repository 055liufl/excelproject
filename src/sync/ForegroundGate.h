#pragma once
#include <QMutex>
#include <QString>

// ============================================================================
// ForegroundGate.h — 前台操作互斥门控（“同一时刻只允许一个前台操作”的轻量信号灯）
// ============================================================================
//
// 【这个文件是什么】
//   一个最小化的“非递归二元互斥闸门”：内部只有一个布尔标志 held_ 表示“门是否被占”，
//   外加一把 QMutex 保护该标志的并发读写。它不是普通互斥锁——不会阻塞等待，而是
//   “抢得到就 true、抢不到就立即 false + 报 E_BUSY”，是一种 try-lock 语义的占位旗。
//
// 【为什么需要它（设计动机）】
//   同步子系统把操作分成两类：
//     · 前台操作：用户显式发起的 sync() / syncSelected() / 比对会话。它们语义上彼此
//       排他——同一个库同一时刻只应有一个“人为发起”的操作在跑，否则进度/ACK 窗口/
//       门控状态会互相踩踏。ForegroundGate 就是用来串行化这类操作的。
//     · 后台活动：SyncWorker 自驱的 inbox 扫描 / 应用入站 / 回 ACK / 周期广播。
//       这些【不受】本门控约束，始终在后台线程持续运转（见类注释）。
//   每个被同步的物理库（每个 SyncContext）持有一个 ForegroundGate 实例（见
//   SyncContext::gate），从而把“前台互斥”的粒度精确收口在“单个库”这一层。
//
// 【典型使用流程（建立直觉）】
//   SyncEngine::sync(): tryAcquire() 成功后才推进状态机；最终在到达终态
//   （Completed/Failed/Stopped）时由 releaseGateIfTerminal() 调 release() 还闸。
//   失败/异常路径必须保证 release()，否则后续前台操作会永远拿到 E_BUSY。
//
// 【线程模型】
//   全部方法都在持锁（QMutexLocker）下读写 held_，因此可被任意线程安全调用
//   （前台调用线程取/还闸；后台不碰它）。mutex_ 标 mutable，使 const 的 isHeld()
//   也能加锁。注意它【不可重入】：同一线程已 tryAcquire() 成功后再次 tryAcquire()
//   会拿到 E_BUSY——前台操作之间本就应当串行，不存在嵌套获取的合法场景。
// ============================================================================

namespace dbridge::sync {

// ForegroundGate —— 每个 DataBridge（每个被同步库）一份的“前台门控”：同一时刻至多
// 一个活动的前台操作。后台活动（inbox 应用 / 广播）不受本门控约束。
// 【不变量】held_ 在 mutex_ 保护下读写；release() 幂等（重复 release 只是把 held_ 再置 false）。
class ForegroundGate {
   public:
    // 尝试占用闸门（不阻塞）。成功置 held_=true 并返回 true；
    // 失败（已被占）则返回 false 并把 *err 置为 "E_BUSY"。
    // 【为什么不阻塞等待】前台操作应当“要么立即开始、要么明确告知忙”，
    // 而不是排队挂起——挂起会让 UI/调用方误以为操作正在进行。
    bool tryAcquire(QString* err = nullptr) {
        QMutexLocker lk(&mutex_);
        if (held_) {  // 已有前台操作占用 → 直接拒绝，不等待。
            if (err)
                *err = QStringLiteral("E_BUSY");  // 错误码见 Errors.h::E_BUSY
            return false;
        }
        held_ = true;  // 抢占成功，闸门关上。
        return true;
    }

    // 释放闸门：把 held_ 置回 false，让下一个前台操作可以 tryAcquire()。
    // 幂等：未持有时调用也安全（只是无效果）。务必在每条终止路径上调用，
    // 否则闸门“常关”，后续操作将永久 E_BUSY。
    void release() {
        QMutexLocker lk(&mutex_);
        held_ = false;
    }

    // 查询闸门当前是否被占（线程安全只读快照）。
    bool isHeld() const {
        QMutexLocker lk(&mutex_);
        return held_;
    }

   private:
    mutable QMutex mutex_;  // 保护 held_ 的互斥锁；mutable 使 const 的 isHeld() 也能加锁。
    bool held_ = false;  // 闸门状态：true=已被某前台操作占用。
};

}  // namespace dbridge::sync
