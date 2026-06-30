// ============================================================================
// tst_sync_foreground_gate.cpp — ForegroundGate（前台互斥闸门）单元测试
// ============================================================================
//
// 【被测对象是什么】
//   ForegroundGate 是同步引擎里的「前台操作互斥闸门」。同一物理库同一时刻最多允许一个
//   用户显式发起的前台操作（sync / syncSelected）在跑；再来一个必须被拒绝。它本质上是一个
//   容量为 1 的非阻塞门闩，提供三个动作：
//     · tryAcquire(&err) —— 尝试抢占：空闲则抢到返回 true；已被占用则【不阻塞】、立刻返回
//                           false 并把错误码 E_BUSY 写进 *err。
//     · release()        —— 释放：把门重新置为空闲（未持有时调用是安全的 no-op）。
//     · isHeld()         —— 查询：当前是否被持有。
//
// 【为什么需要它（在系统中的价值）】
//   见 SyncEngine.cpp：sync()/syncSelected() 必须互斥——一次前台操作要一直「持门」到 ACK
//   收齐或超时才 release，期间第二个前台请求应当快速失败（E_BUSY）而不是排队阻塞调用线程。
//   ForegroundGate 把这条「至多一个前台操作」的不变量收敛成一个可独立测试的小对象。
//
// 【测试策略】纯内存对象、无数据库、无线程：每个用例 new 一个本地 ForegroundGate，
//   围绕「抢占/再抢占/释放/空释放」四种状态迁移断言其行为。E_BUSY 来自 dbridge/Errors.h。
// ============================================================================
#include "dbridge/Errors.h"

#include <QtTest>

#include "sync/ForegroundGate.h"

using namespace dbridge::sync;

class TstSyncForegroundGate : public QObject {
    Q_OBJECT
   private slots:
    // initialState —— 契约：刚构造的闸门处于「未持有」（空闲）状态。
    void initialState() {
        ForegroundGate g;
        QVERIFY(!g.isHeld());
    }

    // tryAcquire_firstSucceeds —— 契约：首次抢占必成功。
    // THEN 返回 true、isHeld() 变 true、且 err 为空（成功路径不写错误）。
    void tryAcquire_firstSucceeds() {
        ForegroundGate g;
        QString err;
        QVERIFY(g.tryAcquire(&err));
        QVERIFY(g.isHeld());
        QVERIFY(err.isEmpty());
    }

    // tryAcquire_reentrant_E_BUSY —— 核心不变量：已被占用时再抢必失败、且报 E_BUSY。
    // GIVEN 第一次 tryAcquire 已抢到  WHEN 同一个门上第二次 tryAcquire
    // THEN 第二次返回 false、err2 含错误码 E_BUSY、且门仍由「第一持有者」持有（isHeld 仍 true）。
    // 这正是「同一时刻至多一个前台操作」的本质——非阻塞拒绝，而非排队等待。
    void tryAcquire_reentrant_E_BUSY() {
        ForegroundGate g;
        QString err1, err2;
        QVERIFY(g.tryAcquire(&err1));
        QVERIFY(!g.tryAcquire(&err2));  // second fails（第二次失败）
        QVERIFY(err2.contains(QLatin1String(dbridge::err::E_BUSY)));  // E_BUSY（错误码正确）
        QVERIFY(g.isHeld());  // still held（仍被第一者持有）
    }

    // release_clearsHeld —— 契约：release() 后门回到未持有。
    void release_clearsHeld() {
        ForegroundGate g;
        QString err;
        g.tryAcquire(&err);
        g.release();
        QVERIFY(!g.isHeld());
    }

    // reacquireAfterRelease —— 契约：释放后可被再次抢占（门可复用，非一次性）。
    // GIVEN 抢占→释放  WHEN 再次 tryAcquire  THEN 再次成功、isHeld() 为 true。
    // 业务含义：一个前台操作结束放门后，下一个前台操作能立即开始。
    void reacquireAfterRelease() {
        ForegroundGate g;
        QString err;
        g.tryAcquire(&err);
        g.release();
        QVERIFY(g.tryAcquire(&err));
        QVERIFY(g.isHeld());
    }

    // release_whenNotHeld_noop —— 契约：在「未持有」状态下 release() 是安全的 no-op。
    // 为什么重要：SyncEngine 的 releaseGateIfTerminal 等收口逻辑可能在没人持门时被调到，
    //            「空释放」必须不崩溃、不把状态搅乱（门仍保持未持有）。
    void release_whenNotHeld_noop() {
        ForegroundGate g;
        g.release();  // must not crash（空释放不得崩溃）
        QVERIFY(!g.isHeld());
    }
};

// 无 GUI 的测试入口；末行引入 moc 生成代码（Qt Test 单文件固定写法）。
QTEST_APPLESS_MAIN(TstSyncForegroundGate)
#include "tst_sync_foreground_gate.moc"
