#pragma once
#include <QMutex>
#include <QString>

namespace dbridge::sync {

// Per-DataBridge foreground gate: at most one active foreground operation.
// Background (inbox apply / broadcast) is NOT subject to this gate.
class ForegroundGate {
   public:
    // Try to acquire. Returns false + sets *err = E_BUSY on failure.
    bool tryAcquire(QString* err = nullptr) {
        QMutexLocker lk(&mutex_);
        if (held_) {
            if (err)
                *err = QStringLiteral("E_BUSY");
            return false;
        }
        held_ = true;
        return true;
    }

    void release() {
        QMutexLocker lk(&mutex_);
        held_ = false;
    }

    bool isHeld() const {
        QMutexLocker lk(&mutex_);
        return held_;
    }

   private:
    mutable QMutex mutex_;
    bool held_ = false;
};

}  // namespace dbridge::sync
