#pragma once
#include "dbridge/Export.h"
#include "dbridge/Types.h"

#include <QList>

#include <memory>

namespace dbridge {

class DataBridge;

struct TransferProgress {
    int percent = 0;
    qint64 rowsDone = 0;
    qint64 rowsTotal = -1;
};

enum class TransferState { Idle, Running, Stopping, Completed, Stopped, Failed };

class DBRIDGE_EXPORT IBatchTransfer {
   public:
    virtual ~IBatchTransfer() = default;

    // ① Non-blocking import start; resets import polling state on success
    virtual bool startImport(const ImportOptions& options, QString* err = nullptr) = 0;

    // ② Non-blocking export start
    virtual bool startExport(const ExportOptions& options, QString* err = nullptr) = 0;

    // ③–⑧ Polling getters (locked snapshots, thread-safe)
    virtual TransferProgress importProgress() const = 0;
    virtual QList<RowError> importErrors() const = 0;
    virtual ImportResult importResult() const = 0;
    virtual TransferProgress exportProgress() const = 0;
    virtual QList<RowError> exportErrors() const = 0;
    virtual ExportResult exportResult() const = 0;

    // C9 symmetric additions
    virtual bool stop(QString* err = nullptr) = 0;
    virtual TransferState importState() const = 0;
    virtual TransferState exportState() const = 0;
};

DBRIDGE_EXPORT std::unique_ptr<IBatchTransfer> createBatchTransfer(DataBridge& bridge);

}  // namespace dbridge
