// ============================================================================
// InboundTableGate.cpp — 门控的实现（极简共享状态 + 全程加锁）
// ============================================================================
//
// 实现策略一句话：内部就是「一个被互斥锁保护的表名列表」。所有四个方法都先取锁，
// 故无论会话线程还是 worker 线程怎么交错调用，watchedTables_ 的读写都不会撕裂。
// 这里没有条件变量、没有等待——shouldDefer 只做一次「问一下」的快照式判断，立即返回。
// ============================================================================

#include "InboundTableGate.h"

namespace dbridge::sync {

// 开启门控：整体替换受监视表集合（注意：是覆盖式赋值，不是并集追加；
// 一次比对会话内 open 只调一次，故无需累加）。加锁防止与 worker 的读相撞。
void InboundTableGate::open(const QStringList& watchedTables) {
    QMutexLocker lock(&mutex_);
    watchedTables_ = watchedTables;
}

// 询问是否推迟：遍历入站包涉及的表，命中任意一张受监视表即返回 true（短路返回）。
// 全程持锁，读到的 watchedTables_ 是某一时刻的一致快照。
bool InboundTableGate::shouldDefer(const QSet<QString>& payloadTables) const {
    QMutexLocker lock(&mutex_);
    for (const QString& t : payloadTables) {
        if (watchedTables_.contains(t))
            return true;  // 该包碰到了正被比对的表 → 现在别应用，留待 rescan
    }
    return false;  // 门控未开（集合空）或本包与被比对表无关 → 放行
}

// 释放门控：清空集合。此后 shouldDefer 对任何表都返回 false，inbox 应用恢复正常。
// 必须在会话收尾时调用，否则被监视表的入站变更会被永久搁置。
void InboundTableGate::releaseAll() {
    QMutexLocker lock(&mutex_);
    watchedTables_.clear();
}

// 门控是否生效：集合非空即「上岗中」。仅作状态查询，同样持锁读取。
bool InboundTableGate::isOpen() const {
    QMutexLocker lock(&mutex_);
    return !watchedTables_.isEmpty();
}

}  // namespace dbridge::sync
