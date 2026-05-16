#include <QDate>
#include <QtTest>

#include "validation/ValidatorChain.h"

using namespace dbridge::detail;

class TstValidatorChain : public QObject {
    Q_OBJECT

   private slots:
    void testNotNull_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"notNull"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("hello")), &out, &code, &msg));
    }

    void testNotNull_fail_null() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"notNull"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(), &out, &code, &msg));
        QCOMPARE(code, QStringLiteral("E_VALIDATE_NULL"));
    }

    void testNotNull_fail_empty() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"notNull"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("")), &out, &code, &msg));
        QCOMPARE(code, QStringLiteral("E_VALIDATE_NULL"));
    }

    void testLenLe_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"len<=5"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("hello")), &out, &code, &msg));
    }

    void testLenLe_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"len<=3"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("hello")), &out, &code, &msg));
        QCOMPARE(code, QStringLiteral("E_VALIDATE_TYPE"));
    }

    void testInt_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"int"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("42")), &out, &code, &msg));
        QCOMPARE(out.toLongLong(), 42LL);
    }

    void testInt_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"int"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("abc")), &out, &code, &msg));
        QCOMPARE(code, QStringLiteral("E_VALIDATE_TYPE"));
    }

    void testIntGe_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"int>=1"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("5")), &out, &code, &msg));
    }

    void testIntGe_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"int>=10"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("5")), &out, &code, &msg));
    }

    void testDecimal_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"decimal"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("3.14")), &out, &code, &msg));
        QCOMPARE(out.toDouble(), 3.14);
    }

    void testDecimal_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"decimal"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("abc")), &out, &code, &msg));
    }

    void testDate_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"date:yyyy-MM-dd"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("2024-01-15")), &out, &code, &msg));
    }

    void testDate_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"date:yyyy-MM-dd"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("not-a-date")), &out, &code, &msg));
    }

    void testRegex_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"regex:^[0-9]+$"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("12345")), &out, &code, &msg));
    }

    void testRegex_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"regex:^[0-9]+$"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("abc")), &out, &code, &msg));
        QCOMPARE(code, QStringLiteral("E_VALIDATE_REGEX"));
    }

    void testEnum_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"enum:A,B,C"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("B")), &out, &code, &msg));
    }

    void testEnum_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"enum:A,B,C"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("D")), &out, &code, &msg));
    }

    void testNullPassesNonNotNull() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"int"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(), &out, &code, &msg));  // null passes int check
    }

    void testUnknownToken() {
        ValidatorChain chain;
        QString err;
        QVERIFY(!chain.compile({"unknownToken"}, &err));
        QVERIFY(!err.isEmpty());
    }
};

QTEST_MAIN(TstValidatorChain)
#include "tst_validator_chain.moc"
