#pragma once
#include <QSet>
#include <QStringList>

namespace dbridge::detail {

// Produce the final column sequence: listed headers first (in columnOrder), then unlisted
// headers from `natural` in their original relative order.
inline QStringList reorderHeaders(const QStringList& natural, const QStringList& columnOrder) {
    if (columnOrder.isEmpty())
        return natural;

    QSet<QString> listed = QSet<QString>::fromList(columnOrder);
    QStringList result = columnOrder;
    for (const QString& h : natural) {
        if (!listed.contains(h))
            result.append(h);
    }
    return result;
}

}  // namespace dbridge::detail
