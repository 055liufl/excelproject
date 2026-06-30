#pragma once
#include "dbridge/sync/SyncSelection.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

// ============================================================================
// SelectionResolver.h — 选择性推送第 1 阶段：把「选择意图」解析成「真实行数据」
// ============================================================================
//
// 【在流水线中的位置】
//   SyncSelection（表+主键，仅意图）→ ★SelectionResolver★ → 库里的真实整行
//                                                           → FkClosureBuilder（补 FK 闭包）
//   它负责「按主键去只读连接里把每一行的完整内容捞出来」，是闭包计算与冻结打包的原料源。
//
// 【为什么需要它】
//   下游要计算 FK 闭包、要冻结行内容指纹、要打包推送，都需要「整行的所有列值」，
//   而 SyncSelection 只给了表名和主键。本阶段把 (表,主键) → QVariantMap(列名→值)。
//
// 【只读连接(rconn)】
//   解析在一个独立的只读快照连接上进行，避免与前台写入互相干扰，也保证一次推送看到的是
//   一致的数据视图（配合后续 freeze 冻结，抵御推送期间的数据漂移）。
//
// 【设计约束】
//   · 当前只支持「单列主键」表（通过 PRAGMA table_info 的 pk 标志识别第一个 pk 列）。
//   · WHERE 子句路径在 MVP 一律安全拒绝（见 .cpp 的 resolveWhere / 与 H-01 对应）。
// ============================================================================

namespace dbridge::sync {

// Resolves SyncSelection entries to concrete rows on a read-only connection.
//   （在只读连接上，把 SyncSelection 的各条目解析为具体的行。）
class SelectionResolver {
   public:
    // ResolveResult —— 一条解析结果：定位信息（表+主键）+ 整行内容。
    struct ResolveResult {
        QString table;  // 行所属表名
        QString pk;  // primary key value as string —— 主键值（统一以字符串承载）
        QVariantMap row;  // full row data —— 整行数据：列名 → 值
    };

    // ── resolvePk —— 顶层入口：解析选择集中的全部条目 ─────────────────────────
    // 做什么：遍历 sel 的 records()（精确主键）与 whereClauses()，逐条解析为真实行，追加到 *out。
    // 为什么：把「意图」物化为「数据」，供 FkClosureBuilder 等下游消费。
    // 参数：rconn 只读连接；sel 已校验的选择集；out 结果输出（追加，不清空）；err 出参错误说明。
    // 返回：true 全部解析成功；false 任一条目失败（*err 含原因），调用方应中止本次推送。
    // 副作用：向 *out 追加 ResolveResult；对 rconn 执行只读 SELECT/PRAGMA（不写库）。
    // 注意：主键查无此行时「不报错也不追加」（视为该选中行已不存在，静默跳过）。
    bool resolvePk(QSqlDatabase& rconn, const SyncSelection& sel, QList<ResolveResult>* out,
                   QString* err);

   private:
    // 解析单条精确记录：按主键 SELECT 整行；命中则追加到 *out。
    bool resolveRecord(QSqlDatabase& rconn, const QString& table, const QString& pk,
                       QList<ResolveResult>* out, QString* err);
    // 解析 WHERE 子句：MVP 阶段安全拒绝（恒返回 false 并写错误），见 .cpp 说明。
    bool resolveWhere(QSqlDatabase& rconn, const QString& table, const QString& whereExpr,
                      QList<ResolveResult>* out, QString* err);
    // 从整行中取出主键值（按表的 pk 列名查表）；表无单列主键时返回空串。
    static QString rowToPk(const QVariantMap& row, const QString& table, QSqlDatabase& rconn);
};

}  // namespace dbridge::sync
