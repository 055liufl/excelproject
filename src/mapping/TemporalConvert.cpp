#include "TemporalConvert.h"

#include "dbridge/Errors.h"

#include <QDate>
#include <QDateTime>
#include <QTime>

namespace dbridge::detail::tconv {

bool isEmptyForTemporal(const QVariant& v) {
    if (v.isNull() || !v.isValid())
        return true;
    if (v.type() == QVariant::String)
        return v.toString().trimmed().isEmpty();
    return false;
}

bool isStructuredTemporal(const QVariant& v, TemporalSlotKind kind) {
    switch (kind) {
        case TemporalSlotKind::Date:
            return v.type() == QVariant::Date;
        case TemporalSlotKind::DateTime:
            return v.type() == QVariant::DateTime;
        case TemporalSlotKind::Time:
            return v.type() == QVariant::Time;
        case TemporalSlotKind::None:
            return false;
    }
    return false;
}

ParseResult parseString(const QString& s, TemporalSlotKind kind, const QString& primary,
                        const QStringList& fallback) {
    QStringList formats;
    if (!primary.isEmpty())
        formats.append(primary);
    formats.append(fallback);

    for (const QString& fmt : formats) {
        if (fmt.isEmpty())
            continue;
        switch (kind) {
            case TemporalSlotKind::Date: {
                QDate d = QDate::fromString(s, fmt);
                if (d.isValid())
                    return {true, QVariant(d), fmt};
                break;
            }
            case TemporalSlotKind::DateTime: {
                QDateTime dt = QDateTime::fromString(s, fmt);
                if (dt.isValid())
                    return {true, QVariant(dt), fmt};
                break;
            }
            case TemporalSlotKind::Time: {
                QTime t = QTime::fromString(s, fmt);
                if (t.isValid())
                    return {true, QVariant(t), fmt};
                break;
            }
            case TemporalSlotKind::None:
                break;
        }
    }
    return {false, QVariant(), QString()};
}

QVariant formatValue(const QVariant& structured, TemporalSlotKind kind,
                     const TemporalSideSpec& side) {
    if (side.type == TemporalPhysType::EpochSec) {
        if (kind == TemporalSlotKind::DateTime && structured.type() == QVariant::DateTime) {
            return QVariant(static_cast<qlonglong>(structured.toDateTime().toSecsSinceEpoch()));
        }
        return QVariant();
    }
    // type=string
    switch (kind) {
        case TemporalSlotKind::Date:
            if (structured.type() == QVariant::Date) {
                QString s = structured.toDate().toString(side.format);
                return s.isEmpty() ? QVariant() : QVariant(s);
            }
            break;
        case TemporalSlotKind::DateTime:
            if (structured.type() == QVariant::DateTime) {
                QString s = structured.toDateTime().toString(side.format);
                return s.isEmpty() ? QVariant() : QVariant(s);
            }
            break;
        case TemporalSlotKind::Time:
            if (structured.type() == QVariant::Time) {
                QString s = structured.toTime().toString(side.format);
                return s.isEmpty() ? QVariant() : QVariant(s);
            }
            break;
        case TemporalSlotKind::None:
            break;
    }
    return QVariant();
}

QVariant toStructured(const QVariant& raw, TemporalSlotKind kind, const TemporalSideSpec& side,
                      QString* errCode, QString* errMsg) {
    if (side.type == TemporalPhysType::EpochSec) {
        // epochSec: raw must be numeric (qlonglong or convertible integer)
        bool ok = false;
        qlonglong secs = raw.toLongLong(&ok);
        if (!ok) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_TIME_PARSE);
            if (errMsg)
                *errMsg = QStringLiteral("Cannot parse epoch value '") + raw.toString() +
                          QStringLiteral("' as integer seconds");
            return QVariant();
        }
        QDateTime dt = QDateTime::fromSecsSinceEpoch(secs);
        if (!dt.isValid()) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_TIME_PARSE);
            if (errMsg)
                *errMsg = QStringLiteral("Epoch value ") + QString::number(secs) +
                          QStringLiteral(" is out of representable range");
            return QVariant();
        }
        return QVariant(dt);
    }
    // type=string
    auto res = parseString(raw.toString(), kind, side.format, side.fallback);
    if (!res.ok) {
        if (errCode)
            *errCode = QString::fromLatin1(err::E_TIME_PARSE);
        if (errMsg)
            *errMsg = QStringLiteral("Cannot parse temporal value '") + raw.toString() +
                      QStringLiteral("'");
        return QVariant();
    }
    return res.value;
}

}  // namespace dbridge::detail::tconv
