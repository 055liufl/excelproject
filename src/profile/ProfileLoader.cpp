#include "ProfileLoader.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>

// ============================================================================
// ProfileLoader.cpp — Profile JSON → ProfileSpec 解析器的实现
// ============================================================================
//
// 【本文件职责】
//   实现 ProfileLoader（声明见 ProfileLoader.h）：把一份 Profile 的 JSON 配置逐字段
//   读出、做合法性校验、补默认值、并把「时间格式」的多种写法归一化，最终填进 ProfileSpec
//   内存模型（见 ProfileSpec.h）。这是 ETL 配置链路「JSON→内存模型」的唯一实现处。
//
// 【在 ETL 管线中的位置】
//   JSON ─Qt 解析→ QJsonDocument ─[本文件 load()]→ ProfileSpec ─[ProfileValidator]→
//   与库 schema 比对 ─→ 导入/导出引擎执行。本文件只校验「配置本身」是否结构正确、
//   取值合法；不连数据库（表/列是否真实存在留给 ProfileValidator）。
//
// 【文件内部组织（自上而下）】
//   1) 匿名命名空间里的「时间格式」解析助手（仅本文件可见）：
//        · slotFormatTokensOk —— 校验 Qt 日期格式串里有没有与槽种类冲突的 token。
//        · slotKindName       —— 槽种类 → JSON 字段名（拼错误信息用）。
//        · parseTemporalSide  —— 解析时间槽「单侧」子对象 {type,format,fallback}。
//        · readTemporalSlot   —— 解析整个时间槽（兼容旧式扁平字段与新式 excel/db 子对象）。
//   2) 文件级静态助手：标识符 / 表.列 形态校验（isSimpleIdentifier / isTableDotColumn）。
//   3) ProfileLoader 成员函数：validateToken → readColumn → readRoute →
//        readSingleTable / readMultiTable / readMixed → load（总入口与后置校验）。
//
// 【贯穿全文件的几条 JSON 解析约定（读注释前先记住，后面不再每处重复）】
//   · QJsonValue 的三态：isUndefined()=该键不存在；isNull()=键存在但值为 JSON null；
//     其余=有实际值。很多字段对这三态有不同处理（缺省→用默认；null→常视为配置错误）。
//   · QJsonObject::value(key) 对不存在的键返回「Undefined」QJsonValue；
//     .toObject()/.toArray()/.toString()/.toInt(默认值) 对类型不符会安静地返回空/默认，
//     因此本文件常先用 isObject()/isArray() 显式判类型，避免「类型写错却被静默忽略」。
//   · 错误回报：失败时若 err 非空就写入一条可读说明并 return false（短路）。这些说明文本
//     是给人看的；对外的机器可读码（E_PROFILE_PARSE 等，见 Errors.h）由上层据语境贴。
//
// 【fix 标记说明】文中 H-0x / M-0x / 2.x 等标记是历史缺陷修复/规格条款编号，逐一保留。
// ============================================================================

namespace dbridge::detail {

// 匿名命名空间：以下助手只在本翻译单元内可见（无外部链接），不污染全局符号表。
namespace {

// slotFormatTokensOk —— 校验一段 Qt 日期/时间格式串是否含有「与本槽种类冲突」的格式 token。
//
// 【翻译保留原注释】add-time-format-profile：按槽（slot）逐一做 token 校验。
//   Date 槽拒绝时间类 token；Time 槽拒绝日期类 token；DateTime 不做限制。
//   token 列表逐字摘自规格文档 "Format token validation per slot type"。
//
// 【为什么需要】Qt 的日期格式串用 yyyy/MM/dd（日期）、HH/mm/ss（时间）等占位符。若用户在一个
//   「纯日期」槽里写了 HH:mm，说明配置语义自相矛盾（日期列里却含时间分量），应尽早在加载期拒绝，
//   而不是等到运行时解析出莫名其妙的值。
// 【参数】kind=槽种类（Date/DateTime/Time/None）；fmt=待校验的格式串；
//   offendingToken=可选出参，命中违规 token 时写回是哪一个（供拼错误信息）。
// 【返回】true=未发现冲突 token（含 DateTime/None 不限制、空串直接放行）；false=发现冲突。
// 【副作用】仅可能写 *offendingToken。【复杂度】O(fmt 长度 × 禁用 token 数)。
bool slotFormatTokensOk(TemporalSlotKind kind, const QString& fmt, QString* offendingToken) {
    // 空格式串无可校验：放行（空格式的「是否合法」由别处按 type 判定，例如
    // type=string 要求非空 format、type=epochSec 要求空 format，见 load() 后置校验）。
    if (fmt.isEmpty())
        return true;
    // 根据槽种类，组装一张「本槽禁止出现」的 token 黑名单。
    QStringList banned;
    if (kind == TemporalSlotKind::Date) {
        // Date 槽里禁止时间相关 token。
        // 含义：HH/H=24时制小时、hh/h=12时制小时、mm/m=分、ss/s=秒、zzz/z=毫秒、
        //       AP/ap/A/a=上下午标记、t=时区。出现任一即说明「日期槽里混进了时间」。
        banned << QStringLiteral("HH") << QStringLiteral("H") << QStringLiteral("hh")
               << QStringLiteral("h") << QStringLiteral("mm") << QStringLiteral("m")
               << QStringLiteral("ss") << QStringLiteral("s") << QStringLiteral("zzz")
               << QStringLiteral("z") << QStringLiteral("AP") << QStringLiteral("ap")
               << QStringLiteral("A") << QStringLiteral("a") << QStringLiteral("t");
    } else if (kind == TemporalSlotKind::Time) {
        // Time 槽里禁止日期相关 token。
        // 含义：yyyy/yy/y=年、MMMM/MMM/MM/M=月（名/缩写/数字）、dddd/ddd/dd/d=日（含星期名）。
        banned << QStringLiteral("yyyy") << QStringLiteral("yy") << QStringLiteral("y")
               << QStringLiteral("MMMM") << QStringLiteral("MMM") << QStringLiteral("MM")
               << QStringLiteral("M") << QStringLiteral("dddd") << QStringLiteral("ddd")
               << QStringLiteral("dd") << QStringLiteral("d");
    } else {
        return true;  // DateTime / None —— 日期+时间皆可，不做任何限制。
    }
    // 按长度从长到短排序，使多字符 token 先于其子串被匹配。
    // 关键：若不这样，扫描到 "HH" 时会先撞上更短的 "H" 而错报为 "H"——长优先保证报出的是
    //       用户实际写的那个完整 token（如把 "HH" 报成 "HH" 而非 "H"）。
    std::sort(banned.begin(), banned.end(),
              [](const QString& a, const QString& b) { return a.length() > b.length(); });
    // 逐字符扫描格式串，并跳过被单引号包裹的「字面量片段」
    // （Qt 日期格式中 'text' 表示原样输出的文字，其中的字母不应被当作格式 token）。
    bool inQuote = false;
    for (int i = 0; i < fmt.size(); ++i) {
        QChar c = fmt[i];
        if (c == QLatin1Char('\'')) {
            inQuote = !inQuote;  // 遇单引号翻转「引号内/外」状态
            continue;
        }
        if (inQuote)
            continue;  // 引号内的字符是字面量，跳过不校验
        // 在当前位置尝试匹配每个禁用 token（midRef 取从 i 起 tk.size() 长的零拷贝子串视图）。
        for (const QString& tk : banned) {
            if (fmt.midRef(i, tk.size()) == tk) {
                if (offendingToken)
                    *offendingToken = tk;  // 回报命中的违规 token
                return false;
            }
        }
    }
    return true;  // 全程未命中任何禁用 token
}

// slotKindName —— 把槽种类枚举映射回它在 JSON 里对应的字段名。
// 【为什么需要】仅用于「拼装错误信息」：报错时要告诉用户出问题的是哪个字段
//   （如 "...: timeFormat must be a JSON object"），这里提供人类可读的字段名字符串。
// 【返回】静态字符串字面量（无需释放）；None/默认返回 "(none)"。
const char* slotKindName(TemporalSlotKind kind) {
    switch (kind) {
        case TemporalSlotKind::Date:
            return "dateFormat";
        case TemporalSlotKind::DateTime:
            return "datetimeFormat";
        case TemporalSlotKind::Time:
            return "timeFormat";
        case TemporalSlotKind::None:
        default:
            return "(none)";
    }
}

// parseTemporalSide —— 解析时间槽「单侧」（excel 侧 或 db 侧）的子对象 {type?, format?,
// fallback?}。
//
// 【翻译保留原注释】把单侧子对象 { type?, format?, fallback? } 解析进 TemporalSideSpec。
//   值为 null → 报 E_PROFILE_PARSE 类错误；键缺省/undefined → 视为「未声明」（返回 true、out
//   不变）。
//
// 【字段语义回顾（详见 ProfileSpec.h 的 TemporalSideSpec）】
//   type：物理类型，"string"（默认，用格式串）或 "epochSec"（Unix 纪元秒，仅 db 侧 datetime 用）。
//   format：type=string 时的 Qt 格式串；epochSec 时应为空。
//   fallback：仅 excel 侧有意义——主格式解析失败时依次尝试的备选格式串数组。
// 【参数】v=该侧的 JSON 值；kind=所属槽种类（供 token 校验）；ownerLabel=上层已拼好的定位前缀
//   （如 "column 'x': dateFormat"）；sideName="excel"/"db"；out=输出单侧规格；err=错误出参。
// 【返回】true=解析成功或该侧缺省；false=取值非法（详情写 *err）。
// 【副作用】成功时把 out->declared 置 true 并填 type/format/fallback。
// 【错误模式】null 值、未知 type、db 侧误用 fallback、fallback 非数组/含空串、含禁用 token 等
//   均 → 失败（关联 Errors.h::E_PROFILE_PARSE，由上层贴码）。
bool parseTemporalSide(const QJsonValue& v, TemporalSlotKind kind, const QString& ownerLabel,
                       const QString& sideName, TemporalSideSpec* out, QString* err) {
    // 键不存在 → 该侧「未声明」，保持 out 原样（declared 仍为 false），成功返回。
    if (v.isUndefined())
        return true;
    // 键存在但写成 JSON null → 视为配置错误（用户想表达「某侧」就得给一个对象）。
    if (v.isNull()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + sideName +
                   QStringLiteral(" must be an object, not null");
        return false;
    }
    // 键存在但不是对象（写成了数字/字符串/数组等）→ 类型错误。
    if (!v.isObject()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + sideName +
                   QStringLiteral(" must be a JSON object");
        return false;
    }
    QJsonObject o = v.toObject();
    out->declared = true;  // 走到这里即确认本侧被显式声明（影响后续与 profile 默认的合并）

    // ── type 字段 ──：缺省或空 → 默认 "string"；显式 null → 报错。
    // 注意下方分支顺序：先用「非 undefined 且非 null」取值，再单独处理 null；
    // 取不到（缺省）时 typeStr 保持初值 "string"。
    QJsonValue typeVal = o.value(QStringLiteral("type"));
    QString typeStr = QStringLiteral("string");
    if (!typeVal.isUndefined() && !typeVal.isNull()) {
        typeStr = typeVal.toString();
    } else if (typeVal.isNull()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + sideName +
                   QStringLiteral(".type must not be null");
        return false;
    }
    // 把 type 字符串解析成枚举（"string"/"epochSec"）；无法识别 → 报错并列出允许值。
    auto physType = temporalPhysTypeFromString(typeStr);
    if (!physType.has_value()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + sideName + QStringLiteral(".type='") +
                   typeStr +
                   QStringLiteral("' is not recognized; allowed: \"string\", \"epochSec\"");
        return false;
    }
    out->type = physType.value();

    // ── format 字段 ──：显式 null → 报错；缺省/空串 → 留空（对 epochSec 是合法的）。
    // 这里只解析、不做「type×format 自洽」校验（如 string 必须有 format）——那一步集中放在
    // load() 的后置校验（validateColumn）里统一做，以便对「列级合并 profile
    // 默认后的有效规格」校验。
    QJsonValue fmtVal = o.value(QStringLiteral("format"));
    if (fmtVal.isNull()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + sideName +
                   QStringLiteral(".format must not be null");
        return false;
    }
    out->format = fmtVal.toString();  // 缺省 → 空串（toString 对 undefined 返回 ""）

    // M-04 fix：fallback 只对 excel 侧有意义（它服务于 Excel→内存
    // 的解析方向：主格式失败再试备选）。
    //   db 侧（内存→DB / DB→内存）没有「多格式回退」的语义，若用户在 db 侧写了 fallback，
    //   多半是配置笔误——在此显式拒绝，避免它被默默忽略而让用户误以为生效。
    if (sideName == QLatin1String("db") && !o.value(QStringLiteral("fallback")).isUndefined()) {
        if (err)
            *err = ownerLabel + QStringLiteral(
                                    ": db.fallback is not allowed — fallback only applies to the "
                                    "excel side (Excel→memory parse direction)");
        return false;
    }

    // ── fallback 字段 ──：显式 null → 报错；缺省/[] → 空列表；否则必须是「非空字符串」的数组。
    QJsonValue fbVal = o.value(QStringLiteral("fallback"));
    if (fbVal.isNull()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + sideName +
                   QStringLiteral(".fallback must not be null");
        return false;
    }
    if (!fbVal.isUndefined()) {
        if (!fbVal.isArray()) {
            if (err)
                *err = ownerLabel + QStringLiteral(": ") + sideName +
                       QStringLiteral(".fallback must be a JSON array of format strings");
            return false;
        }
        // 逐个收集备选格式串；任一为空串即报错（空格式串无意义，且会掩盖配置疏漏）。
        for (const auto& fv : fbVal.toArray()) {
            QString s = fv.toString();
            if (s.isEmpty()) {
                if (err)
                    *err = ownerLabel + QStringLiteral(": ") + sideName +
                           QStringLiteral(".fallback contains empty string");
                return false;
            }
            out->fallback.append(s);
        }
    }

    // ── Qt token 校验 ──：仅当 type=string 时才校验格式串里的 token 是否与槽种类冲突
    //   （epochSec 不用格式串，没有 token 可校验）。主 format 与每个 fallback 都要过一遍。
    if (out->type == TemporalPhysType::String) {
        QString offending;
        // 校验主 format。
        if (!slotFormatTokensOk(kind, out->format, &offending)) {
            if (err)
                *err = ownerLabel + QStringLiteral(": ") + sideName +
                       QStringLiteral(".format contains forbidden token '") + offending +
                       QStringLiteral("' for this slot type");
            return false;
        }
        // 逐个校验 fallback 备选格式（qAsConst 避免对容器的隐式深拷贝触发 detach）。
        for (const QString& s : qAsConst(out->fallback)) {
            if (!slotFormatTokensOk(kind, s, &offending)) {
                if (err)
                    *err = ownerLabel + QStringLiteral(": ") + sideName +
                           QStringLiteral(".fallback entry '") + s +
                           QStringLiteral("' contains forbidden token '") + offending + '\'';
                return false;
            }
        }
    }

    return true;
}

// readTemporalSlot —— 解析「一个完整时间槽」对象（含 excel 侧 + db 侧），并把两种历史写法归一化。
//
// 【翻译保留原注释】解析单个时间槽对象。支持两种写法：
//   旧式（Legacy）：{ excelFormat, dbFormat?, excelFormatFallback? }  —— 扁平字段，隐含
//   type=string。 新式（New）：    { excel?: {type,format,fallback}, db?: {type,format,fallback} }
//   —— 分侧子对象。 两种写法混用于同一槽对象 → 报 E_PROFILE_PARSE 类错误（不允许半新半旧）。
//
// 【做什么】判别用的是哪种写法；旧式会被「翻译」成新式（即填进 out->excel/out->db，type 固定
// string），
//   使下游只需面对统一的新式内存模型。这是「归一化」职责的核心。
// 【参数】v=槽的 JSON 值；kind=槽种类；ownerLabel=定位前缀（"profile" 或 "column 'x'"）；
//   out=输出整槽规格；err=错误出参。
// 【返回】true=成功或槽缺省；false=失败。【副作用】成功时填 out（declared/excel/db）。
// 【错误模式】非对象、新旧混用、各侧子字段非法（见 parseTemporalSide 及下方旧式分支）。
bool readTemporalSlot(const QJsonValue& v, TemporalSlotKind kind, const QString& ownerLabel,
                      TemporalFormatSpec* out, QString* err) {
    // 槽缺省或显式 null → 视为「未声明此槽」，保持 out 原样成功返回（与 parseTemporalSide 一致：
    // 槽这一层的 null 是宽容的——表示「不配置」，而非错误；更严的 null 检查在各侧子字段上）。
    if (v.isUndefined() || v.isNull())
        return true;
    if (!v.isObject()) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)) +
                   QStringLiteral(" must be a JSON object");
        return false;
    }
    QJsonObject o = v.toObject();
    out->declared = true;  // 槽对象存在即标记「本槽已声明」

    // 探测写法：是否出现任一「旧式扁平键」 vs 是否出现任一「新式子对象键」。
    bool hasLegacy = o.contains(QStringLiteral("excelFormat")) ||
                     o.contains(QStringLiteral("dbFormat")) ||
                     o.contains(QStringLiteral("excelFormatFallback"));
    bool hasNew = o.contains(QStringLiteral("excel")) || o.contains(QStringLiteral("db"));

    // 两种写法不可混用——否则语义含糊（哪一套生效？），直接拒绝。
    if (hasLegacy && hasNew) {
        if (err)
            *err = ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)) +
                   QStringLiteral(
                       ": cannot mix legacy fields (excelFormat/dbFormat/excelFormatFallback)"
                       " with new sub-objects (excel/db) in the same slot object");
        return false;
    }

    if (hasLegacy) {
        // 旧式 → 新式归一化（type 一律为 string，因为旧式没有「物理类型」概念）。
        // 【翻译保留原注释】只有当对应的旧式键「显式出现」时，才认为该侧被声明。
        //   这保留了原有行为：缺省的 excelFormat/dbFormat 仍从 profile 级继承
        //   （而非被当作空串覆盖掉 profile 默认）。
        bool hasExcelFormat = o.contains(QStringLiteral("excelFormat"));
        bool hasDbFormat = o.contains(QStringLiteral("dbFormat"));
        bool hasFallback = o.contains(QStringLiteral("excelFormatFallback"));

        // —— 处理 excel 侧：只要写了 excelFormat 或 excelFormatFallback 之一，就构造 excel 侧。
        if (hasExcelFormat || hasFallback) {
            TemporalSideSpec excelSide;
            excelSide.declared = true;
            excelSide.type = TemporalPhysType::String;

            // M-02 fix：excelFormat 显式写成 null 必须拒绝（不能默默当成空串 "" 处理）。
            if (hasExcelFormat && o.value(QStringLiteral("excelFormat")).isNull()) {
                if (err)
                    *err = ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)) +
                           QStringLiteral(".excelFormat must not be null");
                return false;
            }
            // 缺省 excelFormat 时 toString() 返回空串——此时本侧仍 declared，靠空串触发后置校验
            // 或与 profile 侧合并（具体合并规则见 ProfileSpec.h::effectiveTemporalFor）。
            excelSide.format = o.value(QStringLiteral("excelFormat")).toString();

            QJsonValue fbVal = o.value(QStringLiteral("excelFormatFallback"));
            // M-02 fix：excelFormatFallback 显式 null 必须拒绝（不能默默跳过当作没写）。
            if (hasFallback && fbVal.isNull()) {
                if (err)
                    *err = ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)) +
                           QStringLiteral(".excelFormatFallback must not be null");
                return false;
            }
            // 收集旧式备选格式数组：必须是数组、元素非空串、且每条都通过 token 校验。
            if (!fbVal.isUndefined() && !fbVal.isNull()) {
                if (!fbVal.isArray()) {
                    if (err)
                        *err = ownerLabel + QStringLiteral(": ") +
                               QLatin1String(slotKindName(kind)) +
                               QStringLiteral(".excelFormatFallback must be a JSON array");
                    return false;
                }
                for (const auto& fv : fbVal.toArray()) {
                    QString s = fv.toString();
                    if (s.isEmpty()) {
                        if (err)
                            *err = ownerLabel + QStringLiteral(": ") +
                                   QLatin1String(slotKindName(kind)) +
                                   QStringLiteral(".excelFormatFallback contains empty string");
                        return false;
                    }
                    QString offending;
                    if (!slotFormatTokensOk(kind, s, &offending)) {
                        if (err)
                            *err = ownerLabel + QStringLiteral(": ") +
                                   QLatin1String(slotKindName(kind)) +
                                   QStringLiteral(".excelFormatFallback entry '") + s +
                                   QStringLiteral("' contains forbidden token '") + offending +
                                   '\'';
                        return false;
                    }
                    excelSide.fallback.append(s);
                }
            }

            // 主 excelFormat 也要过 token 校验（fallback 已在上面逐条校验过）。
            QString offending;
            if (!slotFormatTokensOk(kind, excelSide.format, &offending)) {
                if (err)
                    *err = ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)) +
                           QStringLiteral(".excelFormat contains forbidden token '") + offending +
                           QStringLiteral("' for this slot type");
                return false;
            }
            out->excel = excelSide;  // 旧式 excel 字段 → 写入新式 out->excel
        }

        // —— 处理 db 侧：旧式 db 侧只有一个 dbFormat（无 fallback、无 type 概念）。
        if (hasDbFormat) {
            TemporalSideSpec dbSide;
            dbSide.declared = true;
            dbSide.type = TemporalPhysType::String;

            // M-02 fix：dbFormat 显式 null 必须拒绝（不能默默当作空串 ""）。
            if (o.value(QStringLiteral("dbFormat")).isNull()) {
                if (err)
                    *err = ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)) +
                           QStringLiteral(".dbFormat must not be null");
                return false;
            }
            dbSide.format = o.value(QStringLiteral("dbFormat")).toString();

            QString offending;
            if (!slotFormatTokensOk(kind, dbSide.format, &offending)) {
                if (err)
                    *err = ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)) +
                           QStringLiteral(".dbFormat contains forbidden token '") + offending +
                           QStringLiteral("' for this slot type");
                return false;
            }
            out->db = dbSide;  // 旧式 dbFormat → 写入新式 out->db
        }
    } else {
        // 新式：直接解析 excel/db 两个子对象（任一可缺省 → 该侧 undeclared）。
        // 这里把已拼好的「ownerLabel: 槽名」作为定位前缀传下去，使错误信息能精确到
        // "column 'x': dateFormat: excel.format ..." 这样的层级。
        if (!parseTemporalSide(
                o.value(QStringLiteral("excel")), kind,
                ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)),
                QStringLiteral("excel"), &out->excel, err))
            return false;
        if (!parseTemporalSide(
                o.value(QStringLiteral("db")), kind,
                ownerLabel + QStringLiteral(": ") + QLatin1String(slotKindName(kind)),
                QStringLiteral("db"), &out->db, err))
            return false;
    }

    return true;
}

}  // namespace

// isSimpleIdentifier —— 判断字符串是否为「简单标识符」：以字母或下划线开头，后接字母/数字/下划线。
// 【为什么】表名、列名等会被直接拼进 SQL；限定为这种保守形态可避免 SQL 注入与引号转义问题
//   （配置里只允许「裸标识符」，不接受带空格/点号/特殊字符的名字）。re 用 static 只编译一次。
static bool isSimpleIdentifier(const QString& s) {
    static QRegularExpression re(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return re.match(s).hasMatch();
}

// isTableDotColumn —— 判断字符串是否为「table.column」限定形态，或退化为一个简单标识符。
// 【为什么】导出排序 orderBy 既可写 "col"（裸列名），也可写 "table.col"（多表导出时消歧），
//   两种都合法。先试 "标识符.标识符" 形态，不匹配再退回 isSimpleIdentifier。
static bool isTableDotColumn(const QString& s) {
    static QRegularExpression re(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*\\.[A-Za-z_][A-Za-z0-9_]*$"));
    return re.match(s).hasMatch() || isSimpleIdentifier(s);
}

// ProfileLoader::validateToken —— 校验单个 validator token 是否属于受支持的「校验规则词汇表」。
//
// 【做什么】只做「语法层面」的合法性判断——确认 token 形态是引擎认识的某种规则；不在此处执行规则
//   （真正对单元格数据跑校验是 validation 子系统在导入时做的事）。这一步是「配置自检」。
// 【支持的 token 文法】
//   · 固定关键字：notNull（非空）、int（整数）、decimal（小数）。
//   · len<=N / len>=N      —— 长度上/下限（lenRe：len 后接 < 或 > 再接 =，再接十进制数字）。
//   · int>=N（N 可为负）   —— 整数下限（intGeRe，允许前导负号）。
//   · date:<fmt>           —— 按给定格式做日期校验（dateRe：date: 后至少一个字符）。
//   · regex:<pattern>      —— 正则匹配（regexRe）。
//   · enum:<...>           —— 枚举取值（enumRe）。
// 【参数】token=待校验的原始字符串；err=可选错误出参。
// 【返回】true=属于已知文法；false=无法识别（写入 *err："Unknown validator token: ..."）。
// 【副作用】无（仅可能写 *err）。【复杂度】O(token 长度)，固定几个正则。
// 【对应错误码】校验失败本身在运行时关联 Errors.h 的 E_VALIDATE_*；此处仅做配置期文法把关。
bool ProfileLoader::validateToken(const QString& token, QString* err) {
    // 这些正则用 static 仅在首次调用时编译一次（QRegularExpression 编译开销不低）。
    static QRegularExpression lenRe(QStringLiteral("^len[<>]=\\d+$"));   // len<=N / len>=N
    static QRegularExpression intGeRe(QStringLiteral("^int>=-?\\d+$"));  // int>=N（N 可负）
    static QRegularExpression dateRe(QStringLiteral("^date:.+$"));       // date:<fmt>
    static QRegularExpression regexRe(QStringLiteral("^regex:.+$"));     // regex:<pattern>
    static QRegularExpression enumRe(QStringLiteral("^enum:.+$"));       // enum:<values>

    // 先匹配三个无参数的固定关键字。
    if (token == QStringLiteral("notNull") || token == QStringLiteral("int") ||
        token == QStringLiteral("decimal")) {
        return true;
    }
    // 再依次尝试带参数的文法。
    if (lenRe.match(token).hasMatch())
        return true;
    if (intGeRe.match(token).hasMatch())
        return true;
    if (dateRe.match(token).hasMatch())
        return true;
    if (regexRe.match(token).hasMatch())
        return true;
    if (enumRe.match(token).hasMatch())
        return true;

    // 全不匹配 → 未知 token，配置非法。
    if (err)
        *err = QStringLiteral("Unknown validator token: ") + token;
    return false;
}

// ProfileLoader::readColumn —— 解析「一列」的映射定义为 ColumnSpec。
//
// 【做什么】把 JSON 里「列对象」的各字段读进 ColumnSpec：
//   · dbColumn ← 由 JSON 键 dbCol 给出（即数据库列名，必须是合法标识符）。
//   · source   ← Excel 表头名；缺省时回退为 dbCol（即「同名」约定，省去重复书写）。
//   · validatorTokens ← validators 数组里的校验规则 token（逐个过 validateToken 把关）。
//   · 三个时间格式槽（dateFormat/datetimeFormat/timeFormat）—— 列级覆盖，会与 profile 级合并。
// 【为什么 source 回退到 dbCol】绝大多数场景 Excel 表头名与 DB 列名一致，故把「不写 source」
//   定义为「与列同名」，是降低配置噪音的默认值。
// 【参数】dbCol=JSON 中该列的键（=DB 列名）；o=列对象；out=输出列规格；err=错误出参。
// 【返回】true=成功；false=列名非法 / 校验 token 非法 / 时间槽解析失败。
// 【副作用】填充 *out。【错误模式】关联 Errors.h::E_PROFILE_PARSE（由上层贴码）。
// 【上下文】被 readRoute 对 columns 对象的每个键值对各调用一次。
bool ProfileLoader::readColumn(const QString& dbCol, const QJsonObject& o, ColumnSpec* out,
                               QString* err) {
    // DB 列名必须是简单标识符（直接进 SQL，防注入/转义问题）。
    if (!isSimpleIdentifier(dbCol)) {
        if (err)
            *err = QStringLiteral("column name is not a valid identifier: ") + dbCol;
        return false;
    }
    out->dbColumn = dbCol;
    // source 缺省回退为 dbCol：toString(默认值) 在键不存在/非字符串时返回该默认值。
    out->source = o.value(QStringLiteral("source")).toString(dbCol);

    // 逐个解析 validators 数组（非数组时 toArray() 返回空数组 → 视为「无校验」）。
    QJsonArray va = o.value(QStringLiteral("validators")).toArray();
    for (const auto& v : va) {
        QString token = v.toString();
        if (!validateToken(token, err))  // 任一 token 文法非法即整列失败
            return false;
        out->validatorTokens.append(token);
    }

    // add-time-format-profile：列级时间格式槽（逐字段覆盖 profile 级默认）。
    // colLabel 作为定位前缀传给 readTemporalSlot，使错误信息能精确到本列。
    // 注意：此处不校验「三槽至多声明其一」——那一互斥约束在 load() 的后置校验里统一查。
    QString colLabel = QStringLiteral("column '") + dbCol + '\'';
    if (!readTemporalSlot(o.value(QStringLiteral("dateFormat")), TemporalSlotKind::Date, colLabel,
                          &out->dateFormat, err))
        return false;
    if (!readTemporalSlot(o.value(QStringLiteral("datetimeFormat")), TemporalSlotKind::DateTime,
                          colLabel, &out->datetimeFormat, err))
        return false;
    if (!readTemporalSlot(o.value(QStringLiteral("timeFormat")), TemporalSlotKind::Time, colLabel,
                          &out->timeFormat, err))
        return false;

    return true;
}

// ProfileLoader::readRoute —— 解析「一条路由」对象为 RouteSpec（本类最核心、最长的解析函数）。
//
// 【一条路由代表什么】「一行 Excel 数据要写入的一张目标表」及其全部映射材料：
//   目标表名、父路由、UPSERT 冲突键、外键注入(fkInject)、外键查找(lookups)、列映射(columns)。
//   单表模式整份 profile 是一条路由；多表/混合模式有多条（见 readMultiTable/readMixed）。
//
// 【解析顺序】table → parent → conflict → fkInject → lookups → columns，逐段读取、逐段校验。
// 【参数】o=路由 JSON 对象；out=输出路由规格；err=错误出参；
//   warnings=可选「信息级诊断」收集器（非阻断，如 exportOnMissing 无效配置的提示）。
// 【返回】true=成功；false=任一字段非法。【副作用】填充 *out，可能 append 到 *warnings。
// 【错误模式】缺 table、非法标识符、fkInject/lookups 结构错等，统一关联 E_PROFILE_PARSE。
// 【复杂度】O(列数 + fkInject 对数 + lookup 数 × 其 match/select 项数)，线性遍历 JSON。
bool ProfileLoader::readRoute(const QJsonObject& o, RouteSpec* out, QString* err,
                              QStringList* warnings) {
    // ── table（目标表名，必填）──
    out->table = o.value(QStringLiteral("table")).toString();
    if (out->table.isEmpty()) {
        if (err)
            *err = QStringLiteral("route missing 'table'");
        return false;
    }
    if (!isSimpleIdentifier(out->table)) {  // 表名直接进 SQL，必须是裸标识符
        if (err)
            *err = QStringLiteral("route table is not a valid identifier: ") + out->table;
        return false;
    }

    // ── parent（父路由表名，可选）──：空串表示「根路由」（无父）。
    // 此字段在多表写入时用于建立父子依赖（→ 拓扑排序 → 父先写、子后写）；此处仅原样读入，
    // 「父表是否真实存在/是否成环」由后续 Validator/排序阶段判定。
    out->parent = o.value(QStringLiteral("parent")).toString();

    // ── conflict（UPSERT 冲突键）──：conflict.columns 数组逐列读入。
    // 这组列决定「插入还是更新」（须恰好对应目标表的 PRIMARY KEY 或某 UNIQUE 约束，由 Validator
    // 核对）。 非对象/缺省时 toObject()/toArray() 返回空 → 冲突键为空（合法性由 Validator 在需要
    // UPSERT 时判， 缺冲突键关联 Errors.h::E_PROFILE_NO_CONFLICT_KEY）。
    QJsonObject conflictObj = o.value(QStringLiteral("conflict")).toObject();
    QJsonArray conflictCols = conflictObj.value(QStringLiteral("columns")).toArray();
    for (const auto& c : conflictCols) {
        QString col = c.toString();
        if (!isSimpleIdentifier(col)) {  // 冲突键列名同样要求是裸标识符
            if (err)
                *err = QStringLiteral("conflict column is not a valid identifier: ") + col;
            return false;
        }
        out->conflict.columns.append(col);
    }

    // ── fkInject（外键注入组）──：父表写完后把父表的主键值复制到本（子）表对应列。
    // 取值规则：缺省/null/空数组 [] 都视为「无 fkInject」（no-op）；旧的单对象形式被显式拒绝。
    // 用一对花括号 {} 把这段局部变量限定在自己的作用域内，避免与下方 lookups 段变量名冲突。
    {
        QJsonValue fkVal = o.value(QStringLiteral("fkInject"));
        if (!fkVal.isUndefined() && !fkVal.isNull()) {
            // 旧的单对象形式 {"from":"t.c","to":"t.c"} 已移除——显式给出迁移提示而非默默忽略。
            if (fkVal.isObject()) {
                if (err)
                    *err =
                        QStringLiteral("route '") + out->table +
                        QStringLiteral(
                            "': fkInject must be an array [{from,pairs:[[p_col,c_col],...]},...]; "
                            "old {\"from\":\"t.c\",\"to\":\"t.c\"} object form is removed");
                return false;
            }
            // 既非对象也非数组（如写成字符串/数字）→ 类型错误。
            if (!fkVal.isArray()) {
                if (err)
                    *err = QStringLiteral("route '") + out->table +
                           QStringLiteral("': fkInject must be an array");
                return false;
            }
            // 遍历每个注入「组」：一组 = 一个父表 from + 一组 (父列,子列) 对 pairs。
            for (const auto& fkElem : fkVal.toArray()) {
                QJsonObject fkObj = fkElem.toObject();
                FkInjectSpec fk;
                // from：父表名，必填且须为合法标识符（它必须是本 profile 里的另一条路由——
                // 这点由 Validator 核对；此处只做形态校验）。
                fk.fromTable = fkObj.value(QStringLiteral("from")).toString();
                if (fk.fromTable.isEmpty()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': fkInject group missing 'from'");
                    return false;
                }
                if (!isSimpleIdentifier(fk.fromTable)) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': fkInject 'from' is not a valid identifier: ") +
                               fk.fromTable;
                    return false;
                }
                // pairs：必须是数组，且非空（一个注入组至少要注入一列才有意义）。
                QJsonValue pairsVal = fkObj.value(QStringLiteral("pairs"));
                if (!pairsVal.isArray()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': fkInject group from='") + fk.fromTable +
                               QStringLiteral("' missing 'pairs' array");
                    return false;
                }
                QJsonArray pairsArr = pairsVal.toArray();
                if (pairsArr.isEmpty()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': fkInject group from='") + fk.fromTable +
                               QStringLiteral("' has empty 'pairs'");
                    return false;
                }
                // 每个 pair 必须恰好是二元组 [父列, 子列]，两个名字都须是合法标识符。
                for (const auto& pairElem : pairsArr) {
                    QJsonArray pair = pairElem.toArray();
                    if (pair.size() != 2) {
                        if (err)
                            *err =
                                QStringLiteral("route '") + out->table +
                                QStringLiteral("': fkInject pair must be [parent_col, child_col]");
                        return false;
                    }
                    QString parentCol = pair[0].toString();  // 父表中被取值的列
                    QString childCol = pair[1].toString();   // 本（子）表中被注入的列
                    if (!isSimpleIdentifier(parentCol) || !isSimpleIdentifier(childCol)) {
                        if (err)
                            *err = QStringLiteral("route '") + out->table +
                                   QStringLiteral("': fkInject pair contains invalid identifier");
                        return false;
                    }
                    fk.pairs.append({parentCol, childCol});
                }
                out->fkInject.append(fk);
            }
        }
    }

    // ── lookups（外键正向/反向查找组）──：用 Excel 业务键去同库的参照表 G 查出代理主键等列。
    // 取值规则：缺省/null 视为「无 lookup」；若出现则必须是数组。同样用 {} 限定局部作用域。
    {
        QJsonValue lookupsVal = o.value(QStringLiteral("lookups"));
        if (!lookupsVal.isUndefined() && !lookupsVal.isNull()) {
            if (!lookupsVal.isArray()) {
                if (err)
                    *err = QStringLiteral("route '") + out->table +
                           QStringLiteral("': 'lookups' must be an array");
                return false;
            }
            // seenLookupNames：在本路由内检测重名（lookup name 须路由内唯一，用作诊断标识）。
            QSet<QString> seenLookupNames;
            for (const auto& lv : lookupsVal.toArray()) {
                QJsonObject lo = lv.toObject();
                LookupSpec lk;
                // name：必填非空，trimmed() 去首尾空白（避免「看似不同实则相同」的名字）。
                lk.name = lo.value(QStringLiteral("name")).toString().trimmed();
                if (lk.name.isEmpty()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': lookup missing non-empty 'name'");
                    return false;
                }
                if (seenLookupNames.contains(lk.name)) {  // 路由内重名 → 拒绝
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': duplicate lookup name '") + lk.name + '\'';
                    return false;
                }
                // from：参照表 G 的表名（必填）。区别于 fkInject 的 from——后者指本 profile 内的
                // 另一条路由，而这里指外部参照表。
                lk.fromTable = lo.value(QStringLiteral("from")).toString();
                if (lk.fromTable.isEmpty()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': lookup '") + lk.name +
                               QStringLiteral("' missing 'from'");
                    return false;
                }

                // ── match（匹配条件）──：形如 [[G_列, Excel_表头], ...]，即「在 G 表里按这些列
                // 等于对应 Excel 列的值」来定位记录。必须是非空数组，且明确拒绝对象写法（防误用）。
                QJsonValue matchVal = lo.value(QStringLiteral("match"));
                if (matchVal.isObject()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': lookup '") + lk.name +
                               QStringLiteral(
                                   "' match must be [[G_col,excel_header],...], not object");
                    return false;
                }
                if (!matchVal.isArray() || matchVal.toArray().isEmpty()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': lookup '") + lk.name +
                               QStringLiteral("' missing non-empty 'match' array");
                    return false;
                }
                // 逐条收集匹配对：每条须是二元组 [G_列, Excel_表头]。
                for (const auto& mv : matchVal.toArray()) {
                    QJsonArray pair = mv.toArray();
                    if (pair.size() != 2) {
                        if (err)
                            *err = QStringLiteral("route '") + out->table +
                                   QStringLiteral("': lookup '") + lk.name +
                                   QStringLiteral("' match entry must be [G_column, excel_header]");
                        return false;
                    }
                    lk.match.append({pair[0].toString(), pair[1].toString()});
                }

                // ── select（取出列）──：形如 [[G_列, 目标_dbColumn], ...]，即「把 G 表命中行的
                // 这些列取出来，分别作为本路由载荷上的局部 dbColumn」。规则同
                // match：非空数组、拒绝对象。
                QJsonValue selectVal = lo.value(QStringLiteral("select"));
                if (selectVal.isObject()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': lookup '") + lk.name +
                               QStringLiteral(
                                   "' select must be [[G_col,target_dbColumn],...], not object");
                    return false;
                }
                if (!selectVal.isArray() || selectVal.toArray().isEmpty()) {
                    if (err)
                        *err = QStringLiteral("route '") + out->table +
                               QStringLiteral("': lookup '") + lk.name +
                               QStringLiteral("' missing non-empty 'select' array");
                    return false;
                }
                // 逐条收集取出对：每条须是二元组 [G_列, 目标_dbColumn]。
                for (const auto& sv : selectVal.toArray()) {
                    QJsonArray pair = sv.toArray();
                    if (pair.size() != 2) {
                        if (err)
                            *err = QStringLiteral("route '") + out->table +
                                   QStringLiteral("': lookup '") + lk.name +
                                   QStringLiteral(
                                       "' select entry must be [G_column, target_dbColumn]");
                        return false;
                    }
                    lk.select.append({pair[0].toString(), pair[1].toString()});
                }

                // ── exportRoundtrip（导出方向开关，默认 true）──
                // add-export-reverse-lookup：true=导出时用代理主键反查回业务键再写 Excel；
                //   false=跳过反查，直接把 select 取出的列（H）原样输出。缺省/null → 用默认 true；
                //   出现则必须是布尔。
                QJsonValue ertVal = lo.value(QStringLiteral("exportRoundtrip"));
                if (!ertVal.isUndefined() && !ertVal.isNull()) {
                    if (!ertVal.isBool()) {
                        if (err)
                            *err = QStringLiteral("route '") + out->table +
                                   QStringLiteral("': lookup '") + lk.name +
                                   QStringLiteral("': exportRoundtrip must be a boolean");
                        return false;
                    }
                    lk.exportRoundtrip = ertVal.toBool();
                }

                // ── exportOnMissing（导出反查未命中策略，默认 "error"）──
                // 取值须是 "error"/"null"/"skip" 之一（见 ProfileSpec.h::ExportOnMissing）。
                // exportOnMissingExplicit：记录用户是否「显式」写了该字段，供下方 2.4 诊断使用。
                bool exportOnMissingExplicit = false;
                QJsonValue eomVal = lo.value(QStringLiteral("exportOnMissing"));
                if (!eomVal.isUndefined() && !eomVal.isNull()) {
                    QString eom = eomVal.toString();
                    if (!ExportOnMissing::isValid(eom)) {  // 取值不在白名单 → 报错并列出允许值
                        if (err)
                            *err = QStringLiteral("route '") + out->table +
                                   QStringLiteral("': lookup '") + lk.name +
                                   QStringLiteral("': exportOnMissing value '") + eom +
                                   QStringLiteral("' is not allowed; use one of ") +
                                   ExportOnMissing::allowedList();
                        return false;
                    }
                    lk.exportOnMissing = eom;
                    exportOnMissingExplicit = true;
                } else {
                    // 缺省 → 用默认 "error"（未命中即报行级错，关联 E_REVERSE_LOOKUP_NOT_FOUND）。
                    lk.exportOnMissing = QString::fromLatin1(ExportOnMissing::kError);
                }

                // 2.4：当 exportRoundtrip=false（压根不反查）却又显式写了 exportOnMissing 时，
                //   后者其实「无作用」——这不是错误，但提示用户配置冗余/可能误解，记一条信息级诊断。
                if (!lk.exportRoundtrip && exportOnMissingExplicit && warnings) {
                    warnings->append(
                        QStringLiteral("route '") + out->table + QStringLiteral("': lookup '") +
                        lk.name +
                        QStringLiteral("': exportOnMissing has no effect when exportRoundtrip is "
                                       "false"));
                }

                seenLookupNames.insert(lk.name);  // 记下名字以检测后续重名
                out->lookups.append(lk);
            }
        }
    }

    // ── columns（列映射表）──：columns 是「DB列名 → 列定义对象」的 JSON 对象（不是数组）。
    // 遍历每个键值对：键=DB 列名（传给 readColumn 做 dbCol），值=该列的定义对象。
    QJsonObject colsObj = o.value(QStringLiteral("columns")).toObject();
    for (auto it = colsObj.begin(); it != colsObj.end(); ++it) {
        ColumnSpec col;
        if (!readColumn(it.key(), it.value().toObject(), &col,
                        err))  // 逐列解析，任一失败即整路由失败
            return false;
        out->columns.append(col);
    }

    return true;
}

// ProfileLoader::readSingleTable —— 解析「单表模式」profile。
//
// 【单表模式特点】整份 profile 的顶层对象「本身就是一条路由」——table/columns/conflict 等
//   直接写在顶层，没有 routes 数组。因此本函数只是把顶层对象当作一条路由交给 readRoute。
// 【参数】o=profile 顶层对象；out=输出（会设 mode 并追加唯一一条路由）；err=错误出参。
// 【返回】true=成功；false=缺 table 或路由解析失败。【副作用】out->mode 置 SingleTable，
//   并向 out->routes 追加 1 条；解析中的诊断写入 out->loadWarnings。
bool ProfileLoader::readSingleTable(const QJsonObject& o, ProfileSpec* out, QString* err) {
    out->mode = ProfileMode::SingleTable;

    // 委派前先校验必填的 table（虽然 readRoute 也会查，但这里给出更贴合「单表」语境的错误信息）。
    QString table = o.value(QStringLiteral("table")).toString();
    if (table.isEmpty()) {
        if (err)
            *err = QStringLiteral("singleTable profile missing 'table'");
        return false;
    }

    // 委派给 readRoute，使 lookups/fkInject/columns 等也被一并解析（顶层对象即路由对象）。
    RouteSpec route;
    if (!readRoute(o, &route, err, &out->loadWarnings))
        return false;

    out->routes.append(route);
    return true;
}

// ProfileLoader::readMultiTable —— 解析「多表模式」profile。
//
// 【多表模式特点】顶层有一个 routes 数组，列出多条路由（各写一张表，可有父子依赖）。
// 【参数/返回/副作用】同 readSingleTable，区别是向 out->routes 追加 N 条。
// 【错误模式】routes 缺失或为空 → 失败；任一路由解析失败即整体失败。
bool ProfileLoader::readMultiTable(const QJsonObject& o, ProfileSpec* out, QString* err) {
    out->mode = ProfileMode::MultiTable;

    QJsonArray routesArr = o.value(QStringLiteral("routes")).toArray();
    if (routesArr.isEmpty()) {  // 缺省或空数组都不允许（多表模式至少要一条路由）
        if (err)
            *err = QStringLiteral("multiTable profile missing non-empty 'routes'");
        return false;
    }
    for (const auto& rv : routesArr) {
        RouteSpec route;
        if (!readRoute(rv.toObject(), &route, err, &out->loadWarnings))
            return false;
        out->routes.append(route);
    }
    return true;
}

// ProfileLoader::readMixed —— 解析「混合模式」profile。
//
// 【混合模式特点】先用一个「判别列(discriminator)」的值把每行 Excel 分派到某个「类别(class)」，
//   每个类别再各自拥有一组 routes。即「按列值路由到不同的表集合」。结构：
//     discriminator.source  —— 用作判别的 Excel 表头；
//     classes[] = { id, match.equals, routes[] }  —— 各类别及其判别值与路由集合。
// 【为什么 discriminator 可以为空】【翻译保留原注释】discriminator 对「导入」是必需的
//   （要据它把进来的行路由到正确的类别），但对「仅导出」profile 是可选的——这类 profile 只用
//   classes 来描述各类别的路由。允许 discriminatorSource 为空，使仅导出的混合 profile 也能加载；
//   若真去做导入而 discriminatorSource 为空，Router 会在运行时拒绝。
// 【参数/返回/副作用】同上；向 out->classes 追加 N 个类别；out->mode 置 Mixed。
bool ProfileLoader::readMixed(const QJsonObject& o, ProfileSpec* out, QString* err) {
    out->mode = ProfileMode::Mixed;

    // discriminator.source：判别列（可空，见上方说明）。非对象时 toObject() 返回空对象 → 空串。
    QJsonObject discObj = o.value(QStringLiteral("discriminator")).toObject();
    out->discriminatorSource = discObj.value(QStringLiteral("source")).toString();
    // 【翻译保留原注释】discriminator 对导入必需（把进来的行路由到正确类别），但对仅用 classes
    // 描述各类别路由的「仅导出」profile 是可选的。允许空 discriminatorSource，让仅导出混合 profile
    // 无错加载。Router 会在运行时——若真发起导入而 discriminatorSource 为空——拒绝该次导入。

    // classes：类别数组，必须非空（混合模式至少要一个类别）。
    QJsonArray classesArr = o.value(QStringLiteral("classes")).toArray();
    if (classesArr.isEmpty()) {
        if (err)
            *err = QStringLiteral("mixed profile missing non-empty 'classes'");
        return false;
    }

    // 逐个解析类别。
    for (const auto& cv : classesArr) {
        QJsonObject co = cv.toObject();
        ClassSpec cls;
        // id：类别标识，必填（如 "A"/"B"；也用于导出时写入 classColumn）。
        cls.id = co.value(QStringLiteral("id")).toString();
        if (cls.id.isEmpty()) {
            if (err)
                *err = QStringLiteral("class missing 'id'");
            return false;
        }
        // match.equals：判别值（MVP 仅支持等值匹配）。仅导出 profile 可留空。
        QJsonObject matchObj = co.value(QStringLiteral("match")).toObject();
        cls.matchEquals = matchObj.value(QStringLiteral("equals")).toString();

        // 本类别的 routes：必须非空（一个类别至少要有一条路由）。
        QJsonArray routesArr = co.value(QStringLiteral("routes")).toArray();
        if (routesArr.isEmpty()) {
            if (err)
                *err = QStringLiteral("class '") + cls.id + QStringLiteral("' has no routes");
            return false;
        }
        for (const auto& rv : routesArr) {
            RouteSpec route;
            if (!readRoute(rv.toObject(), &route, err, &out->loadWarnings))
                return false;
            cls.routes.append(route);
        }
        out->classes.append(cls);
    }

    // matchEquals 唯一性检查——仅在「存在判别列」（即可导入的 profile）时才做。
    // 【翻译保留原注释】仅导出的混合 profile 没有 discriminator、也没有 matchEquals 值；
    //   跳过此检查，以允许多个类别都带空 matchEquals。
    // 【为什么必须唯一】导入时靠 matchEquals 把行分到唯一类别；若两个类别判别值相同则路由有歧义。
    if (!out->discriminatorSource.isEmpty()) {
        QHash<QString, QString>
            seen;  // matchEquals 值 → 首次出现它的类别 id（用于报哪两个类别撞了）
        for (const auto& cls : out->classes) {
            if (seen.contains(cls.matchEquals)) {
                if (err)
                    *err = QStringLiteral("duplicate matchEquals '") + cls.matchEquals +
                           QStringLiteral("' in classes '") + seen[cls.matchEquals] +
                           QStringLiteral("' and '") + cls.id + QStringLiteral("'");
                return false;
            }
            seen[cls.matchEquals] = cls.id;
        }
    }

    // 混合模式不支持原生导出 SQL（export.sql）——因为混合导出靠按类别拼装，与单条原生 SELECT
    // 不兼容。
    QJsonObject expObj = o.value(QStringLiteral("export")).toObject();
    if (expObj.contains(QStringLiteral("sql"))) {
        if (err)
            *err = QStringLiteral("mixed mode does not support export.sql");
        return false;
    }

    return true;
}

// ProfileLoader::load —— 解析总入口：把整份 Profile 文档解析、校验、归一化进 *out。
//
// 【流水线】(详见函数体分段注释)
//   1. 校验文档是顶层对象。
//   2. 读顶层公共字段：profileName(必填) / sheet(必填) / headerRow(默认 1，须 >=1)。
//   3. 按 mode 分派到 readSingleTable / readMultiTable / readMixed 解析路由部分。
//   4. 解析公共的 export 段（orderBy / sql / classColumn / columnOrder，并拒绝已废弃字段）。
//   5. 解析 profile 级三个时间格式槽（作为列级默认）。
//   6. 信息级诊断：列同时声明 dateFormat 与 date: 校验器时记一条提示（dateFormat 胜出）。
//   7. 后置跨字段校验：每列「至多一个时间槽」「type×format 自洽」「epochSec 限制」。
// 【参数】doc=已解析 JSON 文档；out=输出模型；err=错误出参。
// 【返回】true=全部成功；false=任一步失败（详情写 *err）。
// 【副作用】填充 *out 各字段；信息级诊断写入 out->loadWarnings（非阻断）。
// 【错误模式】结构/取值非法统一关联 Errors.h::E_PROFILE_PARSE，由调用方据语境贴码。
bool ProfileLoader::load(const QJsonDocument& doc, ProfileSpec* out, QString* err) {
    // (1) 文档必须是一个 JSON 顶层对象（不能是数组/标量/空文档）。
    if (doc.isNull() || !doc.isObject()) {
        if (err)
            *err = QStringLiteral("Profile JSON is not an object");
        return false;
    }

    QJsonObject o = doc.object();

    // (2) 顶层公共字段 ──────────────────────────────────────────────
    // profileName：profile 名，必填非空（调用方按名选用 profile）。
    out->name = o.value(QStringLiteral("profileName")).toString();
    if (out->name.isEmpty()) {
        if (err)
            *err = QStringLiteral("Profile missing 'profileName'");
        return false;
    }

    // sheet：默认作用的工作表名，必填非空（导入/导出选项未指定 sheet 时回退到它）。
    out->sheet = o.value(QStringLiteral("sheet")).toString();
    if (out->sheet.isEmpty()) {
        if (err)
            *err = QStringLiteral("Profile missing 'sheet'");
        return false;
    }

    // headerRow：表头所在行号（1 基）。toInt(1) —— 缺省或非整数时默认 1；必须 >=1。
    out->headerRow = o.value(QStringLiteral("headerRow")).toInt(1);
    if (out->headerRow < 1) {
        if (err)
            *err = QStringLiteral("headerRow must be >= 1");
        return false;
    }

    // (3) 按 mode 分派 ─────────────────────────────────────────────
    // mode 决定走哪条解析分支、以及运行时如何路由一行数据；未知值即报错。
    QString modeStr = o.value(QStringLiteral("mode")).toString();
    bool ok = false;
    if (modeStr == QStringLiteral("singleTable")) {
        ok = readSingleTable(o, out, err);
    } else if (modeStr == QStringLiteral("multiTable")) {
        ok = readMultiTable(o, out, err);
    } else if (modeStr == QStringLiteral("mixed")) {
        ok = readMixed(o, out, err);
    } else {
        if (err)
            *err = QStringLiteral("Unknown profile mode '") + modeStr +
                   QStringLiteral("'; expected singleTable/multiTable/mixed");
        return false;
    }
    if (!ok)
        return false;  // 路由部分解析失败，err 已由被调函数填好

    // (4) 公共 export 段 ───────────────────────────────────────────
    // 不论哪种 mode，export 子对象都按相同规则解析（混合模式额外禁了 export.sql，已在 readMixed
    // 查过）。
    QJsonObject expObj = o.value(QStringLiteral("export")).toObject();
    // orderBy：导出排序列数组；每项须是 "col" 或 "table.col" 形态（见 isTableDotColumn）。
    QJsonArray orderByArr = expObj.value(QStringLiteral("orderBy")).toArray();
    for (const auto& v : orderByArr) {
        QString s = v.toString();
        if (!isTableDotColumn(s)) {
            if (err)
                *err = QStringLiteral("orderBy contains invalid identifier: ") + s;
            return false;
        }
        out->exportSpec.orderBy.append(s);
    }
    // sql：原生 SELECT（仅 singleTable/multiTable 允许；混合模式已在 readMixed
    // 拒绝）。空串=未配置。
    out->exportSpec.explicitSql = expObj.value(QStringLiteral("sql")).toString();
    // classColumn：导出时把类别 id 写进哪个合成表头。
    // 【翻译保留原注释】classColumn 既可声明在 export 子对象里，也可声明在 profile 顶层。
    //   当 export 级为空时回退到顶层字段，以便那些把 classColumn 放在根部的混合 profile
    //   （仅导出混合 profile 的常见写法）也能被正确处理。
    out->exportSpec.classColumn = expObj.value(QStringLiteral("classColumn")).toString();
    if (out->exportSpec.classColumn.isEmpty())
        out->exportSpec.classColumn = o.value(QStringLiteral("classColumn")).toString();

    // add-export-column-order：解析可选的 columnOrder 数组（按 Excel 表头名指定输出列顺序）。
    // 缺省/null → 不指定顺序；出现则必须是数组，且每项是非空字符串。
    QJsonValue colOrderVal = expObj.value(QStringLiteral("columnOrder"));
    if (!colOrderVal.isUndefined() && !colOrderVal.isNull()) {
        if (!colOrderVal.isArray()) {
            if (err)
                *err = QStringLiteral("export.columnOrder must be an array of strings");
            return false;
        }
        QJsonArray colOrderArr = colOrderVal.toArray();
        for (int i = 0; i < colOrderArr.size(); ++i) {
            QString s = colOrderArr[i].toString();
            if (s.isEmpty()) {  // 用下标 i 拼错误信息，便于用户定位是第几项
                if (err)
                    *err = QStringLiteral("export.columnOrder[") + QString::number(i) +
                           QStringLiteral("] must be a non-empty string");
                return false;
            }
            out->exportSpec.columnOrder.append(s);
        }
        // 注意：columnOrder 与原生 sql 互斥、以及里面的表头是否「已知/不重复」等更深校验
        // （关联 E_EXPORT_UNKNOWN_HEADER / E_EXPORT_DUPLICATE_ORDER / E_EXPORT_ORDER_WITH_RAW_SQL）
        // 由后续 ProfileValidator 结合实际表头集合判定，不在此 loader 内做。
    }

    // add-export-reverse-lookup 2.5：拒绝已废弃的 export.reverseLookups / export.exportLookups。
    // 反向查找现在统一用「路由级 lookups[] + exportRoundtrip / exportOnMissing」表达，不再有
    // export 级的独立配置——出现旧字段即给出迁移提示。
    if (expObj.contains(QStringLiteral("reverseLookups")) ||
        expObj.contains(QStringLiteral("exportLookups"))) {
        if (err)
            *err = QStringLiteral(
                "export.reverseLookups / export.exportLookups are not supported; "
                "use the route-level lookups[] array with exportRoundtrip / exportOnMissing");
        return false;
    }

    // (5) profile 级时间格式槽 ─────────────────────────────────────
    // add-time-format-profile：作为各列的「默认时间格式」，列级同名槽会逐侧覆盖它
    //   （合并规则见 ProfileSpec.h::effectiveTemporalFor）。ownerLabel 用 "profile" 以标明层级。
    if (!readTemporalSlot(o.value(QStringLiteral("dateFormat")), TemporalSlotKind::Date,
                          QStringLiteral("profile"), &out->dateFormat, err))
        return false;
    if (!readTemporalSlot(o.value(QStringLiteral("datetimeFormat")), TemporalSlotKind::DateTime,
                          QStringLiteral("profile"), &out->datetimeFormat, err))
        return false;
    if (!readTemporalSlot(o.value(QStringLiteral("timeFormat")), TemporalSlotKind::Time,
                          QStringLiteral("profile"), &out->timeFormat, err))
        return false;

    // (6) 信息级诊断：列同时声明了 dateFormat 槽 与 date:fmt 校验器。
    // 【翻译保留原注释】按规格 "Compatibility with `date:fmt` validator"，dateFormat 胜出；
    //   该校验器退化为「直通(pass-through)」（即不再实际做日期校验）。这不是错误，只记一条提示。
    // walkRoutes：遍历一组路由的所有列，命中上述情形就往 loadWarnings 追加诊断。
    //   classCtx 非空时表示在混合模式的某类别下（用于把上下文拼进诊断信息，便于定位）。
    auto walkRoutes = [&out](const QVector<RouteSpec>& routes, const QString& classCtx) {
        for (const RouteSpec& r : routes) {
            for (const ColumnSpec& c : r.columns) {
                if (!c.dateFormat.declared)
                    continue;  // 没声明 dateFormat 槽，无冲突可言
                for (const QString& t : c.validatorTokens) {
                    if (t.startsWith(QStringLiteral("date:"))) {
                        // 拼装定位上下文："route 'x'" 或 "class 'A' route 'x'"。
                        QString ctx = classCtx.isEmpty()
                                          ? QStringLiteral("route '") + r.table + '\''
                                          : QStringLiteral("class '") + classCtx +
                                                QStringLiteral("' route '") + r.table + '\'';
                        out->loadWarnings.append(
                            ctx + QStringLiteral(" column '") + c.dbColumn +
                            QStringLiteral("': dateFormat overrides validator '") + t +
                            QStringLiteral("'; validator becomes pass-through"));
                        break;  // 一列只提示一次（找到首个 date: 校验器即可）
                    }
                }
            }
        }
    };
    // 按 mode 决定遍历 classes 还是 routes。
    if (out->mode == ProfileMode::Mixed) {
        for (const auto& cls : out->classes)
            walkRoutes(cls.routes, cls.id);
    } else {
        walkRoutes(out->routes, QString());
    }

    // (7) 后置跨字段校验：每列「至多一个时间槽」「type×format 自洽」「epochSec 槽位限制」。
    // 这些校验放在最后，是因为它们需要「列级 + profile 级合并后的有效规格」(effectiveTemporalFor)，
    // 而 profile 级槽要等上面第 (5) 步读完才齐备。validateColumn 对「一列」做这三类检查：
    auto validateColumn = [&](const ColumnSpec& col, const QString& colCtx) -> bool {
        // 2.5：一列至多声明 dateFormat / datetimeFormat / timeFormat 三者之一（互斥）。
        int declaredCount = (col.dateFormat.declared ? 1 : 0) +
                            (col.datetimeFormat.declared ? 1 : 0) +
                            (col.timeFormat.declared ? 1 : 0);
        if (declaredCount > 1) {
            if (err)
                *err = colCtx + QStringLiteral(
                                    ": column may declare at most one of dateFormat,"
                                    " datetimeFormat, timeFormat");
            return false;
        }

        // 2.6-2.8：逐槽种类检查「合并后有效规格」的两侧自洽性。
        static const TemporalSlotKind allKinds[] = {
            TemporalSlotKind::Date, TemporalSlotKind::DateTime, TemporalSlotKind::Time};
        for (TemporalSlotKind k : allKinds) {
            // 把列级槽与 profile 级同名槽按「侧」合并，得到该列在此槽种类下的最终生效规格。
            TemporalFormatSpec eff = effectiveTemporalFor(k, col, *out);
            if (!eff.declared)
                continue;  // 此槽种类对本列未生效，跳过

            // checkSide：校验「单侧」(excel / db) 的 type 与 format 是否自洽。
            auto checkSide = [&](const TemporalSideSpec& side, const QString& sideName,
                                 TemporalSlotKind slotKind) -> bool {
                if (!side.declared)
                    return true;  // 该侧未声明，无可校验

                // 2.7：epochSec 只允许出现在 datetimeFormat 的 db 侧
                //   （纪元秒是「日期+时间」概念，且只在写库一侧用整数存储；日期/时间槽或 excel
                //   侧都不合理）。
                if (side.type == TemporalPhysType::EpochSec) {
                    if (slotKind != TemporalSlotKind::DateTime ||
                        sideName != QStringLiteral("db")) {
                        if (err)
                            *err = colCtx + QStringLiteral(": ") +
                                   QLatin1String(slotKindName(slotKind)) + QStringLiteral(".") +
                                   sideName +
                                   QStringLiteral(
                                       ".type=epochSec is only allowed on datetimeFormat.db");
                        return false;
                    }
                }

                // 2.6：type=string 必须配非空 format（字符串表示离不开格式串）。
                if (side.type == TemporalPhysType::String && side.format.isEmpty()) {
                    if (err)
                        *err = colCtx + QStringLiteral(": ") +
                               QLatin1String(slotKindName(slotKind)) + QStringLiteral(".") +
                               sideName +
                               QStringLiteral(".type=string requires a non-empty format");
                    return false;
                }

                // 2.6：type=epochSec 必须没有 format（纪元秒是数值，无格式串可言）。
                if (side.type == TemporalPhysType::EpochSec && !side.format.isEmpty()) {
                    if (err)
                        *err = colCtx + QStringLiteral(": ") +
                               QLatin1String(slotKindName(slotKind)) + QStringLiteral(".") +
                               sideName +
                               QStringLiteral(".type=epochSec must have no format (got '") +
                               side.format + QStringLiteral("')");
                    return false;
                }

                return true;
            };

            // 两侧都要过 checkSide。
            if (!checkSide(eff.excel, QStringLiteral("excel"), k))
                return false;
            if (!checkSide(eff.db, QStringLiteral("db"), k))
                return false;
        }
        return true;
    };

    // validateRouteColumns：对一组路由里的每一列调用 validateColumn，并为其拼好定位上下文。
    auto validateRouteColumns = [&](const QVector<RouteSpec>& routes,
                                    const QString& classCtx) -> bool {
        for (const RouteSpec& r : routes) {
            for (const ColumnSpec& c : r.columns) {
                // colCtx 形如 "route 'x' column 'y'" 或 "class 'A' route 'x' column 'y'"。
                QString colCtx =
                    (classCtx.isEmpty() ? QStringLiteral("route '") + r.table + '\''
                                        : QStringLiteral("class '") + classCtx +
                                              QStringLiteral("' route '") + r.table + '\'') +
                    QStringLiteral(" column '") + c.dbColumn + '\'';
                if (!validateColumn(c, colCtx))
                    return false;
            }
        }
        return true;
    };

    // 按 mode 对所有列跑后置校验。
    if (out->mode == ProfileMode::Mixed) {
        for (const auto& cls : out->classes) {
            if (!validateRouteColumns(cls.routes, cls.id))
                return false;
        }
    } else {
        if (!validateRouteColumns(out->routes, QString()))
            return false;
    }

    return true;  // 全部解析与校验通过：*out 已是一份完整、自洽的内存模型
}

}  // namespace dbridge::detail
