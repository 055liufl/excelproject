#pragma once
#include <QSqlDatabase>
#include <QString>

namespace dbridge::sync {

// RAII wrapper for BEGIN IMMEDIATE / COMMIT / ROLLBACK on wconn.
// Owner of CapturedWriteTemplate; not copyable.
class WriteTxn {
   public:
    explicit WriteTxn(QSqlDatabase& db) : db_(db) {
    }
    ~WriteTxn() {
        if (active_)
            rollback();
    }

    WriteTxn(const WriteTxn&) = delete;
    WriteTxn& operator=(const WriteTxn&) = delete;

    bool begin(QString* err = nullptr);
    bool commit(QString* err = nullptr);
    void rollback();

    bool isActive() const {
        return active_;
    }

   private:
    QSqlDatabase& db_;
    bool active_ = false;
};

}  // namespace dbridge::sync
