// ============================================================================
// ValidatorChain.cpp — ValidatorChain 的实现：编译 token 串 + 逐环跑链
// ============================================================================
//
// 本文件实现 ValidatorChain.h 声明的两件事：
//   1) compile() —— 把校验 token 串翻译成 ValidatorFn 序列（含「类型 token 互斥」检查）。
//   2) run()     —— 对一个值顺序执行各环，前一环输出喂后一环输入，短路于首个失败。
//
// 关键不直观处见下方 isTypeNormalizingToken 与 compile 内的互斥检查注释。
// ============================================================================

#include "ValidatorChain.h"

#include "dbridge/Errors.h"

#include "Validators.h"

namespace dbridge::detail {

// isTypeNormalizingToken —— 判断一个 token 是否属于「会把值归一化成某种类型」的校验器。
//
// 为什么要识别这一类：这类校验器不仅校验，还会改写 out 的「类型」——
//   · "int" / "int>=N" 把文本转成 qlonglong；
//   · "decimal"        把文本转成 double；
//   · "date:fmt"       把文本转成 QDate。
// 同一列若同时挂两个这种 token（如既要 int 又要 decimal），它们对「最终类型」的诉求
// 互相冲突、无法同时满足，属于配置自相矛盾。故下方 compile 会拒绝「多于一个」的情形。
// 注意：notNull/len<=/len>=/regex/enum 只校验不改类型，不在此列，可与一个类型 token 共存。
static bool isTypeNormalizingToken(const QString& token) {
    return token == QStringLiteral("int") || token.startsWith(QStringLiteral("int>=")) ||
           token == QStringLiteral("decimal") || token.startsWith(QStringLiteral("date:"));
}

// compile —— 把 token 串编译为 fns_。详见头文件函数注释。
bool ValidatorChain::compile(const QStringList& tokens, QString* err) {
    fns_.clear();  // 重置：允许同一对象被重复 compile（也保证失败时不残留半截链）

    // ── 互斥检查：一列最多只能有一个「类型归一化」token（规格 §5.7）──────────────
    // 先数一遍 typeTokenCount；>1 即判为配置冲突，直接报 E_PROFILE_PARSE 失败。
    // 之所以在压入任何 ValidatorFn 之前先查，是为了「要么整条链编译成功、要么干净失败」。
    int typeTokenCount = 0;
    for (const auto& token : tokens) {
        if (isTypeNormalizingToken(token))
            typeTokenCount++;
    }
    if (typeTokenCount > 1) {
        if (err)
            *err = QString::fromLatin1(err::E_PROFILE_PARSE) +
                   QStringLiteral(": multiple conflicting type tokens in validator chain");
        return false;
    }

    // ── 逐个 token 编译为 ValidatorFn 并按原顺序压入链 ───────────────────────────
    // 顺序保留很重要：run 时按入链顺序执行，token 的书写次序即校验/转换的先后次序。
    for (const auto& token : tokens) {
        ValidatorFn fn = compileToken(token, err);  // 工厂分发（见 Validators.cpp）
        if (!fn)  // 空函数对象 = 该 token 非法（err 已填）
            return false;
        fns_.push_back(std::move(fn));  // move：转移闭包所有权，避免拷贝其捕获状态（如正则）
    }
    return true;
}

// run —— 逐环执行：current 在链上「流动」，被每一环读入并产出 next。详见头文件。
bool ValidatorChain::run(const QVariant& in, QVariant* outVal, QString* errCode,
                         QString* msg) const {
    QVariant current = in;  // 链上流动的「当前值」，初值为原始输入
    for (const auto& fn : fns_) {
        QVariant next;  // 本环的规整输出（成功时成为下一环的输入）
        if (!fn(current, &next, errCode, msg)) {
            // 短路：任一环失败立即停止。契约——失败时把原始 in 写回 outVal，
            // 绝不交付任何环产出的半成品值（防止坏值污染下游 payload）。
            *outVal = in;  // return original on error
            return false;
        }
        current = next;  // 通过：把本环输出接力给下一环
    }
    *outVal = current;  // 全链通过：交付最后一环的规整值
    return true;
}

}  // namespace dbridge::detail
