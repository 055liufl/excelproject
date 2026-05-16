#include "ExcelWriter.h"

#include <memory>
#include <xlsxdocument.h>

namespace dbridge::detail {

struct ExcelWriter::Impl {
    std::unique_ptr<QXlsx::Document> doc;
    QString sheet;
};

ExcelWriter::ExcelWriter() : impl_(std::make_unique<Impl>()) {
}
ExcelWriter::~ExcelWriter() = default;

bool ExcelWriter::open(const QString& xlsxPath, const QString& sheetName, QString* err) {
    path_ = xlsxPath;
    rowCursor_ = 1;
    impl_->doc = std::make_unique<QXlsx::Document>();  // empty doc; saveAs writes path_
    impl_->sheet = sheetName;
    // Fresh QXlsx::Document already contains "Sheet1"; rename it if user wants
    // a different sheet name, otherwise addSheet+selectSheet.
    QStringList names = impl_->doc->sheetNames();
    if (!names.contains(sheetName)) {
        if (!impl_->doc->addSheet(sheetName)) {
            if (err)
                *err = QStringLiteral("Failed to add sheet: ") + sheetName;
            return false;
        }
    }
    if (!impl_->doc->selectSheet(sheetName)) {
        if (err)
            *err = QStringLiteral("Failed to select sheet: ") + sheetName;
        return false;
    }
    return true;
}

void ExcelWriter::writeHeader(const QStringList& headers) {
    for (int i = 0; i < headers.size(); ++i) {
        impl_->doc->write(rowCursor_, i + 1, headers[i]);
    }
    ++rowCursor_;
}

void ExcelWriter::writeRow(const QVector<QVariant>& row) {
    for (int i = 0; i < row.size(); ++i) {
        if (!row[i].isNull()) {
            impl_->doc->write(rowCursor_, i + 1, row[i]);
        }
    }
    ++rowCursor_;
}

bool ExcelWriter::save(QString* err) {
    if (!impl_->doc->saveAs(path_)) {
        if (err)
            *err = QStringLiteral("Failed to save xlsx: ") + path_;
        return false;
    }
    return true;
}

}  // namespace dbridge::detail
