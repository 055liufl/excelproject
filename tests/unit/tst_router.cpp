#include <QtTest>

#include "mapping/Router.h"
#include "profile/ProfileSpec.h"

// ============================================================================
// tst_router.cpp — Router（类别路由器）单元测试
// ============================================================================
//
// 【被测对象 Router 是什么】
//   在「混合模式（Mixed）」导入里，同一张工作表的每一行需要按某个「判别列
//   （discriminator）」的取值，被分派（route）到不同的「类别（ClassSpec）」去做后续
//   解析/写库。Router 就是这个分派器：给定判别列的值（QVariant），返回它应归属的
//   ClassSpec*（命中）或 nullptr（无匹配）。各类别的匹配规则由 ProfileSpec.classes
//   中每个 ClassSpec 的 matchEquals 字段定义（精确相等匹配）。
//
// 【这些测试在验证什么契约】
//   · 精确匹配：判别值等于某类别的 matchEquals → 返回该类别（testMatchA/B）；
//   · 无匹配：判别值不等于任何 matchEquals → 返回 nullptr（testUnmatchedValue）；
//   · 空判别值：QVariant() 不应匹配任何类别（testNullDiscriminator）；
//   · 配置自检：两个类别 matchEquals 相同 → init() 必须失败（testDuplicateMatchEquals），
//     因为「同一个判别值映射到两个类别」会让路由产生歧义，属配置错误，需在 init 阶段拦截。
//
// 【测试基础设施】QtTest 框架：每个 `private slots:` 下的无参 void 方法即一个测试用例，
//   QTEST_MAIN 生成 main()。断言宏：QVERIFY(真)、QVERIFY(!假)、QCOMPARE(实际,期望)。
// ============================================================================

using namespace dbridge::detail;

class TstRouter : public QObject {
    Q_OBJECT

    // ── 夹具辅助：按 (类别id, 匹配值) 列表快速造一个 Mixed 模式的 ProfileSpec ──────
    // 做什么：构造一个判别列名为 "Type" 的混合模式 Profile，并按入参把每个
    //   (id, eq) 变成一个 matchEquals==eq 的 ClassSpec 追加进去。
    // 为什么需要：各测试只关心「类别集合」这一个变量，把 Profile 的样板字段（mode、
    //   discriminatorSource）固定在这里，使每个用例只需一行就能描述其场景，读起来更聚焦。
    // 参数：classes —— (类别id, 该类别精确匹配的判别值) 的列表。
    // 返回：填好的 ProfileSpec（值返回，副本开销可忽略）。
    ProfileSpec makeProfile(const QVector<QPair<QString, QString>>& classes) {
        ProfileSpec p;
        p.mode = ProfileMode::Mixed;
        p.discriminatorSource = QStringLiteral("Type");  // 判别列名（本测试统一用 "Type"）
        for (const auto& [id, eq] : classes) {           // 结构化绑定解开每个 pair
            ClassSpec cls;
            cls.id = id;           // 类别唯一 id
            cls.matchEquals = eq;  // 该类别的精确匹配值（判别值 == 它即命中本类别）
            p.classes.append(cls);
        }
        return p;
    }

   private slots:
    // ── testMatchA —— 判别值 "A" 命中 id 为 "A" 的类别 ───────────────────────────
    // GIVEN：三个类别 A/B/C，分别精确匹配 "A"/"B"/"C"。
    // WHEN ：用判别值 "A" 调 match()。
    // THEN ：返回非空且其 id == "A"（验证「精确匹配返回正确类别」这一核心契约）。
    void testMatchA() {
        auto profile = makeProfile({{"A", "A"}, {"B", "B"}, {"C", "C"}});
        Router router;
        QString err;
        QVERIFY(router.init(profile, &err));  // 合法配置：init 应成功
        const ClassSpec* cls = router.match(QVariant(QStringLiteral("A")));
        QVERIFY(cls);                            // 必须命中（非 nullptr）
        QCOMPARE(cls->id, QStringLiteral("A"));  // 且命中的正是 A 类
    }

    // ── testMatchB —— 同上，验证命中的是「对应」的类别而非总返回第一个 ──────────
    // 与 testMatchA 互为对照：换判别值 "B" 应得到 id=="B"，证明 match 真按值区分类别。
    void testMatchB() {
        auto profile = makeProfile({{"A", "A"}, {"B", "B"}, {"C", "C"}});
        Router router;
        QString err;
        QVERIFY(router.init(profile, &err));
        const ClassSpec* cls = router.match(QVariant(QStringLiteral("B")));
        QVERIFY(cls);
        QCOMPARE(cls->id, QStringLiteral("B"));
    }

    // ── testUnmatchedValue —— 判别值不属于任何类别 → 返回 nullptr ────────────────
    // GIVEN：只有 A、B 两类。 WHEN：判别值 "Z"。 THEN：match 返回 nullptr。
    // 业务含义：路由器不会「兜底」乱归类；无匹配由调用方按未路由行处理（如报错/跳过）。
    void testUnmatchedValue() {
        auto profile = makeProfile({{"A", "A"}, {"B", "B"}});
        Router router;
        QString err;
        QVERIFY(router.init(profile, &err));
        const ClassSpec* cls = router.match(QVariant(QStringLiteral("Z")));
        QVERIFY(!cls);  // 无匹配
    }

    // ── testNullDiscriminator —— 空判别值不匹配任何类别 ─────────────────────────
    // 边界用例：QVariant()（无效/空值，对应「该行判别列为空」）必须返回 nullptr，
    // 而不能与某个 matchEquals 巧合相等。保证空判别值被当作「无法路由」。
    void testNullDiscriminator() {
        auto profile = makeProfile({{"A", "A"}});
        Router router;
        QString err;
        QVERIFY(router.init(profile, &err));
        const ClassSpec* cls = router.match(QVariant());  // 空 QVariant
        QVERIFY(!cls);
    }

    // ── testDuplicateMatchEquals —— 重复匹配值是配置错误，init 必须拒绝 ──────────
    // GIVEN：A、B 两类的 matchEquals 都是 "A"。
    // WHEN ：init()。
    // THEN ：init 返回 false（拒绝歧义配置）。
    // 不变量：同一判别值不能映射到多个类别，否则 match("A") 无法确定唯一目标。把校验
    //   前移到 init，能在导入开始前就暴露 Profile 写错，而不是运行时才产生不确定行为。
    void testDuplicateMatchEquals() {
        auto profile =
            makeProfile({{"A", "A"}, {"B", "A"}});  // both match "A"（两类都匹配 "A"，冲突）
        Router router;
        QString err;
        QVERIFY(!router.init(profile, &err));  // 必须失败
    }
};

QTEST_MAIN(TstRouter)
#include "tst_router.moc"
