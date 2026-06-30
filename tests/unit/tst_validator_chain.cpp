#include <QDate>
#include <QtTest>

#include "validation/ValidatorChain.h"

// ============================================================================
// tst_validator_chain.cpp — ValidatorChain（单元格校验链）的单元测试
// ============================================================================
//
// 【被测对象是什么】
//   ValidatorChain 是 ETL 导入流水线里的「单元格校验/转换器」：每一列可以配置一串
//   校验规则（token），如 "notNull"、"len<=5"、"int>=1"、"date:yyyy-MM-dd"、
//   "regex:^[0-9]+$"、"enum:A,B,C"。校验链先 compile(tokens) 把规则文本「编译」成
//   一串可执行的校验步骤；随后对每个单元格的值 run() 跑一遍，既做校验、也做「规范化
//   转换」（如把字符串 "42" 转成整数 42 写进出参 out）。
//
// 【两个核心 API（贯穿所有用例）】
//   · compile(QStringList tokens, QString* err)
//       —— 解析规则。返回 true=全部 token 合法可编译；false=有未知/非法 token（err 写原因）。
//   · run(QVariant in, QVariant* out, QString* code, QString* msg)
//       —— 对单个值执行校验链。返回 true=通过（out 为规范化后的值）；false=校验失败，
//          此时 code 给出机器可读错误码（如 E_VALIDATE_NULL / E_VALIDATE_TYPE /
//          E_VALIDATE_REGEX），msg 给人类可读说明。
//
// 【框架】Qt Test：每个 private slot 即一个测试用例，宏含义——
//   QVERIFY(cond)        断言 cond 为真；QVERIFY(!cond) 断言为假。
//   QCOMPARE(a, b)       断言 a==b（不等时打印两值，比 QVERIFY 更易排查）。
//   QTEST_MAIN(Class)    生成 main()，自动按字典序逐个跑 private slot。
//
// 【命名约定】*_pass = 「应当通过」的正路用例；*_fail = 「应当被拒」的反路用例。
//   每个 token 类型都配一对 pass/fail，确保「该放的放、该挡的挡」两侧边界都被钉住。
//
// 注意：这是测试文件，只增注释、不改任何断言或被测调用。
// ============================================================================

using namespace dbridge::detail;

class TstValidatorChain : public QObject {
    Q_OBJECT

   private slots:
    // notNull · 正路：非空字符串 "hello" 应当通过 notNull 校验。
    // GIVEN 规则 {notNull}；WHEN 传入非空值；THEN run() 返回 true（无错误码）。
    void testNotNull_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"notNull"}, &err));  // 规则可编译
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("hello")), &out, &code, &msg));  // 非空 → 通过
    }

    // notNull · 反路（值为 null）：QVariant() 即「无值/NULL」，应被 notNull 挡下。
    // THEN run() 返回 false，且错误码精确为 E_VALIDATE_NULL（区别于类型/格式错误）。
    void testNotNull_fail_null() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"notNull"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(), &out, &code, &msg));  // NULL → 被拒
        QCOMPARE(code, QStringLiteral("E_VALIDATE_NULL"));   // 错误码须为「空值」
    }

    // notNull · 反路（空字符串）：notNull 不仅挡 NULL，也把空串 "" 视为「等同于空」。
    // 这是业务约定——Excel 单元格留空常被读成空串而非真 NULL，二者都不应通过 notNull。
    void testNotNull_fail_empty() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"notNull"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("")), &out, &code, &msg));  // "" → 被拒
        QCOMPARE(code, QStringLiteral("E_VALIDATE_NULL"));  // 空串与 NULL 同码
    }

    // len<=N · 正路：长度恰好等于上界 N（"hello" 长 5，规则 len<=5）应通过（闭区间，含等号）。
    void testLenLe_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"len<=5"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("hello")), &out, &code, &msg));  // 5<=5 通过
    }

    // len<=N · 反路：长度超上界（"hello" 长 5 > 3）应被拒，错误码归类为 E_VALIDATE_TYPE
    //   （长度约束被视作「类型/格式」一类违例）。
    void testLenLe_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"len<=3"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("hello")), &out, &code, &msg));  // 5>3 被拒
        QCOMPARE(code, QStringLiteral("E_VALIDATE_TYPE"));
    }

    // int · 正路 + 规范化：字符串 "42" 应通过 int 校验，且 out 被转换为整数 42。
    // 关键断言 QCOMPARE(out.toLongLong(), 42LL)：验证校验链不仅「判合法」，还做了「类型转换」
    //   ——下游入库的是真正的整数，而非原始字符串。
    void testInt_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"int"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("42")), &out, &code, &msg));
        QCOMPARE(out.toLongLong(), 42LL);  // 规范化为整数 42
    }

    // int · 反路：非数字串 "abc" 无法转成整数，应被拒，错误码 E_VALIDATE_TYPE。
    void testInt_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"int"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("abc")), &out, &code, &msg));
        QCOMPARE(code, QStringLiteral("E_VALIDATE_TYPE"));
    }

    // int>=N · 正路：带下界的整数约束，"5" 满足 >=1 应通过（既校验是整数、又校验落在下界内）。
    void testIntGe_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"int>=1"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("5")), &out, &code, &msg));  // 5>=1 通过
    }

    // int>=N · 反路：值是合法整数但低于下界（5 < 10），应被拒（范围越界，非类型错误）。
    void testIntGe_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"int>=10"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("5")), &out, &code, &msg));  // 5<10 被拒
    }

    // decimal · 正路 + 规范化："3.14" 通过 decimal 校验，且 out 转为浮点 3.14。
    void testDecimal_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"decimal"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("3.14")), &out, &code, &msg));
        QCOMPARE(out.toDouble(), 3.14);  // 规范化为浮点数
    }

    // decimal · 反路：非数字串 "abc" 不能转浮点，应被拒。
    void testDecimal_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"decimal"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("abc")), &out, &code, &msg));
    }

    // date:fmt · 正路：按给定格式 yyyy-MM-dd 解析 "2024-01-15"，应通过（日期格式校验）。
    void testDate_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"date:yyyy-MM-dd"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("2024-01-15")), &out, &code, &msg));
    }

    // date:fmt · 反路：不符合日期格式的串 "not-a-date" 应被拒。
    void testDate_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"date:yyyy-MM-dd"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("not-a-date")), &out, &code, &msg));
    }

    // regex:pattern · 正路：纯数字串 "12345" 匹配 ^[0-9]+$，应通过。
    void testRegex_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"regex:^[0-9]+$"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("12345")), &out, &code, &msg));
    }

    // regex:pattern · 反路："abc" 不匹配 ^[0-9]+$ 应被拒，错误码精确为 E_VALIDATE_REGEX
    //   （区别于 TYPE/NULL，便于上层把「格式不符」单独报给用户）。
    void testRegex_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"regex:^[0-9]+$"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("abc")), &out, &code, &msg));
        QCOMPARE(code, QStringLiteral("E_VALIDATE_REGEX"));
    }

    // enum:a,b,c · 正路：值在白名单内（"B" ∈ {A,B,C}）应通过。
    void testEnum_pass() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"enum:A,B,C"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(QStringLiteral("B")), &out, &code, &msg));
    }

    // enum:a,b,c · 反路：值不在白名单内（"D" ∉ {A,B,C}）应被拒。
    void testEnum_fail() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"enum:A,B,C"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(!chain.run(QVariant(QStringLiteral("D")), &out, &code, &msg));
    }

    // 重要语义不变量：NULL 对「非 notNull」规则一律放行。
    //   GIVEN 只有 int 校验（没有 notNull）；WHEN 传入 NULL；THEN 通过。
    //   设计含义：类型/范围/格式类校验只约束「有值时的形态」；「是否允许为空」是 notNull 的
    //   专责。可空列即便配了 int 也不该因为「值缺失」而失败——否则无法表达「可空整数列」。
    void testNullPassesNonNotNull() {
        ValidatorChain chain;
        QString err;
        QVERIFY(chain.compile({"int"}, &err));
        QVariant out;
        QString code, msg;
        QVERIFY(chain.run(QVariant(), &out, &code, &msg));  // null passes int check（NULL 放行）
    }

    // 编译期防御：未知规则 token 必须在 compile 阶段就被拒（而非等到 run 才出错）。
    //   THEN compile() 返回 false，且 err 写入非空原因 —— 让配置错误「早失败、给提示」。
    void testUnknownToken() {
        ValidatorChain chain;
        QString err;
        QVERIFY(!chain.compile({"unknownToken"}, &err));  // 未知 token → 编译失败
        QVERIFY(!err.isEmpty());                          // 须给出可读原因
    }
};

QTEST_MAIN(TstValidatorChain)
#include "tst_validator_chain.moc"
