#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <memory>

// Forward declare to avoid pulling in QXlsx headers in public-facing code
namespace QXlsx {
class Document;
}

namespace dbridge::detail {

class ExcelReader {
   public:
    ExcelReader();
    ~ExcelReader();

    bool open(const QString& xlsxPath, QString* err);
    bool selectSheet(const QString& name, QString* err);
    bool readHeader(int headerRow, QString* err);

    int firstDataRow() const {
        return headerRow_ + 1;
    }
    int lastRow() const {
        return lastRow_;
    }

    QVariant cellBySource(int row, const QString& source) const;
    QStringList headers() const {
        return headers_;
    }
    int columnOfSource(const QString& source) const;
    bool hasSource(const QString& source) const {
        return sourceToCol_.contains(source);
    }

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int headerRow_ = 1;
    int lastRow_ = 0;
    QStringList headers_;
    QHash<QString, int> sourceToCol_;  // source -> 1-based column index
};

}  // namespace dbridge::detail
