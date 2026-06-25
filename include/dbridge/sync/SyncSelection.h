#pragma once
#include "dbridge/Export.h"

#include <QStringList>

namespace dbridge::sync {

class DBRIDGE_EXPORT SyncSelection {
   public:
    class Builder;

    struct Record {
        QString table;
        QString primaryKey;
    };
    struct WhereClause {
        QString table;
        QString whereExpr;
    };

    QList<Record> records() const {
        return records_;
    }
    QList<WhereClause> whereClauses() const {
        return whereClauses_;
    }
    bool includeFkDeps() const {
        return includeFkDeps_;
    }
    bool pruneConsistent() const {
        return pruneConsistent_;
    }
    bool isEmpty() const {
        return records_.isEmpty() && whereClauses_.isEmpty();
    }

   private:
    friend class Builder;
    SyncSelection() = default;
    QList<Record> records_;
    QList<WhereClause> whereClauses_;
    bool includeFkDeps_ = true;
    bool pruneConsistent_ = true;
};

class DBRIDGE_EXPORT SyncSelection::Builder {
   public:
    Builder() = default;
    Builder& addRecord(const QString& table, const QString& pk) {
        sel_.records_.append({table, pk});
        return *this;
    }
    Builder& addRecords(const QString& table, const QStringList& pks) {
        for (const auto& pk : pks)
            sel_.records_.append({table, pk});
        return *this;
    }
    Builder& addWhere(const QString& table, const QString& whereExpr) {
        sel_.whereClauses_.append({table, whereExpr});
        return *this;
    }
    Builder& includeFkDependencies(bool on = true) {
        sel_.includeFkDeps_ = on;
        return *this;
    }
    Builder& pruneConsistentDependencies(bool on = true) {
        sel_.pruneConsistent_ = on;
        return *this;
    }

    SyncSelection build(QString* err = nullptr) {
        if (sel_.isEmpty()) {
            if (err)
                *err = QStringLiteral("E_SYNC_SELECTION_EMPTY: selection is empty");
        }
        return sel_;
    }

   private:
    SyncSelection sel_;
};

}  // namespace dbridge::sync
