#pragma once
#include <QString>

#include "ValidatorChain.h"

// ============================================================================
// Validators.h — 内建校验器的「工厂」声明（token → ValidatorFn）
// ============================================================================
//
// 【单一职责】
//   声明一组工厂函数：每个 makeXxx() 产出一个具体的内建校验器（ValidatorFn，
//   签名见 ValidatorChain.h），以及一个总分发函数 compileToken()——把一段
//   校验 token 字符串（如 "notNull"、"len<=32"、"regex:^...$"）翻译成对应的
//   ValidatorFn。本文件只「造」单个校验器；把它们串成链是 ValidatorChain 的事。
//
// 【在 ETL 流水线中的位置】
//   Profile（列级 validatorTokens）
//     → ValidatorChain::compile() 逐 token 调 compileToken()（本文件）
//       → 得到 ValidatorFn 序列 → 串成校验链
//         → Mapper::map() 逐行运行链校验单元格。
//   即本文件处于「配置 token → 可执行校验逻辑」翻译链条的最底层（单个校验器粒度）。
//
// 【协作者】
//   · ValidatorChain.h —— 提供 ValidatorFn 类型；compile() 是 compileToken() 的调用方。
//   · Errors.h         —— 各校验器失败时回报的错误码来源（E_VALIDATE_*）。
//   · Validators.cpp   —— 本文件所有声明的实现（各闭包的具体校验逻辑）。
//
// 【命名空间】dbridge::detail —— 库内部实现细节。
// ============================================================================

namespace dbridge::detail {

// ── 内建校验器工厂 ───────────────────────────────────────────────────────────
// 约定：每个工厂返回一个 ValidatorFn；若构造过程本身失败（仅 makeRegex 可能，
//       因正则模式串可能非法），返回「空 std::function」并通过 *err 反馈原因。
// 各校验器对 null/空值的处理见 .cpp（多数「放行 null，交由 notNull 专管必填」）。

ValidatorFn makeNotNull();     // 非空：值为 null 或空串则失败（E_VALIDATE_NULL）
ValidatorFn makeLenLe(int n);  // 长度上限：字符串长度 > n 失败（E_VALIDATE_TYPE）
ValidatorFn makeLenGe(int n);  // 长度下限：字符串长度 < n 失败（E_VALIDATE_TYPE）
ValidatorFn makeInt();  // 整数：不能转成整数则失败；成功则把 out 归一化为 qlonglong
ValidatorFn makeIntGe(long long n);  // 整数且 >= n：先转整数，再判下限（E_VALIDATE_TYPE）
ValidatorFn makeDecimal();  // 小数：不能转 double 则失败；成功则 out 归一化为 double
ValidatorFn makeDate(const QString& fmt);  // 日期：按 fmt 解析（遗留路径，见 Mapper 时间转换说明）
ValidatorFn makeRegex(const QString& pattern,
                      QString* err);  // 正则：整串匹配；模式非法→空函数+*err
ValidatorFn makeEnum(const QString& valuesStr);  // 枚举：值须在逗号分隔的允许集合内

// compileToken —— 把单个 token 字符串分发到对应工厂，得到 ValidatorFn。
//   做什么：识别 token 的前缀/字面量（"notNull"/"int"/"len<="/"regex:"…），调相应工厂。
//   返回：成功返回 ValidatorFn；token 未知或参数非法时返回空函数并设置 *err。
//   错误模式：空返回 → ValidatorChain::compile 失败 → 上游归为 E_PROFILE_PARSE。
ValidatorFn compileToken(const QString& token, QString* err);

}  // namespace dbridge::detail
