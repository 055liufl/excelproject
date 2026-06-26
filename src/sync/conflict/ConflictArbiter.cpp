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

    if (aSeq != bSeq) {
        return aSeq > bSeq;
    }

    // H-01 fix: rank == rank and seq == seq — use originId as a stable, deterministic
    // tie-breaker so that applying changesets in any order yields the same final state.
    // Lexicographically larger originId wins (arbitrary but consistent).
    return aOrigin > bOrigin;
}

}  // namespace dbridge::sync
