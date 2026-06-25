#include "sync/conflict/ConflictArbiter.h"

namespace dbridge::sync {

void ConflictArbiter::setRankMap(const QHash<QString, int>& rankMap) {
    rankMap_ = rankMap;
}

int ConflictArbiter::rankOf(const QString& origin) const {
    return rankMap_.value(origin, 0);
}

bool ConflictArbiter::beats(const QString& aOrigin, qint64 aSeq, const QString& bOrigin,
                            qint64 bSeq) const {
    int ra = rankOf(aOrigin);
    int rb = rankOf(bOrigin);

    if (ra != rb) {
        return ra > rb;
    }

    return aSeq > bSeq;
}

}  // namespace dbridge::sync
