#pragma once
#include <QString>
#include <QStringList>
#include <QVariant>

#include "profile/ProfileSpec.h"

namespace dbridge::detail::tconv {

// True if the value should be treated as "no temporal value" — null QVariant
// or an empty/whitespace-only string. Numeric zero is NOT empty.
bool isEmptyForTemporal(const QVariant& v);

// True if the value is already a structured QDate / QDateTime / QTime,
// matching the kind requested. None matches no structured value.
bool isStructuredTemporal(const QVariant& v, TemporalSlotKind kind);

struct ParseResult {
    bool ok = false;
    QVariant value;       // QDate / QDateTime / QTime on success
    QString triedFormat;  // format string that succeeded, if any
};

// Parse a string into the requested kind using `primary`; on failure, try each
// `fallback` in order. Returns the first successful parse. Empty `primary` →
// the function still tries fallbacks; if both are empty the result is ok=false.
ParseResult parseString(const QString& s, TemporalSlotKind kind, const QString& primary,
                        const QStringList& fallback);

// Format a structured temporal QVariant to a target side value.
// type=string: returns QVariant(QString) using Qt format string.
// type=epochSec: returns QVariant(qlonglong) from QDateTime::toSecsSinceEpoch().
// Failure or wrong kind: returns QVariant() (invalid).
QVariant formatValue(const QVariant& structured, TemporalSlotKind kind,
                     const TemporalSideSpec& side);

// Parse raw value (from source side) to structured QDate/QDateTime/QTime.
// type=string: calls parseString; type=epochSec: calls QDateTime::fromSecsSinceEpoch.
// On failure sets errCode/errMsg and returns QVariant() (invalid).
QVariant toStructured(const QVariant& raw, TemporalSlotKind kind, const TemporalSideSpec& side,
                      QString* errCode, QString* errMsg);

}  // namespace dbridge::detail::tconv
