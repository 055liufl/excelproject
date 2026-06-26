#pragma once
#include "dbridge/DataBridge.h"
#include "dbridge/IBatchTransfer.h"

#include <QFuture>
#include <QMutex>
#include <QSqlDatabase>
#include <QtConcurrent/QtConcurrent>

#include "profile/ProfileSpec.h"
#include "schema/SchemaCatalog.h"
#include <atomic>
#include <memory>

namespace dbridge {

class BatchTransfer : public IBatchTransfer {
   public:
    explicit BatchTransfer(DataBridge& bridge);
    ~BatchTransfer() override;

    bool startImport(const ImportOptions& options, QString* err = nullptr) override;
    bool startExport(const ExportOptions& options, QString* err = nullptr) override;

    TransferProgress importProgress() const override;
    QList<RowError> importErrors() const override;
    ImportResult importResult() const override;
    TransferProgress exportProgress() const override;
    QList<RowError> exportErrors() const override;
    ExportResult exportResult() const override;

    bool stop(QString* err = nullptr) override;
    TransferState importState() const override;
    TransferState exportState() const override;

   private:
    void runImport(const ImportOptions& opts, const QString& dbPath,
                   const detail::ProfileSpec& profile, const detail::SchemaCatalog& catalog);
    void runExport(const ExportOptions& opts, const QString& dbPath,
                   const detail::ProfileSpec& profile, const detail::SchemaCatalog& catalog);

    DataBridge& bridge_;

    mutable QMutex mutex_;

    TransferState importState_ = TransferState::Idle;
    TransferState exportState_ = TransferState::Idle;
    TransferProgress importProgress_;
    TransferProgress exportProgress_;
    QList<RowError> importErrors_;
    QList<RowError> exportErrors_;
    ImportResult importResult_;
    ExportResult exportResult_;
    QFuture<void> importFuture_;
    QFuture<void> exportFuture_;

    // Separate stop flags so import and export can be stopped independently.
    std::atomic<bool> importStopRequested_{false};
    std::atomic<bool> exportStopRequested_{false};
};

}  // namespace dbridge
