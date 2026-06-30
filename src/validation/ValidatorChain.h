#pragma once
#include <QStringList>
#include <QVariant>

#include <functional>
#include <vector>

// ============================================================================
// ValidatorChain.h — 单元格校验的「可执行单元」与「校验链」
// ============================================================================
//
// 【单一职责】
//   定义两样东西：
//     1) ValidatorFn —— 「一个校验器」的统一函数签名（类型别名）。所有内建校验器
//        （非空 / 整数 / 长度 / 正则 / 枚举 / 日期……，见 Validators.h）都被做成
//        这一种函数对象，从而能被同质地存储、组合、串联执行。
//     2) ValidatorChain —— 把若干 ValidatorFn 串成「一条链」，对同一个单元格值
//        依次执行（前一个的输出喂给后一个的输入），任一环失败即整链失败。
//
// 【在 ETL 流水线中的位置】
//   Profile 里每列声明一串校验 token（如 "notNull"/"int"/"len<=32"）。
//     · Mapper::compileValidators() 调 ValidatorChain::compile()，把 token 串
//       编译成一条 ValidatorChain，并按 (routeKey, dbColumn) 存进 ValidatorMap；
//     · Mapper::map() 逐行调 ValidatorChain::run()，跑链得到「规整后的值」或错误码。
//   即：本类是「列级校验规则」从配置文本到可执行逻辑的载体。
//
// 【协作者】
//   · Validators.h/.cpp —— 提供各 makeXxx() 工厂与 compileToken()，产出 ValidatorFn。
//   · Mapper.cpp        —— 编译期建链、运行期逐行跑链的唯一调用方。
//   · Errors.h          —— 失败时回报的错误码来源（E_VALIDATE_* / E_PROFILE_PARSE）。
//
// 【命名空间】dbridge::detail —— 库内部实现细节，不对外暴露。
// ============================================================================

namespace dbridge::detail {

// ValidatorFn —— 「一个校验器」的统一签名。
//
// 语义约定（所有内建校验器都遵守，便于串链）：
//   · in      —— 待校验的输入值（上一环的输出，或链首的原始单元格值）。
//   · out     —— 出参：本环「规整 / 归一化」后的值。校验通过时写入；某些校验器
//                会顺手做类型转换（如 "int" 把文本 "42" 转成 qlonglong 42），
//                这个转换结果就经由 out 向下一环传递。
//   · errCode —— 出参：失败时填错误码字符串（取值见 Errors.h，如 E_VALIDATE_NULL）。
//   · msg     —— 出参：失败时填人类可读的诊断描述。
//   返回 —— true=通过（out 有效）；false=失败（errCode/msg 已填，out 内容不应被采信）。
//
// 为什么用 std::function 而非裸函数指针：内建校验器多为「带参数的闭包」
//   （如 makeLenLe(n) 捕获上限 n、makeRegex 捕获编译好的正则），需要捕获状态，
//   只有 std::function 能同质地装下这些不同的闭包。
using ValidatorFn =
    std::function<bool(const QVariant& in, QVariant* out, QString* errCode, QString* msg)>;

// ValidatorChain —— 把多个 ValidatorFn 串成一条「逐环执行」的校验链。
//   设计要点：编译一次（compile）、运行多次（run）。一列的链在导入开始时编译好，
//   随后对该列的每一行单元格反复 run，避免每行重新解析 token / 重编译正则。
class ValidatorChain {
   public:
    // compile —— 把一串校验 token 编译为本链内部的 ValidatorFn 序列。
    //
    // 做什么：先做「类型归一化 token 互斥」检查（见 .cpp，int/decimal/date: 等彼此冲突，
    //         一列至多一个），再逐个 token 调 compileToken() 生成 ValidatorFn 压入链。
    // 为什么：把易变的配置文本一次性翻译成固定的可执行闭包，运行期零解析开销。
    // 参数：
    //   tokens —— 该列的校验 token 列表（已剥离交由时间转换层处理的 date:* token，
    //             剥离逻辑在 Mapper::compileValidators，不在本类）。
    //   err    —— 出参：任一 token 非法 / 多个类型 token 冲突时，写入诊断文本。
    // 返回：全部编译成功 true；否则 false（err 已填）。失败前已编译的部分会被清空。
    // 副作用：重置并填充内部 fns_。无 I/O。
    // 错误模式：归类为 E_PROFILE_PARSE 级别（配置阶段错误，应使整个 Profile 加载失败）。
    bool compile(const QStringList& tokens, QString* err);

    // run —— 对一个值跑完整条链；「逐环串联」：前一环的输出作下一环的输入。
    //
    // 做什么：从 in 出发依次执行每个 ValidatorFn，任一环返回 false 即停（短路）。
    // 为什么短路：第一处错误已足以判定该单元格无效，无需继续；也避免在坏值上做后续转换。
    // 参数：
    //   in      —— 原始单元格值。
    //   outVal  —— 出参：全链通过时，写入「最后一环规整后的值」；任一环失败时，写回原始 in
    //              （契约：失败不得交付半成品值，避免污染下游 payload，见 Mapper.cpp 用法）。
    //   errCode —— 出参：失败时由出错那一环填写的错误码（E_VALIDATE_*）。
    //   msg     —— 出参：失败时的人类可读描述。
    // 返回：全链通过 true；任一环失败 false。
    // 副作用：无（const 方法，不改成员）。可对不同值并发调用。
    // 复杂度：O(链长)，每环一次函数调用。
    bool run(const QVariant& in, QVariant* outVal, QString* errCode, QString* msg) const;

    // isEmpty —— 链中是否没有任何校验器（即该列无需校验）。
    //   Mapper 据此跳过对空链的 run（微优化，且语义上「无校验=恒通过」）。
    bool isEmpty() const {
        return fns_.empty();
    }

   private:
    std::vector<ValidatorFn> fns_;  // 编译产物：按 token 顺序排列的校验器序列，run 时顺序执行
};

}  // namespace dbridge::detail
