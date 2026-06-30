#pragma once
#include <QHash>
#include <QStringList>
#include <QVector>

// ============================================================================
// SchemaCatalog.h — 数据库 schema（表结构）的内存数据模型
// ============================================================================
//
// 【这个文件是什么】
//   它把「一个 SQLite 数据库里有哪些表、每张表有哪些列/索引/外键」这件事，
//   一次性读出来、以结构体形式缓存在内存里，形成一份「schema 目录」。
//   注意：这是 Excel↔SQLite 批量导入导出（ETL）子系统的 schema 自省，
//   位于 src/schema，与同步子系统的 src/sync/schema 是两码事，互不相关。
//
// 【在 ETL 管线中的位置】
//   程序启动/连接数据库后，先由 SchemaIntrospector（见同目录 .h/.cpp）通过
//   一系列 PRAGMA 自省语句把真实库结构读出来，填进本文件定义的 SchemaCatalog。
//   随后 ETL 的多个环节都把这份目录当作「权威的结构事实」来查询：
//     · 路由（Routing）：判断 Profile 里写的目标表/列是否真实存在；
//     · 外键查找（FK lookup）：靠 FkInfo 知道「子表的哪一列指向父表的哪一列」，
//       从而用业务键查父表代理主键、并把主键注入子表；
//     · 拓扑排序（Topological sort）：多表写入时，依据表间外键依赖排出先后顺序
//       （先写被依赖的父表，再写引用它的子表），避免外键约束失败。
//
// 【设计取向】
//   本文件全是「只装数据、几乎无行为」的 POD 风格结构 + 一个轻量容器类。
//   它只描述结构「是什么样」，不负责「怎么读出来」（那是 Introspector 的职责），
//   也不负责「怎么用」（那是路由/查找/排序模块的职责）——职责单一、便于复用。
//
// 【协作者】
//   · SchemaIntrospector —— 本目录的填充者：执行 PRAGMA 并写入这些结构。
//   · ETL 的路由 / 外键查找 / 拓扑排序模块 —— 本目录的消费者（只读查询）。
//   · 错误码字典见 include/dbridge/Errors.h（如 E_PROFILE_TABLE_NOT_FOUND 等）。
//
// 命名空间 dbridge::detail：表明这些是库内部实现细节，非对外公共 API。
// ============================================================================

namespace dbridge::detail {

// ── ColumnInfo —— 一张表里「一列」的结构描述 ────────────────────────────────
//
//   对应 SQLite `PRAGMA table_info / table_xinfo` 自省出来的一行。
//   它回答关于某列的所有结构性问题：叫什么、声明类型、能否为空、是否主键……
//   这些信息后续被校验（非空约束）、类型转换（类型亲和性）、外键注入（主键）等使用。
struct ColumnInfo {
    QString name;  // 列名（在表内唯一），如 "id" / "customer_id"
    // 列在 CREATE TABLE 里「声明的类型文本」，原样保留，如 "INTEGER" / "VARCHAR(32)" / ""。
    //
    // 关键概念——SQLite 的「类型亲和性（type affinity）」：
    //   SQLite 是动态类型，列的「声明类型」只是一段文本，真正起作用的是由这段文本
    //   推导出的 5 种「亲和性」之一（TEXT / NUMERIC / INTEGER / REAL / BLOB）。
    //   推导规则（官方）大致为：含 "INT"→INTEGER；含 "CHAR/CLOB/TEXT"→TEXT；
    //   含 "BLOB" 或为空→BLOB；含 "REAL/FLOA/DOUB"→REAL；其余→NUMERIC。
    //   因此这里保存的是「原始声明文本」，是否/如何换算成亲和性交给消费方按需处理。
    QString declaredType;
    bool notNull = false;  // 是否带 NOT NULL 约束（true 表示该列不允许写入 NULL）
    bool primaryKey = false;  // 是否为主键的一部分（单列或复合主键中的任一列均为 true）
    // 复合主键中该列的次序，1 基（1 表示主键第 1 列，2 表示第 2 列……）。
    // composite PK order (1-based)
    // 非主键列保持 0；单列主键则为 1。后续判断「是否单列主键」可用 pkOrder==1 且无第二列。
    int pkOrder = 0;
    QString defaultValue;  // 列的默认值表达式文本（无默认值则为空字符串）
    // 是否为 AUTOINCREMENT 自增列。注意：这并非来自 PRAGMA，而是 Introspector 额外
    // 解析 CREATE TABLE 原文里是否含 "AUTOINCREMENT" 关键字得出（见 isAutoIncrement）。
    bool autoIncrement = false;
    // 是否为「生成列（generated/computed column）」（VIRTUAL 或 STORED）。
    // 生成列的值由表达式算出、不可直接写入，ETL 写库时应跳过它。
    bool generated = false;
};

// ── IndexInfo —— 一张表里「一个索引」的结构描述 ─────────────────────────────
//
//   对应 `PRAGMA index_list`（索引清单）+ `PRAGMA index_info`（每索引的列）。
//   ETL 关心索引主要是为了 UPSERT：UPSERT 需要一个「冲突目标（唯一约束）」来
//   判定「该插入还是更新」。能充当冲突目标的，正是 unique 且非 partial 的索引。
struct IndexInfo {
    QString name;  // 索引名
    bool unique = false;  // 是否唯一索引（UNIQUE）；只有唯一索引才可能做 UPSERT 冲突目标
    QStringList columns;  // 该索引覆盖的列（有序）；多列即「复合唯一约束」
    // H-02 fix：部分索引（partial index，带 WHERE 子句的索引）不能作为 UPSERT 的
    //   冲突目标——因为 SQLite 要求 ON CONFLICT 子句必须匹配一个「非部分」的
    //   UNIQUE 约束或 PRIMARY KEY。故此处单独记录 partial 标志，供 UPSERT 逻辑排除。
    // H-02 fix: partial indexes are not valid UPSERT conflict targets because SQLite requires the
    // ON CONFLICT clause to match a non-partial UNIQUE constraint or PRIMARY KEY.
    bool partial = false;
};

// ── FkInfo —— 一条「外键关系」的描述（本表的某列指向另一张表的某列）─────────
//
//   对应 `PRAGMA foreign_key_list`。它是 ETL 外键查找与拓扑排序的核心数据：
//     · 外键查找：知道「本表 fromColumn 引用 refTable.toColumn」，便能用业务键
//       去父表 refTable 查出代理主键，再注入回子表的外键列；
//     · 拓扑排序：每条 FkInfo 就是一条「本表依赖 refTable」的有向边，据此定写入序。
struct FkInfo {
    QString refTable;    // 被引用的父表名（外键指向的目标表）
    QString fromColumn;  // 本表（持有外键的子表）中作为外键的列名
    QString toColumn;    // 父表 refTable 中被引用的列名（通常是父表主键）
};

// ── TableInfo —— 一张表的完整结构（列 + 索引 + 外键的聚合）──────────────────
//
//   它是 schema 目录的「最小自洽单元」：拿到一个 TableInfo，就掌握了对这张表做
//   校验 / 路由 / 外键查找 / 拓扑排序所需的全部结构事实，无需再回查数据库。
struct TableInfo {
    QString name;                 // 表名（在目录内唯一）
    QVector<ColumnInfo> columns;  // 全部列（按 cid 自然次序，即建表时的列序）
    QVector<IndexInfo> indexes;   // 全部索引（含主键索引、唯一索引、普通索引）
    QVector<FkInfo> foreignKeys;  // 全部外键关系（本表作为子表指向其它父表的边）

    // column —— 按列名查找该列的结构信息。
    //   做什么：在 columns 中线性查找名为 n 的列。
    //   返回：找到则返回指向该 ColumnInfo 的指针；找不到返回 nullptr。
    //   为什么返回指针而非值：避免拷贝，且用 nullptr 干净地表达「不存在」。
    //   副作用：无（const 成员函数，纯查询）。
    //   复杂度：O(列数)。表的列数通常很小，线性扫描足够；调用方若高频查找，
    //           可自行在外层建哈希索引，本结构刻意保持简单。
    //   注意：返回指针指向 columns 内部元素，其有效性依赖 TableInfo 本身存活、
    //         且此后未对 columns 增删（增删可能触发 QVector 重分配使指针失效）。
    const ColumnInfo* column(const QString& n) const {
        for (const auto& c : columns) {
            if (c.name == n)
                return &c;
        }
        return nullptr;
    }
};

// ── SchemaCatalog —— 整库 schema 目录（表名 → TableInfo 的查询容器）─────────
//
//   做什么：以「表名」为键，持有全库所有 TableInfo，提供增/清/存在性/取用/列表
//           几个最小操作。它是 ETL 各环节共享的、一份只读的「结构权威」。
//   为什么独立成类（而非裸 QHash）：把底层容器（这里是 QHash）封装起来，对外只暴露
//           受控的语义化接口，将来若想换实现（如改顺序容器、加缓存）调用方不受影响。
class SchemaCatalog {
   public:
    // addTable —— 加入/覆盖一张表的结构。
    //   做什么：以 t.name 为键写入；若同名已存在则覆盖。
    //   谁来调：由 SchemaIntrospector::load 在自省完每张表后调用，逐张灌入。
    //   副作用：修改内部哈希表 tables_。复杂度：均摊 O(1)（含一次 TableInfo 拷贝）。
    void addTable(const TableInfo& t) {
        tables_[t.name] = t;
    }
    // clear —— 清空目录。
    //   何时用：重新自省前先清空（见 load 开头），保证目录不残留上一次的旧结构。
    //   副作用：清空 tables_。复杂度：O(表数)。
    void clear() {
        tables_.clear();
    }

    // hasTable —— 目录里是否存在名为 name 的表。
    //   典型用途：路由阶段校验 Profile 引用的表是否真实存在
    //             （不存在则报 E_PROFILE_TABLE_NOT_FOUND）。
    //   返回：存在为 true。副作用：无（const）。复杂度：O(1)。
    bool hasTable(const QString& name) const {
        return tables_.contains(name);
    }

    // table —— 取出名为 name 的表结构（只读）。
    //   做什么：在哈希表中查 name。
    //   返回：找到返回指向内部 TableInfo 的指针；找不到返回 nullptr（干净表达「无此表」）。
    //   注意：返回的指针在该表被 addTable 覆盖、clear、或 catalog 析构后即失效；
    //         调用方不应长期持有，应即取即用。
    //   副作用：无（const）。复杂度：O(1)。
    const TableInfo* table(const QString& name) const {
        auto it = tables_.find(name);
        return it != tables_.end() ? &it.value() : nullptr;
    }

    // allTables —— 列出目录中所有表名。
    //   典型用途：拓扑排序前先取全表清单作为图的顶点集，再据外键边定序。
    //   返回：表名列表（QHash::keys 的次序不保证；需要确定序请调用方自行排序）。
    //   副作用：无（const）。复杂度：O(表数)。
    QStringList allTables() const {
        return tables_.keys();
    }

   private:
    // 底层存储：表名 → 该表完整结构。选 QHash 是为了 O(1) 的按名取用
    //   （ETL 中「按表名查结构」是最高频操作）。代价是遍历无固定次序。
    QHash<QString, TableInfo> tables_;
};

}  // namespace dbridge::detail
