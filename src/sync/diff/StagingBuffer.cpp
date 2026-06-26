#include "StagingBuffer.h"

#include "../WriteTxn.h"

namespace dbridge::sync {

void StagingBuffer::stage(const QString& table, const QString& pk, const QVariantMap& row) {
    for (StagedRow& sr : staged_) {
        if (sr.table == table && sr.pk == pk) {
            sr.row = row;
            return;
        }
    }
    staged_.append(StagedRow{table, pk, row});
}

void StagingBuffer::unstage(const QString& table, const QString& pk) {
    for (int i = 0; i < staged_.size(); ++i) {
        if (staged_[i].table == table && staged_[i].pk == pk) {
            staged_.removeAt(i);
            return;
        }
    }
}

bool StagingBuffer::save(QSqlDatabase& wconn, UpsertExecutor& upsert, const QStringList& pkCols,
                         QString* err) {
    if (staged_.isEmpty())
        return true;

    // Group all staged rows into a single batch of RowMutations.
    QList<RowMutation> mutations;
    mutations.reserve(staged_.size());

    for (const StagedRow& sr : staged_) {
        RowMutation rm;
        rm.table = sr.table;
        rm.columns = sr.row.keys();
        rm.values = sr.row.values();
        rm.pkColumns = pkCols;
        rm.mode = UpsertMode::DoUpdate;
        mutations.append(rm);
    }

    WriteTxn txn(wconn);
    QString beginErr;
    if (!txn.begin(&beginErr)) {
        if (err)
            *err = beginErr;
        return false;
    }

    QString applyErr;
    if (!upsert.apply(wconn, mutations, nullptr, &applyErr)) {
        txn.rollback();
        if (err)
            *err = applyErr;
        return false;
    }

    QString commitErr;
    if (!txn.commit(&commitErr)) {
        if (err)
            *err = commitErr;
        return false;
    }

    return true;
}

QList<RowMutation> StagingBuffer::toMutations(const QHash<QString, QStringList>& pkColsPerTable,
                                              const QStringList& pkColsFallback) const {
    QList<RowMutation> mutations;
    mutations.reserve(staged_.size());
    for (const StagedRow& sr : staged_) {
        RowMutation rm;
        rm.table = sr.table;
        rm.columns = sr.row.keys();
        rm.values = sr.row.values();
        rm.pkColumns = pkColsPerTable.value(sr.table, pkColsFallback);
        rm.mode = UpsertMode::DoUpdate;
        mutations.append(rm);
    }
    return mutations;
}

QVariantMap StagingBuffer::getRow(const QString& table, const QString& pk) const {
    for (const StagedRow& sr : staged_) {
        if (sr.table == table && sr.pk == pk)
            return sr.row;
    }
    return {};
}

void StagingBuffer::discard() {
    staged_.clear();
}

bool StagingBuffer::isEmpty() const {
    return staged_.isEmpty();
}

}  // namespace dbridge::sync
