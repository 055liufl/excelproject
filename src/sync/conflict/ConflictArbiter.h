#pragma once
#include <QHash>
#include <QString>

namespace dbridge::sync {

// Multi-source conflict arbiter using canonical ordering (rank DESC, originSeq DESC).
// Higher rank wins. If ranks equal, higher originSeq wins.
class ConflictArbiter {
   public:
    void setRankMap(const QHash<QString, int>& rankMap);
    int rankOf(const QString& origin) const;

    // Returns true if candidate a beats candidate b.
    bool beats(const QString& aOrigin, qint64 aSeq, const QString& bOrigin, qint64 bSeq) const;

   private:
    QHash<QString, int> rankMap_;
};

}  // namespace dbridge::sync
