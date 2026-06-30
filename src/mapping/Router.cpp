// ============================================================================
// Router.cpp — Mixed 模式行分类器的实现
// ============================================================================
//
// 本文件实现 Router 的两件事：
//   init()  —— 拷贝判别列名与类别列表，建立 matchEquals → 类别下标 索引并校验判别值唯一。
//   match() —— 按判别值做 O(1) 哈希查找，返回命中的 ClassSpec 或 nullptr。
// ============================================================================

#include "Router.h"

#include "dbridge/Errors.h"

namespace dbridge::detail {

// init —— 见头文件。建立判别值索引，并拒绝重复的 matchEquals。
bool Router::init(const ProfileSpec& profile, QString* err) {
    discriminatorSource_ = profile.discriminatorSource;  // 记下判别列表头名
    classes_ = profile.classes;  // 拷贝类别集合（match 的指针将指向这里）
    matchToClassIdx_.clear();

    // 逐个类别登记 matchEquals → 下标；若发现两个类别用同一判别值，则无法唯一分派，报错。
    for (int i = 0; i < classes_.size(); ++i) {
        const auto& cls = classes_[i];
        if (matchToClassIdx_.contains(cls.matchEquals)) {
            if (err)
                *err = QStringLiteral("Duplicate matchEquals '") + cls.matchEquals +
                       QStringLiteral("' in mixed profile classes");
            return false;
        }
        matchToClassIdx_[cls.matchEquals] = i;
    }
    return true;
}

// match —— 见头文件。null 判别值或无匹配项均返回 nullptr。
const ClassSpec* Router::match(const QVariant& discriminator) const {
    if (discriminator.isNull())
        return nullptr;  // 判别值缺失 → 无法分类（上游通常报 E_ROUTE_UNMATCHED）
    QString val = discriminator.toString();  // 以文本形式做等值比对
    auto it = matchToClassIdx_.find(val);
    if (it == matchToClassIdx_.end())
        return nullptr;            // 没有任何类别的 matchEquals 等于该值 → 未命中
    return &classes_[it.value()];  // 命中：返回指向内部 classes_ 元素的指针
}

}  // namespace dbridge::detail
