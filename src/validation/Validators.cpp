#include "Validators.h"

#include "dbridge/Errors.h"

#include <QDate>
#include <QRegularExpression>
#include <QSet>

namespace dbridge::detail {

ValidatorFn makeNotNull() {
    return [](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        *out = in;
        if (in.isNull() || in.toString().isEmpty()) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_VALIDATE_NULL);
            if (msg)
                *msg = QStringLiteral("Value is required (must not be null or empty)");
            return false;
        }
        return true;
    };
}

ValidatorFn makeLenLe(int n) {
    return [n](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        *out = in;
        if (in.isNull())
            return true;  // null passes length check (notNull handles null)
        QString s = in.toString();
        if (s.length() > n) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_VALIDATE_TYPE);
            if (msg)
                *msg = QStringLiteral("Length %1 exceeds maximum %2").arg(s.length()).arg(n);
            return false;
        }
        return true;
    };
}

ValidatorFn makeLenGe(int n) {
    return [n](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        *out = in;
        if (in.isNull())
            return true;
        QString s = in.toString();
        if (s.length() < n) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_VALIDATE_TYPE);
            if (msg)
                *msg = QStringLiteral("Length %1 is below minimum %2").arg(s.length()).arg(n);
            return false;
        }
        return true;
    };
}

ValidatorFn makeInt() {
    return [](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        if (in.isNull()) {
            *out = in;
            return true;
        }
        bool ok = false;
        qlonglong v = in.toString().toLongLong(&ok);
        if (!ok) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_VALIDATE_TYPE);
            if (msg)
                *msg = QStringLiteral("Value must be an integer: ") + in.toString();
            return false;
        }
        *out = QVariant(v);
        return true;
    };
}

ValidatorFn makeIntGe(long long n) {
    return [n](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        if (in.isNull()) {
            *out = in;
            return true;
        }
        bool ok = false;
        qlonglong v = in.toString().toLongLong(&ok);
        if (!ok) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_VALIDATE_TYPE);
            if (msg)
                *msg = QStringLiteral("Value must be an integer: ") + in.toString();
            return false;
        }
        if (v < n) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_VALIDATE_TYPE);
            if (msg)
                *msg = QStringLiteral("Value %1 is below minimum %2").arg(v).arg(n);
            return false;
        }
        *out = QVariant(v);
        return true;
    };
}

ValidatorFn makeDecimal() {
    return [](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        if (in.isNull()) {
            *out = in;
            return true;
        }
        bool ok = false;
        double v = in.toString().toDouble(&ok);
        if (!ok) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_VALIDATE_TYPE);
            if (msg)
                *msg = QStringLiteral("Value must be a decimal number: ") + in.toString();
            return false;
        }
        *out = QVariant(v);
        return true;
    };
}

ValidatorFn makeDate(const QString& fmt) {
    return [fmt](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        if (in.isNull()) {
            *out = in;
            return true;
        }
        // If already QDate or QDateTime, pass through
        if (in.type() == QVariant::Date || in.type() == QVariant::DateTime) {
            *out = in;
            return true;
        }
        QDate d = QDate::fromString(in.toString(), fmt);
        if (!d.isValid()) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_VALIDATE_TYPE);
            if (msg)
                *msg = QStringLiteral("Value '%1' does not match date format '%2'")
                           .arg(in.toString(), fmt);
            return false;
        }
        *out = QVariant(d);
        return true;
    };
}

ValidatorFn makeRegex(const QString& pattern, QString* err) {
    QRegularExpression re(QRegularExpression::anchoredPattern(pattern));
    if (!re.isValid()) {
        if (err)
            *err = QStringLiteral("Invalid regex pattern: ") + pattern;
        return {};
    }
    return [re](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        *out = in;
        if (in.isNull())
            return true;
        if (!re.match(in.toString()).hasMatch()) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_VALIDATE_REGEX);
            if (msg)
                *msg = QStringLiteral("Value does not match pattern: ") + in.toString();
            return false;
        }
        return true;
    };
}

ValidatorFn makeEnum(const QString& valuesStr) {
    QSet<QString> allowed;
    for (const QString& v : valuesStr.split(',')) {
        allowed.insert(v.trimmed());
    }
    return [allowed](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        *out = in;
        if (in.isNull())
            return true;
        if (!allowed.contains(in.toString())) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_VALIDATE_TYPE);
            if (msg)
                *msg = QStringLiteral("Value '%1' is not in allowed values").arg(in.toString());
            return false;
        }
        return true;
    };
}

ValidatorFn compileToken(const QString& token, QString* err) {
    if (token == QStringLiteral("notNull"))
        return makeNotNull();
    if (token == QStringLiteral("int"))
        return makeInt();
    if (token == QStringLiteral("decimal"))
        return makeDecimal();

    if (token.startsWith(QStringLiteral("len<="))) {
        bool ok = false;
        int n = token.mid(5).toInt(&ok);
        if (!ok) {
            if (err)
                *err = QStringLiteral("Invalid len<= token: ") + token;
            return {};
        }
        return makeLenLe(n);
    }
    if (token.startsWith(QStringLiteral("len>="))) {
        bool ok = false;
        int n = token.mid(5).toInt(&ok);
        if (!ok) {
            if (err)
                *err = QStringLiteral("Invalid len>= token: ") + token;
            return {};
        }
        return makeLenGe(n);
    }
    if (token.startsWith(QStringLiteral("int>="))) {
        bool ok = false;
        long long n = token.mid(5).toLongLong(&ok);
        if (!ok) {
            if (err)
                *err = QStringLiteral("Invalid int>= token: ") + token;
            return {};
        }
        return makeIntGe(n);
    }
    if (token.startsWith(QStringLiteral("date:"))) {
        QString fmt = token.mid(5);
        return makeDate(fmt);
    }
    if (token.startsWith(QStringLiteral("regex:"))) {
        QString pattern = token.mid(6);
        return makeRegex(pattern, err);
    }
    if (token.startsWith(QStringLiteral("enum:"))) {
        QString vals = token.mid(5);
        return makeEnum(vals);
    }

    if (err)
        *err = QStringLiteral("Unknown validator token: ") + token;
    return {};
}

}  // namespace dbridge::detail
