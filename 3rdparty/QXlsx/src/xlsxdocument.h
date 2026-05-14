#pragma once
// Minimal QXlsx stub for compilation.
// Replace with the real QXlsx library (https://github.com/QtExcelSoftware/QtXlsxWriter)
// for production use. This stub compiles but xlsx I/O is no-op.
#include <QHash>
#include <QString>
#include <QVariant>

namespace QXlsx {

class Document {
   public:
    explicit Document(const QString& xlsxPath = QString(), QObject* parent = nullptr)
        : path_(xlsxPath), valid_(false), rowCount_(0), colCount_(0) {
        Q_UNUSED(parent)
        if (!xlsxPath.isEmpty()) {
            // stub: pretend open succeeded for write mode
            valid_ = true;
        }
    }
    ~Document() = default;

    bool isValid() const {
        return valid_;
    }

    // Open an existing file (read mode)
    bool load(const QString& xlsxPath) {
        path_ = xlsxPath;
        valid_ = true;  // stub always succeeds
        return true;
    }

    // Write to file
    bool save() {
        return true;
    }
    bool saveAs(const QString& xlsxPath) {
        Q_UNUSED(xlsxPath)
        return true;
    }

    // Sheet operations
    QStringList sheetNames() const {
        return sheets_.keys();
    }

    bool selectSheet(const QString& name) {
        currentSheet_ = name;
        if (!sheets_.contains(name)) {
            sheets_[name] = {};
        }
        return true;
    }

    QString currentSheet() const {
        return currentSheet_;
    }

    // Cell access (1-based row and column)
    QVariant read(int row, int col) const {
        if (!sheets_.contains(currentSheet_))
            return QVariant();
        const auto& sheet = sheets_[currentSheet_];
        auto it = sheet.find(cellKey(row, col));
        return it != sheet.end() ? it.value() : QVariant();
    }

    bool write(int row, int col, const QVariant& value) {
        sheets_[currentSheet_][cellKey(row, col)] = value;
        if (row > rowCount_)
            rowCount_ = row;
        if (col > colCount_)
            colCount_ = col;
        return true;
    }

    // Dimension helpers
    int dimension_rowMax() const {
        return rowCount_;
    }
    int dimension_colMax() const {
        return colCount_;
    }

    // Expose cell data for internal iteration (stub uses QHash)
    void setData(const QString& sheet, const QHash<QString, QVariant>& data, int rows, int cols) {
        sheets_[sheet] = data;
        rowCount_ = rows;
        colCount_ = cols;
        currentSheet_ = sheet;
        valid_ = true;
    }

   private:
    static QString cellKey(int r, int c) {
        return QString::number(r) + QLatin1Char(',') + QString::number(c);
    }

    QString path_;
    bool valid_ = false;
    QString currentSheet_;
    QHash<QString, QHash<QString, QVariant>> sheets_;
    int rowCount_ = 0;
    int colCount_ = 0;
};

}  // namespace QXlsx
