// ============================================================================
// Validators.cpp — 内建校验器工厂的实现（每个 makeXxx 返回一个校验闭包）
// ============================================================================
//
// 本文件实现 Validators.h 声明的全部工厂与 compileToken() 分发器。
//
// 【贯穿全文件的两条通用约定，先读这里再看各函数】
//   1) null 放行约定：除 makeNotNull 外，所有校验器对「null 输入」一律放行（返回 true）。
//      原因——「必填」是一个独立维度，由 notNull 专门负责；其它校验器只在「有值」时
//      才检查值本身。这样配置 ["notNull","int"] 时，null 由 notNull 报 E_VALIDATE_NULL，
//      而不是被 int 误报为「不是整数」，错误归类更精确。
//   2) out 归一化约定：成功时总把结果写入 *out。纯校验器（notNull/len/regex/enum）原样回传
//      （*out = in）；类型归一化校验器（int/intGe/decimal/date）则把转换后的强类型值写入
//      *out（如文本 "42" → QVariant(qlonglong 42)），供链上下一环及最终写库使用。
//
// 【错误码映射一览】（详见各处与 Errors.h）
//   notNull            → E_VALIDATE_NULL
//   len<=/len>=/int/decimal/intGe/date/enum 的「不合规」→ E_VALIDATE_TYPE
//   regex 不匹配        → E_VALIDATE_REGEX
//   makeRegex 模式非法  → 不产生运行期错误码，而是「构造失败」（空函数 + *err，配置期错误）
// ============================================================================

#include "Validators.h"

#include "dbridge/Errors.h"

#include <QDate>
#include <QRegularExpression>
#include <QSet>

namespace dbridge::detail {

// makeNotNull —— 「必填」校验：值不得为 null 也不得为空串。
//   这是唯一会对 null 报错的校验器（其余一律放行 null，见文件头约定 1）。
//   失败码 E_VALIDATE_NULL；通过时原样回传（*out = in）。
ValidatorFn makeNotNull() {
    return [](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        *out = in;
        // 注意：空串 "" 也算「缺失」——QString 默认转换下空串与 null 都视为未填。
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

// makeLenLe —— 「长度上限」：字符串长度（字符数）不得超过 n。
//   纯校验器，不改类型（*out = in）。失败码 E_VALIDATE_TYPE。
ValidatorFn makeLenLe(int n) {
    // 捕获 n（按值）：每个 makeLenLe(n) 都生成一个绑定了自己上限的独立闭包。
    return [n](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        *out = in;
        if (in.isNull())
            return true;  // null passes length check (notNull handles null)
                          // 译：null 直接通过长度检查（是否必填由 notNull 专管，见文件头约定 1）
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

// makeLenGe —— 「长度下限」：字符串长度不得小于 n。与 makeLenLe 对称。
//   纯校验器（*out = in）；null 放行；失败码 E_VALIDATE_TYPE。
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

// makeInt —— 「整数」校验兼归一化：把文本解析为 qlonglong。
//   类型归一化校验器：成功时 *out 改写为 QVariant(qlonglong)，让后续环节与写库拿到强类型整数。
//   null 放行（原样回传）；无法解析为整数 → E_VALIDATE_TYPE。
ValidatorFn makeInt() {
    return [](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        if (in.isNull()) {
            *out = in;  // null 放行：保持原 null，不强转
            return true;
        }
        bool ok = false;
        // 经 toString 再 toLongLong：统一从「文本表示」解析，避免依赖 QVariant 内部原类型。
        qlonglong v = in.toString().toLongLong(&ok);
        if (!ok) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_VALIDATE_TYPE);
            if (msg)
                *msg = QStringLiteral("Value must be an integer: ") + in.toString();
            return false;
        }
        *out = QVariant(v);  // 归一化：把规整后的强类型整数交给下一环 / 写库
        return true;
    };
}

// makeIntGe —— 「整数且 >= n」：先解析为整数，再判定下限。
//   类型归一化校验器（成功 *out = qlonglong）；两道关卡都报 E_VALIDATE_TYPE。
ValidatorFn makeIntGe(long long n) {
    return [n](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        if (in.isNull()) {
            *out = in;
            return true;
        }
        bool ok = false;
        qlonglong v = in.toString().toLongLong(&ok);
        if (!ok) {  // 第一关：必须是整数
            if (errCode)
                *errCode = QString::fromLatin1(err::E_VALIDATE_TYPE);
            if (msg)
                *msg = QStringLiteral("Value must be an integer: ") + in.toString();
            return false;
        }
        if (v < n) {  // 第二关：必须不小于下限 n
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

// makeDecimal —— 「小数」校验兼归一化：把文本解析为 double。
//   类型归一化校验器（成功 *out = double）；null 放行；无法解析 → E_VALIDATE_TYPE。
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

// makeDate —— 「日期」校验兼归一化：按 fmt 把文本解析成 QDate。
//
//   【重要——这是「遗留路径」】当一列只写了 date:fmt 校验、却没有声明列级/profile 级
//   时间槽对象时，时间解析就由本校验器承担（见 Mapper.h / ProfileSpec.h 对「遗留」的说明）；
//   若声明了时间槽，则 date:* token 会被 Mapper::compileValidators 剥离，改由 tconv 层处理，
//   本校验器不会被建链——故这里与 TemporalConvert 互补、互不重叠。
//
//   类型归一化：成功时 *out 写入 QVariant(QDate)。null 放行；解析失败 → E_VALIDATE_TYPE。
ValidatorFn makeDate(const QString& fmt) {
    return [fmt](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        if (in.isNull()) {
            *out = in;
            return true;
        }
        // If already QDate or QDateTime, pass through
        // 译：若输入本身已是 QDate/QDateTime（Excel
        // 原生日期单元格），无需再按字符串解析，直接放行。
        if (in.type() == QVariant::Date || in.type() == QVariant::DateTime) {
            *out = in;
            return true;
        }
        QDate d = QDate::fromString(in.toString(), fmt);  // 按给定格式严格解析
        if (!d.isValid()) {
            if (errCode)
                *errCode = QString::fromLatin1(err::E_VALIDATE_TYPE);
            if (msg)
                *msg = QStringLiteral("Value '%1' does not match date format '%2'")
                           .arg(in.toString(), fmt);
            return false;
        }
        *out = QVariant(d);  // 归一化为 QDate
        return true;
    };
}

// makeRegex —— 「正则」校验：值必须整串匹配给定模式。
//
//   【两段式失败的关键区分】
//     · 构造期失败：模式串本身非法 → 返回「空函数」并写 *err。这是配置错误
//       （E_PROFILE_PARSE 级，由 compileToken/compile 向上传播），不是运行期数据错误。
//     · 运行期失败：值不匹配 → 返回 false 并填 E_VALIDATE_REGEX（运行期数据错误）。
//   anchoredPattern：把模式包成 ^(...)$ 形式，强制「整串匹配」而非「部分命中」。
//   编译好的 QRegularExpression 被闭包按值捕获，运行期复用、不重复编译（性能关键）。
ValidatorFn makeRegex(const QString& pattern, QString* err) {
    QRegularExpression re(QRegularExpression::anchoredPattern(pattern));
    if (!re.isValid()) {
        if (err)
            *err = QStringLiteral("Invalid regex pattern: ") + pattern;
        return {};  // 空函数对象：通知 compileToken 此 token 非法（配置期错误）
    }
    return [re](const QVariant& in, QVariant* out, QString* errCode, QString* msg) -> bool {
        *out = in;  // 纯校验器，不改值
        if (in.isNull())
            return true;  // null 放行
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

// makeEnum —— 「枚举」校验：值必须在「逗号分隔的允许集合」内。
//   构造期把 "a,b,c" 拆分、各项 trim 后存进 QSet（O(1) 查找）。运行期纯校验（不改值）。
//   null 放行；不在集合内 → E_VALIDATE_TYPE。
ValidatorFn makeEnum(const QString& valuesStr) {
    QSet<QString> allowed;
    for (const QString& v : valuesStr.split(',')) {
        allowed.insert(v.trimmed());  // 去掉各项首尾空白，容忍 "a, b, c" 这种写法
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

// compileToken —— token 字符串 → ValidatorFn 的总分发器。
//
//   做什么：依次比对「字面量 token」（notNull/int/decimal）与「前缀 token」
//           （len<=/len>=/int>=/date:/regex:/enum:），命中即调对应工厂；带参数的
//           前缀 token 还要解析参数（如 len<=32 取 "32"），参数非法则报错返回空函数。
//   参数：token=单个校验规则文本；err=出参，token 未知或参数非法时填诊断。
//   返回：ValidatorFn；失败返回空函数（并已写 *err）。
//   错误模式：空返回会让 ValidatorChain::compile 失败，上游归为 E_PROFILE_PARSE。
//   注意：匹配顺序——「精确字面量」在前、「前缀」在后，避免误判（如 "int" vs "int>=")。
ValidatorFn compileToken(const QString& token, QString* err) {
    // ── 无参字面量 token ──────────────────────────────────────────────────────
    if (token == QStringLiteral("notNull"))
        return makeNotNull();
    if (token == QStringLiteral("int"))
        return makeInt();
    if (token == QStringLiteral("decimal"))
        return makeDecimal();

    // ── 带参前缀 token：先取前缀后的子串作参数，再解析、校验、构造 ────────────────
    if (token.startsWith(QStringLiteral("len<="))) {
        bool ok = false;
        int n = token.mid(5).toInt(&ok);  // mid(5)：跳过 "len<=" 5 个字符取数字部分
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
        long long n = token.mid(5).toLongLong(&ok);  // 用 long long 容纳大下限值
        if (!ok) {
            if (err)
                *err = QStringLiteral("Invalid int>= token: ") + token;
            return {};
        }
        return makeIntGe(n);
    }
    if (token.startsWith(QStringLiteral("date:"))) {
        QString fmt = token.mid(5);  // "date:" 之后整段都是 Qt 日期格式串
        return makeDate(fmt);
    }
    if (token.startsWith(QStringLiteral("regex:"))) {
        QString pattern = token.mid(6);  // "regex:" 之后整段都是正则模式
        return makeRegex(pattern, err);  // 模式非法时由 makeRegex 写 *err 并返回空函数
    }
    if (token.startsWith(QStringLiteral("enum:"))) {
        QString vals = token.mid(5);  // "enum:" 之后整段是逗号分隔的允许值
        return makeEnum(vals);
    }

    // ── 全不匹配 → 未知 token：配置错误 ──────────────────────────────────────────
    if (err)
        *err = QStringLiteral("Unknown validator token: ") + token;
    return {};
}

}  // namespace dbridge::detail
