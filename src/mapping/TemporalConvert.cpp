// ============================================================================
// TemporalConvert.cpp — tconv 时间转换工具集的实现
// ============================================================================
//
// 本文件实现 TemporalConvert.h 声明的 5 个自由函数：
//   isEmptyForTemporal / isStructuredTemporal —— 判空与种类识别（决定走哪条路径）。
//   parseString    —— 主格式 + fallback 逐个尝试解析字符串。
//   formatValue    —— 结构化时间 → 目标侧物理表示（字符串 或 纪元秒）。
//   toStructured   —— 来源侧原始值 → 结构化时间（字符串走 parseString，纪元秒走纪元换算）。
//
// 关键不直观处（纪元秒边界、数值 0 不算空、QTime 的 0 值序列化）见各处行内注释。
// ============================================================================

#include "TemporalConvert.h"

#include "dbridge/Errors.h"

#include <QDate>
#include <QDateTime>
#include <QTime>

namespace dbridge::detail::tconv {

// isEmptyForTemporal —— 见头文件。要点：null/无效 → 空；空白字符串 → 空；但数值（含 0）不空。
bool isEmptyForTemporal(const QVariant& v) {
    if (v.isNull() || !v.isValid())
        return true;  // 无值
    if (v.type() == QVariant::String)
        return v.toString().trimmed().isEmpty();  // 纯空白文本视作无时间值
    // 非字符串（如数值纪元秒、已结构化时间）一律不算空——尤其纪元秒 0 是合法时间，绝不能误判。
    return false;
}

// isStructuredTemporal —— 见头文件。按 kind 精确匹配 QVariant 的底层类型。
bool isStructuredTemporal(const QVariant& v, TemporalSlotKind kind) {
    switch (kind) {
        case TemporalSlotKind::Date:
            return v.type() == QVariant::Date;
        case TemporalSlotKind::DateTime:
            return v.type() == QVariant::DateTime;
        case TemporalSlotKind::Time:
            return v.type() == QVariant::Time;
        case TemporalSlotKind::None:
            return false;  // 非时间列：无所谓「结构化」
    }
    return false;  // 防御：枚举意外取值
}

// parseString —— 把字符串按「主格式优先、fallback 兜底」逐个尝试解析。详见头文件。
ParseResult parseString(const QString& s, TemporalSlotKind kind, const QString& primary,
                        const QStringList& fallback) {
    // 组装待尝试格式列表：主格式（若非空）排首位，随后接所有 fallback。
    QStringList formats;
    if (!primary.isEmpty())
        formats.append(primary);
    formats.append(fallback);

    // 逐个格式尝试，命中第一个有效解析即返回（短路）。
    for (const QString& fmt : formats) {
        if (fmt.isEmpty())
            continue;  // 跳过空格式串（fallback 里可能混入空项）
        switch (kind) {
            case TemporalSlotKind::Date: {
                QDate d = QDate::fromString(s, fmt);
                if (d.isValid())
                    return {true, QVariant(d), fmt};  // 记下命中的格式串 fmt，便于诊断
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
                break;  // 非时间列：无格式可解析
        }
    }
    return {false, QVariant(), QString()};  // 所有格式都失败
}

// formatValue —— 结构化时间 → 目标侧物理表示。详见头文件。
QVariant formatValue(const QVariant& structured, TemporalSlotKind kind,
                     const TemporalSideSpec& side) {
    // ── 纪元秒分支（仅 DateTime 槽合法）────────────────────────────────────────
    if (side.type == TemporalPhysType::EpochSec) {
        // 仅当槽是 DateTime 且值确为 QDateTime 时，才能换算为「自纪元起的秒数」。
        // 其它种类（Date/Time）没有完整的「绝对时刻」语义，无法换算 → 返回无效值。
        if (kind == TemporalSlotKind::DateTime && structured.type() == QVariant::DateTime) {
            return QVariant(static_cast<qlonglong>(structured.toDateTime().toSecsSinceEpoch()));
        }
        return QVariant();  // 种类/类型不符 → 无效（上层退化为写 NULL）
    }
    // ── 字符串分支（type=string）：按 side.format 格式化 ─────────────────────────
    switch (kind) {
        case TemporalSlotKind::Date:
            if (structured.type() == QVariant::Date) {
                QString s = structured.toDate().toString(side.format);
                // 格式串非法 / 与值不兼容时 toString 可能返回空串 → 视为不可序列化，退化为无效值。
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
    return QVariant();  // 类型与种类不匹配 → 无效
}

// toStructured —— 来源侧原始值 → 结构化时间。详见头文件。失败置 errCode=E_TIME_PARSE。
QVariant toStructured(const QVariant& raw, TemporalSlotKind kind, const TemporalSideSpec& side,
                      QString* errCode, QString* errMsg) {
    // ── 纪元秒分支：raw 必须能解析为整数秒 ──────────────────────────────────────
    if (side.type == TemporalPhysType::EpochSec) {
        // epochSec: raw must be numeric (qlonglong or convertible integer)
        // 译：纪元秒模式下，raw 必须是数值（qlonglong，或可转为整数的值）。
        bool ok = false;
        qlonglong secs = raw.toLongLong(&ok);
        if (!ok) {
            // 非整数（如文本含非数字字符）→ 解析失败。
            if (errCode)
                *errCode = QString::fromLatin1(err::E_TIME_PARSE);
            if (errMsg)
                *errMsg = QStringLiteral("Cannot parse epoch value '") + raw.toString() +
                          QStringLiteral("' as integer seconds");
            return QVariant();
        }
        QDateTime dt = QDateTime::fromSecsSinceEpoch(secs);
        if (!dt.isValid()) {
            // 秒数超出 QDateTime 可表示范围（极端大/小值）→ 视为解析失败。
            if (errCode)
                *errCode = QString::fromLatin1(err::E_TIME_PARSE);
            if (errMsg)
                *errMsg = QStringLiteral("Epoch value ") + QString::number(secs) +
                          QStringLiteral(" is out of representable range");
            return QVariant();
        }
        return QVariant(dt);  // 成功：得到结构化 QDateTime
    }
    // ── 字符串分支：转调 parseString（主格式 + fallback）────────────────────────
    auto res = parseString(raw.toString(), kind, side.format, side.fallback);
    if (!res.ok) {
        // 所有格式都解析失败 → E_TIME_PARSE。
        if (errCode)
            *errCode = QString::fromLatin1(err::E_TIME_PARSE);
        if (errMsg)
            *errMsg = QStringLiteral("Cannot parse temporal value '") + raw.toString() +
                      QStringLiteral("'");
        return QVariant();
    }
    return res.value;  // 成功：parseString 已给出对应种类的结构化时间
}

}  // namespace dbridge::detail::tconv
