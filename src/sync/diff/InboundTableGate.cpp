#include "InboundTableGate.h"

namespace dbridge::sync {

void InboundTableGate::open(const QStringList& watchedTables) {
    QMutexLocker lock(&mutex_);
    watchedTables_ = watchedTables;
}

bool InboundTableGate::shouldDefer(const QSet<QString>& payloadTables) const {
    QMutexLocker lock(&mutex_);
    for (const QString& t : payloadTables) {
        if (watchedTables_.contains(t))
            return true;
    }
    return false;
}

void InboundTableGate::releaseAll() {
    QMutexLocker lock(&mutex_);
    watchedTables_.clear();
}

bool InboundTableGate::isOpen() const {
    QMutexLocker lock(&mutex_);
    return !watchedTables_.isEmpty();
}

}  // namespace dbridge::sync
