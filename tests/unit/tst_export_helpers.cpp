#include <QtTest>

#include "service/ExportHelpers.h"

using namespace dbridge::detail;

class TstExportHelpers : public QObject {
    Q_OBJECT

   private slots:
    void testEmptyColumnOrderReturnsNatural() {
        QStringList natural;
        natural << QStringLiteral("A") << QStringLiteral("B") << QStringLiteral("C");
        QCOMPARE(reorderHeaders(natural, {}), natural);
    }

    void testFullReorder() {
        QStringList natural;
        natural << QStringLiteral("A") << QStringLiteral("B") << QStringLiteral("C");
        QStringList order;
        order << QStringLiteral("C") << QStringLiteral("A") << QStringLiteral("B");
        QCOMPARE(reorderHeaders(natural, order), order);
    }

    void testPartialReorderListedFirst() {
        // columnOrder lists B and C only; A is unlisted → appended last
        QStringList natural;
        natural << QStringLiteral("A") << QStringLiteral("B") << QStringLiteral("C");
        QStringList order;
        order << QStringLiteral("C") << QStringLiteral("B");

        QStringList expected;
        expected << QStringLiteral("C") << QStringLiteral("B") << QStringLiteral("A");
        QCOMPARE(reorderHeaders(natural, order), expected);
    }

    void testUnlistedPreservesNaturalRelativeOrder() {
        // natural: A B C D E; order: D B
        // result: D B [A C E in natural order]
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

    void testColumnOrderExactlyNatural() {
        // same set, different order — all listed, no suffix
        QStringList natural;
        natural << QStringLiteral("X") << QStringLiteral("Y") << QStringLiteral("Z");
        QStringList order;
        order << QStringLiteral("Z") << QStringLiteral("X") << QStringLiteral("Y");
        QCOMPARE(reorderHeaders(natural, order), order);
    }

    void testColumnOrderSupersetIsRobust() {
        // columnOrder contains an entry not in natural — it appears in result
        // (validator layer is expected to reject this; helper itself is robust)
        QStringList natural;
        natural << QStringLiteral("A") << QStringLiteral("B");
        QStringList order;
        order << QStringLiteral("C") << QStringLiteral("A") << QStringLiteral("B");

        QStringList expected;
        expected << QStringLiteral("C") << QStringLiteral("A") << QStringLiteral("B");
        QCOMPARE(reorderHeaders(natural, order), expected);
    }

    void testEmptyNaturalEmptyColumnOrder() {
        QVERIFY(reorderHeaders({}, {}).isEmpty());
    }

    void testEmptyNaturalWithColumnOrder() {
        QStringList order;
        order << QStringLiteral("A");
        // result contains columnOrder entries even if natural is empty
        QCOMPARE(reorderHeaders({}, order), order);
    }
};

QTEST_MAIN(TstExportHelpers)
#include "tst_export_helpers.moc"
