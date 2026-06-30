#include "sync/conflict/ConflictArbiter.h"

// ConflictArbiter 的实现:全部是无状态的纯比较逻辑,除 rankMap_ 外不持有任何状态,
// 因此线程安全(只读)。详见头文件对“规范序”裁定规则的说明。

namespace dbridge::sync {

// 保存“origin → rank”映射(覆盖式;通常在引擎初始化时调用一次)。
void ConflictArbiter::setRankMap(const QHash<QString, int>& rankMap) {
    rankMap_ = rankMap;
}

// 查 rank;未登记的 origin 返回默认值 0(最低优先级)。
// 这样“未知来源”天然输给任何被显式配置了正 rank 的节点。
int ConflictArbiter::rankOf(const QString& origin) const {
    return rankMap_.value(origin, 0);
}

// 判断候选 a 是否击败候选 b。比较顺序:rank → seq → originId(全序,确定且可重复)。
bool ConflictArbiter::beats(const QString& aOrigin, qint64 aSeq, const QString& bOrigin,
                            qint64 bSeq) const {
    int ra = rankOf(aOrigin);
    int rb = rankOf(bOrigin);

    // ① 第一关键字:rank。高 rank 直接胜出(权威等级最高者说了算)。
    if (ra != rb) {
        return ra > rb;
    }

    // ② 次关键字:originSeq。rank 相同则更“新”(seq 更大)的版本胜出。
    if (aSeq != bSeq) {
        return aSeq > bSeq;
    }

    // ③ H-01 fix:rank 相同且 seq 也相同时——用 originId 作为稳定、确定的决胜手段,
    //    使得“以任意顺序应用这些 changeset”都得到相同的最终状态(避免顺序相关分叉)。
    //    规则:originId 字典序更大者胜(取值本身是任意的,关键在于全局一致)。
    return aOrigin > bOrigin;
}

}  // namespace dbridge::sync
