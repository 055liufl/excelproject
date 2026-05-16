#pragma once
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <memory>

namespace dbridge::detail {

class ExcelWriter {
   public:
    ExcelWriter();
    ~ExcelWriter();

    bool open(const QString& xlsxPath, const QString& sheetName, QString* err);
    void writeHeader(const QStringList& headers);
    void writeRow(const QVector<QVariant>& row);
    bool save(QString* err);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    QString path_;
    int rowCursor_ = 1;
};

}  // namespace dbridge::detail
