// xlsxcelllocation.cpp

#include "xlsxcelllocation.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QVector>

#include "xlsxcell.h"
#include "xlsxglobal.h"

QT_BEGIN_NAMESPACE_XLSX

CellLocation::CellLocation() {
    col = -1;
    row = -1;

    cell.reset();
}

QT_END_NAMESPACE_XLSX
