// ============================================================================
// tst_export_helpers.cpp — ExportHelpers::reorderHeaders() 的单元测试
// ============================================================================
//
// 【被测对象是什么】
//   reorderHeaders(natural, columnOrder) 是导出（Export）链路里的一个纯函数小工具：
//   导出 Excel 时，列的「自然顺序」（natural，通常来自表结构 / 查询结果的列序）需要按
//   用户配置的「期望列序」（columnOrder）重排。本函数就回答一个问题——「最终表头应按
//   什么顺序排列」。
//
// 【被测的核心契约（这组测试要钉死的不变量）】
//   1) columnOrder 为空 → 原样返回 natural（不重排）。
//   2) columnOrder 完整覆盖 natural → 完全按 columnOrder 排。
//   3) columnOrder 只列了部分列 → 已列出的按 columnOrder 在前，未列出的按 natural 中
//      的「相对顺序」追加在后（稳定排序，不打乱未列出列彼此的次序）。
//   4) columnOrder 含 natural 里没有的列 → 该列仍出现在结果里（函数自身「健壮」，把
//      非法列的拒绝交给上游 validator 层，而非在此处静默丢弃）。
//   5) 各种空输入的边界行为（空 natural、空 columnOrder、两者皆空）。
//
// 【为什么用 QtTest】private slots 里的每个无参函数都是一个独立测试用例；QTEST_MAIN
//   会自动发现并逐个运行。QCOMPARE(actual, expected) 不等则失败并打印两侧值；
//   QVERIFY(cond) 断言布尔条件。
//
// 【命名空间】reorderHeaders 在 dbridge::detail（库内部实现细节），故 using 之。
// ============================================================================
#include <QtTest>

#include "service/ExportHelpers.h"

using namespace dbridge::detail;

class TstExportHelpers : public QObject {
    Q_OBJECT

   private slots:
    // 用例①：columnOrder 为空 → 不重排，原样返回 natural。
    // GIVEN 自然列序 A,B,C 且未配置任何期望列序；
    // WHEN  调用 reorderHeaders(natural, {})；
    // THEN  结果应与 natural 完全一致（空配置 = 不干预）。
    void testEmptyColumnOrderReturnsNatural() {
        QStringList natural;
        natural << QStringLiteral("A") << QStringLiteral("B") << QStringLiteral("C");
        QCOMPARE(reorderHeaders(natural, {}), natural);
    }

    // 用例②：columnOrder 完整覆盖 natural（同一集合、不同顺序）→ 完全按 columnOrder 排。
    // GIVEN natural=A,B,C，期望列序=C,A,B（列集合相同，仅顺序不同）；
    // WHEN  重排；
    // THEN  结果就是 columnOrder 本身（每一列都被显式安排了位置，无尾部追加）。
    void testFullReorder() {
        QStringList natural;
        natural << QStringLiteral("A") << QStringLiteral("B") << QStringLiteral("C");
        QStringList order;
        order << QStringLiteral("C") << QStringLiteral("A") << QStringLiteral("B");
        QCOMPARE(reorderHeaders(natural, order), order);
    }

    // 用例③：部分重排——只列出 B、C，未列出的 A 追加到末尾。验证「已列出在前、未列出在后」契约。
    // GIVEN natural=A,B,C，期望列序只给了 C,B（漏掉 A）；
    // WHEN  重排；
    // THEN  C,B 按配置在前，未被点名的 A 自动落在最后 → C,B,A。
    void testPartialReorderListedFirst() {
        // columnOrder lists B and C only; A is unlisted → appended last
        // 期望列序只列出 B、C；A 未列出 → 被追加到最后。
        QStringList natural;
        natural << QStringLiteral("A") << QStringLiteral("B") << QStringLiteral("C");
        QStringList order;
        order << QStringLiteral("C") << QStringLiteral("B");

        QStringList expected;
        expected << QStringLiteral("C") << QStringLiteral("B") << QStringLiteral("A");
        QCOMPARE(reorderHeaders(natural, order), expected);
    }

    // 用例④：未列出的多列，彼此之间须保持 natural 中的相对顺序（稳定性）。这是契约③最严格的形态。
    // GIVEN natural=A,B,C,D,E，期望列序=D,B（只点名两列）；
    // WHEN  重排；
    // THEN  D,B 在前；剩下的 A,C,E 不被打乱、按它们在 natural 里的原相对序追加 → D,B,A,C,E。
    void testUnlistedPreservesNaturalRelativeOrder() {
        // natural: A B C D E; order: D B
        // result: D B [A C E in natural order]
        // 自然序 A B C D E；期望序 D B；结果 = D B 后接 [A C E（保持自然相对序）]。
        QStringList natural;
        natural << QStringLiteral("A") << QStringLiteral("B") << QStringLiteral("C")
                << QStringLiteral("D") << QStringLiteral("E");
        QStringList order;
        order << QStringLiteral("D") << QStringLiteral("B");

        QStringList expected;
        expected << QStringLiteral("D") << QStringLiteral("B") << QStringLiteral("A")
                 << QStringLiteral("C") << QStringLiteral("E");
        QCOMPARE(reorderHeaders(natural, order), expected);
    }

    // 用例⑤：columnOrder 恰好是 natural 的全排列（同集合不同序）→ 结果即 columnOrder，无尾部追加。
    //   与用例②同型，换一组列名（X,Y,Z）再确认一次「全覆盖时无残余追加」的边界。
    void testColumnOrderExactlyNatural() {
        // same set, different order — all listed, no suffix
        // 同一集合、不同顺序——全部列出，故无尾部追加。
        QStringList natural;
        natural << QStringLiteral("X") << QStringLiteral("Y") << QStringLiteral("Z");
        QStringList order;
        order << QStringLiteral("Z") << QStringLiteral("X") << QStringLiteral("Y");
        QCOMPARE(reorderHeaders(natural, order), order);
    }

    // 用例⑥：健壮性——columnOrder 含 natural 里不存在的列（C），函数不应崩溃也不私自丢弃。
    // GIVEN natural=A,B，期望序=C,A,B，其中 C 并不在自然列里（属「越界/非法」配置）；
    // WHEN  重排；
    // THEN  结果照搬 C,A,B —— 本工具职责单一，只管排序；「拒绝非法列」是上游 validator 的事，
    //       本函数保持健壮、可预测，不在此处吞掉数据。
    void testColumnOrderSupersetIsRobust() {
        // columnOrder contains an entry not in natural — it appears in result
        // (validator layer is expected to reject this; helper itself is robust)
        // 期望列序里有个 natural 中不存在的列 → 它仍出现在结果中
        // （拒绝这种非法配置是 validator 层的职责；本工具自身保持健壮、不擅自删列）。
        QStringList natural;
        natural << QStringLiteral("A") << QStringLiteral("B");
        QStringList order;
        order << QStringLiteral("C") << QStringLiteral("A") << QStringLiteral("B");

        QStringList expected;
        expected << QStringLiteral("C") << QStringLiteral("A") << QStringLiteral("B");
        QCOMPARE(reorderHeaders(natural, order), expected);
    }

    // 用例⑦：边界——两个入参都为空 → 结果为空列表（不应抛错或返回非空）。
    void testEmptyNaturalEmptyColumnOrder() {
        QVERIFY(reorderHeaders({}, {}).isEmpty());
    }

    // 用例⑧：边界——natural 为空但 columnOrder 非空 → 结果含 columnOrder 的项。
    //   即便没有任何「自然列」，已显式配置的列序仍被尊重（与用例⑥的健壮性一脉相承）。
    void testEmptyNaturalWithColumnOrder() {
        QStringList order;
        order << QStringLiteral("A");
        // result contains columnOrder entries even if natural is empty
        // 即使 natural 为空，结果中仍包含 columnOrder 列出的项。
        QCOMPARE(reorderHeaders({}, order), order);
    }
};

QTEST_MAIN(TstExportHelpers)
#include "tst_export_helpers.moc"
