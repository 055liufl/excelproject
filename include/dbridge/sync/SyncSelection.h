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
        // H-11 fix: reject non-simple identifiers to prevent SQL injection via table name.
        if (!isSimpleIdent(table))
            invalidTables_.append(table);
        else
            sel_.records_.append({table, pk});
        return *this;
    }
    Builder& addRecords(const QString& table, const QStringList& pks) {
        if (!isSimpleIdent(table)) {
            invalidTables_.append(table);
            return *this;
        }
        for (const auto& pk : pks)
            sel_.records_.append({table, pk});
        return *this;
    }
    // H-01 fix: raw SQL WHERE is not yet supported (design §4.4: MVP = PK set only).
    // Storing the expression to produce a consistent error at build() time (not silently).
    Builder& addWhere(const QString& table, const QString& whereExpr) {
        if (!whereExpr.isEmpty())
            rawWhereAttempts_.append(table);
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
        // H-11 fix: reject invalid table identifiers.
        if (!invalidTables_.isEmpty()) {
            if (err)
                *err = QStringLiteral(
                           "E_SYNC_SELECTION_EMPTY: table name '%1' is not a valid "
                           "SQLite identifier (must match [A-Za-z_][A-Za-z0-9_]*)")
                           .arg(invalidTables_.first());
            return SyncSelection{};
        }
        // H-01 fix: reject raw-SQL addWhere() calls — only PK-set selection is supported in MVP.
        if (!rawWhereAttempts_.isEmpty()) {
            if (err)
                *err = QStringLiteral(
                           "E_SYNC_SELECTION_EMPTY: addWhere() with raw SQL is not "
                           "supported in MVP (design §4.4); use addRecord()/addRecords() "
                           "for table '%1'")
                           .arg(rawWhereAttempts_.first());
            return SyncSelection{};
        }
        if (sel_.isEmpty()) {
            if (err)
                *err = QStringLiteral("E_SYNC_SELECTION_EMPTY: selection is empty");
        }
        return sel_;
    }

   private:
    SyncSelection sel_;
    QStringList rawWhereAttempts_;
    QStringList invalidTables_;

    // H-11: simple identifier = starts with letter/underscore, contains only [A-Za-z0-9_].
    static bool isSimpleIdent(const QString& s) {
        if (s.isEmpty())
            return false;
        const QChar first = s[0];
        if (!first.isLetter() && first != QLatin1Char('_'))
            return false;
        for (const QChar& c : s) {
            if (!c.isLetterOrNumber() && c != QLatin1Char('_'))
                return false;
        }
        return true;
    }
};

}  // namespace dbridge::sync
