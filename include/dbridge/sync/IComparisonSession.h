#pragma once
#include "dbridge/Export.h"
#include "dbridge/sync/SyncConfig.h"
#include "dbridge/sync/SyncTypes.h"

#include <QList>
#include <QVariant>

#include <memory>

namespace dbridge::sync {

enum class TableDiffStatus { Identical, Different, OnlyLocal, OnlyRemote };
enum class RowDiffKind { Same, Added, Deleted, Modified };

struct CellDiff {
    QString column;
    QVariant localValue;
    QVariant remoteValue;
    bool changed = false;
};

struct RowDiff {
    RowDiffKind kind = RowDiffKind::Same;
    QString primaryKey;
    QList<CellDiff> cells;
};

struct TableDiff {
    QString table;
    TableDiffStatus status = TableDiffStatus::Identical;
    int addedRows = 0;
    int deletedRows = 0;
    int modifiedRows = 0;
};

class DBRIDGE_EXPORT IComparisonSession {
   public:
    virtual ~IComparisonSession() = default;

    virtual QList<TableDiff> tableDiffs() const = 0;
    virtual QList<RowDiff> rowDiffs(const QString& table, int offset = 0, int limit = -1) const = 0;
    virtual bool stageRow(const QString& table, const QString& pk) = 0;
    virtual bool stageTable(const QString& table) = 0;
    virtual bool unstage(const QString& table, const QString& pk) = 0;
    virtual bool acceptLocal(const QString& table, const QString& pk) = 0;
    virtual bool acceptRemote(const QString& table, const QString& pk) = 0;
    virtual bool stageCell(const QString& table, const QString& pk, const QString& column,
                           const QVariant& value) = 0;
    virtual QList<RowDiff> fetchRemoteRows(const QString& table, const QString& keysetPageToken,
                                           int pageSize, const QString& snapshotId) const = 0;
    virtual bool save(QString* err = nullptr) = 0;
    virtual void discard() = 0;
};

DBRIDGE_EXPORT std::unique_ptr<IComparisonSession> createComparisonSession(const SyncConfig& config,
                                                                           QString* err = nullptr);

}  // namespace dbridge::sync
