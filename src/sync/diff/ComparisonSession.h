#pragma once
#include "dbridge/sync/IComparisonSession.h"

#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>

#include "../SyncContext.h"
#include "../apply/UpsertExecutor.h"
#include "../schema/TableStateStore.h"
#include "DiffEngine.h"
#include "InboundTableGate.h"
#include "StagingBuffer.h"

namespace dbridge::sync {

struct RemoteTableData {
    DiffEngine::RemoteMeta meta;
    QList<QVariantMap> rows;
};

class ComparisonSession : public IComparisonSession {
   public:
    ComparisonSession(QSqlDatabase& rconn, QSqlDatabase& wconn, TableStateStore& ts,
                      DiffEngine& diff, InboundTableGate& gate, UpsertExecutor& upsert,
                      qint64 streamEpoch, std::shared_ptr<SyncContext> context = nullptr);

    // Initialize: compute diffs against remote, open gate.
    // remoteMetas: table->RemoteMeta. remoteRows: table->rows.
    bool initialize(const QStringList& tables,
                    const QHash<QString, DiffEngine::RemoteMeta>& remoteMetas,
                    const QHash<QString, QList<QVariantMap>>& remoteRows, QString* err);

    // IComparisonSession implementation
    QList<TableDiff> tableDiffs() const override;
    QList<RowDiff> rowDiffs(const QString& table, int offset, int limit) const override;
    bool stageRow(const QString& table, const QString& pk) override;
    bool stageTable(const QString& table) override;
    bool unstage(const QString& table, const QString& pk) override;
    bool acceptLocal(const QString& table, const QString& pk) override;
    bool acceptRemote(const QString& table, const QString& pk) override;
    bool stageCell(const QString& table, const QString& pk, const QString& column,
                   const QVariant& value) override;
    QList<RowDiff> fetchRemoteRows(const QString& table, const QString& keysetPageToken,
                                   int pageSize, const QString& snapshotId) const override;
    bool save(QString* err = nullptr) override;
    void discard() override;

   private:
    bool checkStale(QString* err);
    qint64 readDataVersion(QString* err) const;
    QVariantMap findRemoteRow(const QString& table, const QString& pk) const;
    QVariantMap findLocalRow(const QString& table, const QString& pk) const;
    QString getPkColumn(const QString& table) const;

    QSqlDatabase& rconn_;
    QSqlDatabase& wconn_;
    TableStateStore& ts_;
    DiffEngine& diff_;
    InboundTableGate& gate_;
    UpsertExecutor& upsert_;
    StagingBuffer staging_;
    qint64 streamEpoch_;
    std::shared_ptr<SyncContext> context_;
    qint64 pinnedDataVersion_ = 0;
    QList<TableDiff> diffs_;
    QHash<QString, RemoteTableData> remoteData_;
    mutable QHash<QString, QString> pkColCache_;
};

}  // namespace dbridge::sync
