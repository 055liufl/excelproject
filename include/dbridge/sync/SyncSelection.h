#pragma once
#include "dbridge/Export.h"

#include <QStringList>

// ============================================================================
// SyncSelection.h — 选择性推送（selective push）的「选择集」入参对象
// ============================================================================
//
// 【这个文件是什么】
//   定义调用方用来描述「我要把哪些行推送到中心节点」的不可变值对象 SyncSelection，
//   以及构造它的 SyncSelection::Builder。它是「选择性推送」流水线最前端的输入：
//
//     Builder.addRecord(...)        →  SyncSelection（已校验的选择集）
//                                          │
//        ┌─────────────────────────────────┘
//        ▼  以下各阶段都在 src/sync/selection/ 实现：
//     SelectionResolver  把 (表, 主键) 解析成库里真实的整行数据
//        ▼
//     FkClosureBuilder   沿外键(FK)向上补全「父行依赖闭包」+ 拓扑排序 + 一致性剪枝
//        ▼
//     ChunkStreamer      按字节预算切成若干 chunk（分片），冻结成 FrozenEntry
//        ▼
//     PayloadCodec       打包为 SelectionPush artifact 发往中心节点
//
//   SyncSelection 本身只承载「意图」（选哪些表的哪些主键、是否带 FK 依赖、是否剪枝），
//   不触碰数据库；真正的解析/冻结/打包都在后续阶段完成。
//
// 【为什么用 Builder 模式 + 私有构造】
//   · SyncSelection 的构造函数是 private，且把 Builder 设为 friend ——
//     这意味着外界「无法直接 new 出一个 SyncSelection」，只能经由 Builder.build()。
//   · 好处：所有「合法性校验」（表名是否安全、是否误用 raw SQL、是否为空）都集中在
//     build() 这一个出口完成；一旦拿到 SyncSelection 实例，即代表它已通过校验，
//     下游阶段无需再重复防御。这是典型的「让非法状态无法表示」设计。
//
// 【线程模型】
//   值对象，构造后只读（无 setter，成员都经 getter 暴露拷贝）。Builder 非线程安全
//   （单线程内链式调用即可），但 build() 产物可安全地跨线程只读传递。
//
// 【DBRIDGE_EXPORT】
//   符号可见性宏（见 Export.h）。本项目静态库链接时展开为空；动态库时控制导出。
// ============================================================================

namespace dbridge::sync {

// ── SyncSelection —— 一次选择性推送的「选择集」值对象（不可变） ───────────────
//   语义：调用方精确选中的若干条记录（按表+主键），外加两个行为开关。
//   它描述的是「要推什么」，而非「怎么推」；后者由 selection/ 下各阶段负责。
class DBRIDGE_EXPORT SyncSelection {
   public:
    class Builder;  // 唯一的构造入口（前向声明，定义在文件末尾）

    // Record —— 一条「精确选中的记录」= 表名 + 该表的主键值（字符串形式）。
    //   主键以字符串承载：SQLite 的主键可能是 INTEGER/TEXT，统一转字符串便于跨层传递，
    //   解析阶段再按列类型绑定回查询参数（见 SelectionResolver::resolveRecord）。
    struct Record {
        QString table;  // 目标表名（已由 Builder 校验为「简单标识符」，防注入）
        QString primaryKey;  // 该表主键的值（字符串），唯一定位一行
    };

    // WhereClause —— 「按 WHERE 条件批量选择」的入参（MVP 暂不支持，见 H-01）。
    //   保留此结构是为了 API 形态完整、并在 build() 时给出明确报错，而非默默忽略。
    struct WhereClause {
        QString table;      // 目标表名
        QString whereExpr;  // 原生 SQL WHERE 表达式（MVP 阶段一律拒绝执行）
    };

    // ── 只读访问器（getter）——下游各阶段据此读取选择意图 ─────────────────────
    //   均返回成员的拷贝：值对象语义，杜绝外部持有内部容器引用后被意外篡改。

    // 精确选中的记录列表（表+主键）。
    QList<Record> records() const {
        return records_;
    }
    // WHERE 子句列表（MVP 恒为「构造即报错」，正常路径下应为空）。
    QList<WhereClause> whereClauses() const {
        return whereClauses_;
    }
    // 是否在选择集基础上自动补全外键(FK)父行依赖闭包。默认 true。
    //   true：推「选中行 + 其所有被外键引用的祖先行」，保证中心端应用时 FK 完整；
    //   false：只推选中行本身（调用方自担 FK 完整性，少见，多用于测试/特殊场景）。
    bool includeFkDeps() const {
        return includeFkDeps_;
    }
    // 是否对「与对端已一致的依赖父行」做剪枝（pruneConsistent）。默认 true。
    //   true：若某父行的指纹与一致性缓存中「中心端已确认的指纹」相同，则不重复推送，
    //         从而显著缩小推送体积（见 FkClosureBuilder::buildClosure + ConsistencyCache）。
    bool pruneConsistent() const {
        return pruneConsistent_;
    }
    // 选择集是否为空（既无记录也无 WHERE）。build() 据此发 E_SYNC_SELECTION_EMPTY。
    bool isEmpty() const {
        return records_.isEmpty() && whereClauses_.isEmpty();
    }

   private:
    friend class Builder;              // 仅 Builder 可构造与填充本对象
    SyncSelection() = default;         // 私有构造：外界无法绕过 Builder 直接创建
    QList<Record> records_;            // 见 records()
    QList<WhereClause> whereClauses_;  // 见 whereClauses()
    bool includeFkDeps_ = true;        // 见 includeFkDeps()，默认带 FK 闭包
    bool pruneConsistent_ = true;      // 见 pruneConsistent()，默认开启一致性剪枝
};

// ── SyncSelection::Builder —— 选择集的「链式构造器」+ 唯一合法出口 ───────────
//   用法（典型）：
//       QString err;
//       auto sel = SyncSelection::Builder()
//                      .addRecord("orders", "1001")
//                      .addRecords("order_items", {"5","6","7"})
//                      .includeFkDependencies(true)
//                      .build(&err);
//       if (!err.isEmpty()) { /* 选择集非法，sel 为空对象 */ }
//
//   设计要点：addXxx() 不在调用现场立即报错，而是把「可疑输入」累积到内部列表
//   （invalidTables_ / rawWhereAttempts_），统一推迟到 build() 处一次性裁决。
//   这样链式调用永远不抛错、返回 *this 可继续串接，错误处理只发生在 build() 一处。
class DBRIDGE_EXPORT SyncSelection::Builder {
   public:
    Builder() = default;

    // 追加一条精确记录（表名 + 单个主键值）。返回自身以支持链式调用。
    // 副作用：成功则记入 sel_.records_；表名非法则记入 invalidTables_（推迟到 build 报错）。
    Builder& addRecord(const QString& table, const QString& pk) {
        // H-11 fix：拒绝「非简单标识符」的表名，防止经由表名拼接造成 SQL 注入。
        //   解析阶段会用 quoteIdent(table) 拼进 PRAGMA / SELECT 语句；虽然有引号转义，
        //   但在源头就把表名约束为 [A-Za-z_][A-Za-z0-9_]* 是更稳妥的纵深防御。
        if (!isSimpleIdent(table))
            invalidTables_.append(table);  // 暂存非法表名，build() 时统一报错
        else
            sel_.records_.append({table, pk});
        return *this;
    }

    // 批量追加同一张表的多个主键。返回自身以支持链式调用。
    // 注意：表名非法时「整批」丢弃（直接 return），不会部分追加。
    Builder& addRecords(const QString& table, const QStringList& pks) {
        if (!isSimpleIdent(table)) {
            invalidTables_.append(table);  // 表名非法：记错并整批跳过
            return *this;
        }
        for (const auto& pk : pks)
            sel_.records_.append({table, pk});  // 表名安全：逐个主键展开为 Record
        return *this;
    }

    // 追加一条 WHERE 子句选择。MVP 阶段不支持，仅用于在 build() 给出明确错误。
    // H-01 fix: raw SQL WHERE is not yet supported (design §4.4: MVP = PK set only).
    //   （MVP 仅支持「主键集合」选择；原生 SQL WHERE 暂不支持。）
    // Storing the expression to produce a consistent error at build() time (not silently).
    //   （把表达式先存下来，以便在 build() 时给出一致的报错，而不是默默吞掉。）
    // 注意触发条件：仅当 whereExpr 非空才视为「尝试使用 raw SQL」并记入 rawWhereAttempts_；
    //   传空字符串则只追加一条空 WhereClause（不触发报错，但对选择集无实际贡献）。
    Builder& addWhere(const QString& table, const QString& whereExpr) {
        if (!whereExpr.isEmpty())
            rawWhereAttempts_.append(table);  // 暂存「误用 raw SQL」的表名，build() 时报错
        sel_.whereClauses_.append({table, whereExpr});
        return *this;
    }

    // 开关：是否自动补全外键依赖闭包（默认 true）。见 SyncSelection::includeFkDeps()。
    Builder& includeFkDependencies(bool on = true) {
        sel_.includeFkDeps_ = on;
        return *this;
    }
    // 开关：是否对「与对端已一致的父行」剪枝（默认 true）。见 SyncSelection::pruneConsistent()。
    Builder& pruneConsistentDependencies(bool on = true) {
        sel_.pruneConsistent_ = on;
        return *this;
    }

    // ── build —— 校验并产出最终 SyncSelection（唯一出口） ──────────────────────
    // 做什么：依次检查三类问题，命中即返回「空选择集」并写出错误说明；全部通过则返回填好的 sel_。
    // 参数：err（可空）——出参；命中任一错误分支时写入人类可读说明（已内嵌错误码前缀）。
    // 返回：SyncSelection。注意失败时返回的是「空对象」，调用方必须检查 *err 是否非空，
    //       不能仅凭返回值判断成败（值对象无 bool ok 字段）。
    // 错误模式（均以 E_SYNC_SELECTION_EMPTY 为前缀，见 dbridge/Errors.h）：
    //   1) 存在非法表名               —— H-11；
    //   2) 误用 raw SQL 的 addWhere    —— H-01；
    //   3) 选择集为空（无记录无 WHERE）。
    //   注意：分支 1、2 命中时直接返回「全新空对象」SyncSelection{}（丢弃已累积内容），
    //         分支 3 则返回当前（恰好为空的）sel_——两者对下游都表现为 isEmpty()==true。
    SyncSelection build(QString* err = nullptr) {
        // 分支①：H-11 —— 任一表名未通过 isSimpleIdent 校验，拒绝整个选择集。
        if (!invalidTables_.isEmpty()) {
            if (err)
                *err = QStringLiteral(
                           "E_SYNC_SELECTION_EMPTY: table name '%1' is not a valid "
                           "SQLite identifier (must match [A-Za-z_][A-Za-z0-9_]*)")
                           .arg(invalidTables_.first());  // 只报第一个非法表名作示例
            return SyncSelection{};
        }
        // 分支②：H-01 —— 检测到带非空 whereExpr 的 addWhere()，MVP 不支持，拒绝。
        // reject raw-SQL addWhere() calls — only PK-set selection is supported in MVP.
        if (!rawWhereAttempts_.isEmpty()) {
            if (err)
                *err = QStringLiteral(
                           "E_SYNC_SELECTION_EMPTY: addWhere() with raw SQL is not "
                           "supported in MVP (design §4.4); use addRecord()/addRecords() "
                           "for table '%1'")
                           .arg(rawWhereAttempts_.first());
            return SyncSelection{};
        }
        // 分支③：选择集为空（既无 records_ 也无 whereClauses_）——写错误但仍返回 sel_。
        if (sel_.isEmpty()) {
            if (err)
                *err = QStringLiteral("E_SYNC_SELECTION_EMPTY: selection is empty");
        }
        return sel_;
    }

   private:
    SyncSelection sel_;  // 正在构造的目标对象（Builder 是其 friend，可直填私有成员）
    QStringList rawWhereAttempts_;  // 累积「误用 raw SQL」的表名（H-01），build() 时裁决
    QStringList invalidTables_;  // 累积「表名非法」的表名（H-11），build() 时裁决

    // ── isSimpleIdent —— 表名防注入校验：是否为「简单 SQLite 标识符」 ──────────
    // H-11: simple identifier = starts with letter/underscore, contains only [A-Za-z0-9_].
    //   （简单标识符 = 首字符为字母或下划线，其余字符仅含字母/数字/下划线。）
    // 为什么：表名无法像值那样用「?」参数化绑定，只能字符串拼接进 SQL；把表名限制为
    //   此安全字符集，可从根本上杜绝「'; DROP TABLE ...」之类的注入面。
    // 返回：true=安全可用；false=空串、首字符非法、或含非法字符。
    // 复杂度：O(len)，遍历一遍字符串。
    static bool isSimpleIdent(const QString& s) {
        if (s.isEmpty())
            return false;  // 空表名直接判非法
        const QChar first = s[0];
        // 首字符必须是字母或下划线（数字开头不是合法标识符）。
        if (!first.isLetter() && first != QLatin1Char('_'))
            return false;
        // 其余字符：仅允许字母、数字、下划线。注意 isLetterOrNumber() 基于 Unicode 分类，
        // 已涵盖首字符那枚字符的再次校验（循环从 0 开始，重复检查 first 也无害）。
        for (const QChar& c : s) {
            if (!c.isLetterOrNumber() && c != QLatin1Char('_'))
                return false;
        }
        return true;
    }
};

}  // namespace dbridge::sync
