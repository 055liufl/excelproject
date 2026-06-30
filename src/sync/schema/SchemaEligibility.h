#pragma once
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

// ============================================================================
// SchemaEligibility.h — 同步「准入资格」自省：判定哪些表能安全参与增量同步
// ============================================================================
//
// 【这个文件是什么 / 为什么存在】
//   增量同步的核心动作是把对端的变更（INSERT/UPDATE/DELETE）以 UPSERT 方式应用到本端
//   对应表上。但并非任何 SQLite 对象都能这样做：视图不可写、虚表（FTS/R*Tree 等）的存储
//   是隐式的、影子表是虚表的内部存储不能直接动、没有「单列非空主键」就无法稳定定位行、
//   没有可用的 ON CONFLICT 冲突目标就拼不出 UPSERT 语句。SchemaEligibility 在「session
//   attach（开始捕获/应用变更）之前」逐表体检，把不合格的表连同原因挑出来，让上层及早
//   拒绝（避免运行到一半才在某张表上失败）。
//
// 【拒绝清单（reject reasons）】
//   · 表不存在；
//   · 是视图（view，非基表）；
//   · 是虚表（CREATE VIRTUAL TABLE）；
//   · 是影子表（虚表的内部存储，名字形如 <vtab>_<suffix>）；
//   · 没有显式 PRIMARY KEY；
//   · PRIMARY KEY 列可空（nullable）——无法可靠定位行；
//   · 复合主键（多列 PK）——MVP 阶段不支持，报 E_SYNC_COMPOSITE_PK_NOT_SUPPORTED；
//   · 没有可用作 ON CONFLICT 目标的非 partial 唯一约束（PK 本身就够，故有合法 PK 即通过）。
//
// 【与 SchemaGuard 的分工】
//   SchemaGuard 管「两端结构是否一致」（跨节点比对指纹）；
//   SchemaEligibility 管「本端这张表本身能不能玩同步」（本地准入体检）。两者互补。
//
// 【线程模型】全部为 static 函数，无共享可变状态；但都操作传入的 QSqlDatabase，
//   需在该连接所属线程调用。复杂度均为「表数 × 每表少量 PRAGMA」级别，开销很小。
// ============================================================================

namespace dbridge::sync {

// ── SchemaEligibility —— 同步表准入资格校验器（纯静态工具类）─────────────────
// 在 session attach 前检查每张同步表是否满足 UPSERT 合法条件。
// 拒绝：虚表、视图、影子表、无显式非空PK、无可用冲突目标。
class SchemaEligibility {
   public:
    // 逐表校验。rejected 填不合格表名+原因，返回 false 表示有不合格项。
    // 参数：syncTables 待校验的表名列表；rejected（可空）逐条追加 "表名: 原因"；
    //       err（可空）仅在「自省过程本身出错（如 PRAGMA 查询失败）」时写入——
    //       注意这区别于「表不合格」：表不合格走 rejected + 返回 false，不写 err。
    // 返回：全部合格 → true；存在不合格项 → false（rejected 里有明细）；
    //       自省出错 → false 且 err 非空（此时立即中止，不再继续后续表）。
    static bool verify(QSqlDatabase& db, const QStringList& syncTables, QStringList* rejected,
                       QString* err);

    // 把「空的 syncTables」展开为「全部用户表」。
    // C-08 fix: expand empty syncTables to all user tables (non-sqlite_%, non-__sync_%).
    // Returns the canonical set used for session attach and eligibility check.
    // C-08 修复（译）：调用方传空表示「同步所有业务表」——此处展开为数据库里全部用户表，
    //   并排除 SQLite 内建表（sqlite_%）与本同步子系统的元数据表（__sync_%）。
    //   返回的这份集合即后续 session attach 与资格校验所用的「规范表集」。
    static QStringList expandSyncTables(QSqlDatabase& db, const QStringList& syncTables,
                                        QString* err);

   private:
    // 单表自省结果：把一张表「与准入相关」的结构事实打包，供 verify() 逐项判定。
    struct TableInfo {
        bool exists = false;     // 表是否存在
        bool isView = false;     // 是否为视图（不可写）
        bool isVirtual = false;  // 是否为虚表（CREATE VIRTUAL TABLE）
        bool isShadow = false;   // 名称含 "_" 且被 FTS/R*Tree 引用（影子表）
        QStringList pkCols;      // PK 列，按 pk 序号排列（空=无显式 PK）
        bool pkNotNull = false;  // 所有 PK 列均 NOT NULL（INTEGER rowid 主键视为隐式非空）
    };

    // 体检单张表，填好 TableInfo 返回。遇到查询错误时写 *err 并尽早返回（字段保持默认）。
    static TableInfo introspect(QSqlDatabase& db, const QString& table, QString* err);

    // 检查是否存在非 partial 的 UNIQUE 索引可做 ON CONFLICT 目标（PK 本身就够）
    static bool hasUpsertTarget(QSqlDatabase& db, const QString& table, QString* err);

    // 影子表判断：sqlite_master 中有 CREATE VIRTUAL TABLE 的表名是该表前缀
    static bool isShadowTable(QSqlDatabase& db, const QString& table, QString* err);
};

}  // namespace dbridge::sync
