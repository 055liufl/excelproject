#include <QtTest>

#include "mapping/Router.h"
#include "profile/ProfileSpec.h"

using namespace dbridge::detail;

class TstRouter : public QObject {
    Q_OBJECT

    ProfileSpec makeProfile(const QVector<QPair<QString, QString>>& classes) {
        ProfileSpec p;
        p.mode = ProfileMode::Mixed;
        p.discriminatorSource = QStringLiteral("Type");
        for (const auto& [id, eq] : classes) {
            ClassSpec cls;
            cls.id = id;
            cls.matchEquals = eq;
            p.classes.append(cls);
        }
        return p;
    }

   private slots:
    void testMatchA() {
        auto profile = makeProfile({{"A", "A"}, {"B", "B"}, {"C", "C"}});
        Router router;
        QString err;
        QVERIFY(router.init(profile, &err));
        const ClassSpec* cls = router.match(QVariant(QStringLiteral("A")));
        QVERIFY(cls);
        QCOMPARE(cls->id, QStringLiteral("A"));
    }

    void testMatchB() {
        auto profile = makeProfile({{"A", "A"}, {"B", "B"}, {"C", "C"}});
        Router router;
        QString err;
        QVERIFY(router.init(profile, &err));
        const ClassSpec* cls = router.match(QVariant(QStringLiteral("B")));
        QVERIFY(cls);
        QCOMPARE(cls->id, QStringLiteral("B"));
    }

    void testUnmatchedValue() {
        auto profile = makeProfile({{"A", "A"}, {"B", "B"}});
        Router router;
        QString err;
        QVERIFY(router.init(profile, &err));
        const ClassSpec* cls = router.match(QVariant(QStringLiteral("Z")));
        QVERIFY(!cls);
    }

    void testNullDiscriminator() {
        auto profile = makeProfile({{"A", "A"}});
        Router router;
        QString err;
        QVERIFY(router.init(profile, &err));
        const ClassSpec* cls = router.match(QVariant());
        QVERIFY(!cls);
    }

    void testDuplicateMatchEquals() {
        auto profile = makeProfile({{"A", "A"}, {"B", "A"}});  // both match "A"
        Router router;
        QString err;
        QVERIFY(!router.init(profile, &err));
    }
};

QTEST_MAIN(TstRouter)
#include "tst_router.moc"
