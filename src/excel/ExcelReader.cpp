#include "ExcelReader.h"

#include <memory>
#include <xlsxdocument.h>

namespace dbridge::detail {

struct ExcelReader::Impl {
    std::unique_ptr<QXlsx::Document> doc;
    QString sheet;
};

ExcelReader::ExcelReader() : impl_(std::make_unique<Impl>()) {
}
ExcelReader::~ExcelReader() = default;

bool ExcelReader::open(const QString& xlsxPath, QString* err) {
    impl_->doc = std::make_unique<QXlsx::Document>(xlsxPath);
    if (!impl_->doc->load(xlsxPath)) {
        if (err)
            *err = QStringLiteral("Failed to open xlsx: ") + xlsxPath;
        return false;
    }
    return true;
}

bool ExcelReader::selectSheet(const QString& name, QString* err) {
    if (!impl_->doc->selectSheet(name)) {
        if (err)
            *err = QStringLiteral("Sheet not found: ") + name;
        return false;
    }
    impl_->sheet = name;
    return true;
}

bool ExcelReader::readHeader(int headerRow, QString* err) {
    headerRow_ = headerRow;
    headers_.clear();
    sourceToCol_.clear();

    // Find max columns by scanning the header row
    // In stub we go up to a reasonable max; real QXlsx would give dimension
    int maxCol = impl_->doc->dimension_colMax();
    if (maxCol == 0)
        maxCol = 256;  // fallback

    for (int col = 1; col <= maxCol; ++col) {
        QVariant v = impl_->doc->read(headerRow, col);
        if (!v.isValid() || v.isNull())
            break;
        QString hdr = v.toString().trimmed();
        if (hdr.isEmpty())
            break;
        headers_.append(hdr);
        sourceToCol_[hdr] = col;
    }

    if (headers_.isEmpty()) {
        if (err)
            *err = QStringLiteral("No headers found in row ") + QString::number(headerRow);
        return false;
    }

    lastRow_ = impl_->doc->dimension_rowMax();
    if (lastRow_ < headerRow_)
        lastRow_ = headerRow_;

    return true;
}

QVariant ExcelReader::cellBySource(int row, const QString& source) const {
    auto it = sourceToCol_.find(source);
    if (it == sourceToCol_.end())
        return QVariant();
    QVariant v = impl_->doc->read(row, it.value());
    return v;
}

int ExcelReader::columnOfSource(const QString& source) const {
    return sourceToCol_.value(source, -1);
}

}  // namespace dbridge::detail
