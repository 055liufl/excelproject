#pragma once
#include <QMutex>
#include <QSet>
#include <QStringList>

namespace dbridge::sync {

// During a comparison session, defers inbox processing for tables under comparison.
class InboundTableGate {
   public:
    // Open the gate for the given tables (start deferring).
    void open(const QStringList& watchedTables);

    // Returns true if any of payloadTables intersects watchedTables_ (should defer).
    bool shouldDefer(const QSet<QString>& payloadTables) const;

    // Release gate (allow processing to resume).
    void releaseAll();

    bool isOpen() const;

   private:
    mutable QMutex mutex_;
    QStringList watchedTables_;
};

}  // namespace dbridge::sync
