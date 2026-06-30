#pragma once
#include <QMutex>
#include <QSet>
#include <QStringList>

// ============================================================================
// InboundTableGate.h — 比对期间「门控/暂停」特定表的 inbox 入站变更应用
// ============================================================================
//
// 【这个类是什么 / 在 diff 链路中的位置】
//   diff 三件套（ComparisonSession 会话 + StagingBuffer 暂存 + InboundTableGate 门控）
//   里的「门控」。它是 ComparisonSession 与后台 SyncWorker（inbox 应用线程）之间的一个
//   极简共享开关：在交互式比对进行期间，把「针对正被比对的那些表」的入站变更（inbox 里
//   待应用的对端 artifact）暂时挡住、推迟应用；比对结束（save/discard）后再放行。
//
// 【为什么需要门控（这是关键，不直观）】
//   ComparisonSession 在 initialize() 时会用 BEGIN DEFERRED 钉住一个「本地读快照」，
//   随后人在界面上逐行/逐单元格地看差异、做取舍决定（可能耗时几秒到几分钟）。如果这段
//   时间里后台 SyncWorker 又把对端的新变更应用到了同一张表，就会出现：
//     · 用户看到的本地值与库里实际值不再一致（决策基于过期画面）；
//     · save() 时 PRAGMA data_version 检测到库被改 → 整批暂存判定为 stale 而被拒
//       （E_SYNC_STAGE_STALE），用户白看一场。
//   门控把这些表的 inbox 应用「冻结」起来，最大限度避免比对期间底层数据被搅动；被推迟的
//   变更不会丢——releaseAll() 后由 ComparisonSession 触发 rescanFn() 重新扫描 inbox 补上。
//
// 【协作者】
//   · ComparisonSession：initialize() 调 open(被比对表) 开门控；save()/discard() 调
//     releaseAll() 放行。会话本身不直接读 shouldDefer。
//   · SyncWorker（inbox 应用侧）：每要应用一个 artifact 前，先用 shouldDefer(本包涉及的
//     表集合) 询问；返回 true 就把该包搁置、留待下次 rescan。
//
// 【线程模型（重要）】
//   open()/shouldDefer()/releaseAll()/isOpen() 跨线程调用（会话线程写、worker 线程读），
//   故内部用 QMutex 全程加锁，本类自身是线程安全的。注意「门控」语义本身仍存在固有竞态
//   窗口：open() 与某个已经越过 shouldDefer 检查的 artifact 之间无法绝对互斥——这由会话的
//   data_version stale 检测兜底，门控只是「尽量减少搅动」而非「绝对隔离」。
//
// 【命名提醒：isOpen()==true 表示「门控生效中」（正在拦截），而非「门是敞开放行的」】
//   open=开启门控=开始拦截；releaseAll=释放门控=恢复放行。语义以「门卫上岗/下岗」理解。
// ============================================================================

namespace dbridge::sync {

// During a comparison session, defers inbox processing for tables under comparison.
// 比对会话进行期间，推迟对「正被比对的表」的 inbox 入站变更处理。
class InboundTableGate {
   public:
    // Open the gate for the given tables (start deferring).
    // 开启门控：登记一批「受监视表」，从此刻起拦截针对这些表的 inbox 应用。
    //   做什么：用 watchedTables 整体替换内部监视集合（注意是替换而非追加）。
    //   何时调用：ComparisonSession::initialize() 末尾，传入本次参与比对的所有表。
    //   参数：watchedTables —— 需要冻结 inbox 应用的表名列表。
    //   线程：加锁；可与 worker 线程的 shouldDefer 并发。
    //   复杂度：O(n)（一次列表赋值）。
    void open(const QStringList& watchedTables);

    // Returns true if any of payloadTables intersects watchedTables_ (should defer).
    // 询问门控：本入站包是否应被推迟？
    //   做什么：判断 payloadTables（该 artifact 涉及的表集合）与受监视集合是否有交集，
    //           只要命中任意一张被监视的表，就返回 true（=请稍后再应用）。
    //   为什么用「任一相交即推迟」：一个 artifact 可能同时改多张表，只要其中一张正被比对，
    //           整包就不能现在应用（否则会部分搅动被比对的快照）。
    //   参数：payloadTables —— 待应用包所涉及的表名集合。
    //   返回：true=应推迟（命中监视表）；false=可立即应用（门控未开或未命中）。
    //   线程：加锁；由 SyncWorker 的 inbox 应用线程高频调用。
    //   复杂度：O(|payloadTables| × |watchedTables_|)（线性查找；表数通常很小）。
    bool shouldDefer(const QSet<QString>& payloadTables) const;

    // Release gate (allow processing to resume).
    // 释放门控：清空受监视集合，恢复所有表的 inbox 正常应用。
    //   做什么：把 watchedTables_ 清空，此后 shouldDefer 对任何表都返回 false。
    //   何时调用：ComparisonSession 的 save()/discard() 收尾时（无论成败都要释放，
    //           否则这些表会被永久卡住）。释放后会话还会触发 rescanFn() 让被推迟的包补应用。
    //   线程：加锁。复杂度：O(1)。
    void releaseAll();

    // 门控当前是否生效（受监视集合非空即视为「上岗中」）。供诊断/断言用。
    //   线程：加锁。复杂度：O(1)。
    bool isOpen() const;

   private:
    mutable QMutex mutex_;  // 保护 watchedTables_ 的互斥锁；mutable 以便 const 方法内也能加锁
    QStringList watchedTables_;  // 当前被门控（冻结 inbox 应用）的表名集合；空=门控未生效
};

}  // namespace dbridge::sync
