// ============================================================================
// SelectionResolver.cpp — 选择集解析的实现
// ============================================================================
// 详见 SelectionResolver.h 头注释。本文件把 (表,主键) 物化为整行 QVariantMap。
// ============================================================================
#include "SelectionResolver.h"

#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

#include "sql/SqlBuilder.h"  // detail::SqlBuilder::quoteIdent —— 安全地为标识符加引号

namespace dbridge::sync {

// ── pkColumnForTable —— 查出某表的「单列主键」列名 ────────────────────────────
// Returns the pk column name for a table via PRAGMA table_info.
//   （通过 PRAGMA table_info 取得该表的主键列名。）
// Returns empty string on failure. （失败/无单列主键时返回空串。）
// 实现要点：PRAGMA table_info(<表>) 每行描述一列，列含义为
//   [0]=cid 列序号  [1]=name 列名  [2]=type 类型  [3]=notnull  [4]=dflt_value  [5]=pk。
//   其中 pk 字段：0=非主键；对单列主键恒为 1；复合主键时为 1,2,3...（本函数只认 ==1，
//   因此复合主键表会「找不到主键」返回空串 —— 与「同步仅支持单列主键」的设计约束一致）。
static QString pkColumnForTable(const QString& table, QSqlDatabase& rconn) {
    QSqlQuery q(rconn);
    // M-11 fix: quote identifier (escapes embedded double-quotes) instead of raw "%1".
    //   （用 quoteIdent 给表名加引号并转义内嵌双引号，取代直接 "%1" 拼接，纵深防注入。）
    q.prepare(QStringLiteral("PRAGMA table_info(") + detail::SqlBuilder::quoteIdent(table) +
              QLatin1Char(')'));
    if (!q.exec())
        return {};  // PRAGMA 执行失败（如表不存在）→ 空串
    while (q.next()) {
        // pk column: pragma column index 5 (pk), nonzero means primary key
        //   （主键标志在 PRAGMA 结果的第 5 列；非零即主键。这里只取 ==1 的单列主键。）
        int pkOrder = q.value(5).toInt();
        if (pkOrder == 1) {
            return q.value(1).toString();  // column name at index 1 —— 第 1 列是列名
        }
    }
    return {};  // 遍历完未见 pk==1 → 无单列主键（含复合主键 / WITHOUT ROWID 等情形）
}

// ── recordToMap —— 把查询当前行转成 QVariantMap（列名 → 值） ──────────────────
// Converts a QSqlQuery's current record to a QVariantMap.
// 前置：调用前 q 必须已 next() 定位到某一行。复杂度 O(列数)。
static QVariantMap recordToMap(const QSqlQuery& q) {
    QVariantMap row;
    const QSqlRecord rec = q.record();  // 取得当前行的字段元信息（列名等）
    for (int i = 0; i < rec.count(); ++i)
        row.insert(rec.fieldName(i), q.value(i));  // 列名 → 该列的值
    return row;
}

// ── rowToPk —— 从整行里提取其主键值 ──────────────────────────────────────────
// 做什么：先查出 table 的主键列名，再从 row 里取该列的值（转字符串）。
// 返回：主键值字符串；表无单列主键时返回空串。
// 备注：本类内部辅助，便于「已有整行、需回取主键」的场景统一逻辑。
QString SelectionResolver::rowToPk(const QVariantMap& row, const QString& table,
                                   QSqlDatabase& rconn) {
    const QString pkCol = pkColumnForTable(table, rconn);
    if (pkCol.isEmpty())
        return {};
    return row.value(pkCol).toString();
}

// ── resolveRecord —— 解析单条「表+主键」为整行 ───────────────────────────────
// 做什么：确定主键列名 → 以参数化 SELECT 按主键取一行 → 命中则封装成 ResolveResult 追加到 out。
// 参数：rconn 只读连接；table/pk 待解析的表与主键；out 追加目标；err 出参错误。
// 返回：true=查询成功执行（无论是否命中行）；false=无法确定主键列 / SELECT 执行失败。
// 副作用：对 rconn 执行 PRAGMA + 一次 SELECT；命中时向 *out 追加 1 条结果。
// 错误模式：
//   · 无单列主键 → "Cannot determine PK column ..."（复合主键 / 无主键表）；
//   · q.exec() 失败 → 透传 SQL 驱动错误文本。
// 关键点：q.next() 为假（查无此行）属正常——不报错、不追加，等价于「该选中行已不存在，跳过」。
bool SelectionResolver::resolveRecord(QSqlDatabase& rconn, const QString& table, const QString& pk,
                                      QList<ResolveResult>* out, QString* err) {
    const QString pkCol = pkColumnForTable(table, rconn);
    if (pkCol.isEmpty()) {
        if (err)
            *err = QStringLiteral("Cannot determine PK column for table '%1'").arg(table);
        return false;
    }

    QSqlQuery q(rconn);
    // M-11 fix: quote table and column identifiers.
    //   （对表名与列名均加引号转义；主键值则用占位符 ? 参数化绑定，三处都不裸拼用户数据。）
    // LIMIT 1：主键唯一，至多一行；加 LIMIT 让查询意图更明确、也防御异常多行。
    q.prepare(QStringLiteral("SELECT * FROM ") + detail::SqlBuilder::quoteIdent(table) +
              QStringLiteral(" WHERE ") + detail::SqlBuilder::quoteIdent(pkCol) +
              QStringLiteral(" = ? LIMIT 1"));
    q.addBindValue(pk);  // 主键值作为绑定参数，杜绝注入

    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();  // 透传底层 SQL 错误
        return false;
    }

    if (q.next()) {  // 命中一行才追加；未命中则什么都不做（静默跳过，见上方说明）
        ResolveResult r;
        r.table = table;
        r.pk = pk;
        r.row = recordToMap(q);  // 物化整行
        out->append(std::move(r));
    }
    return true;
}

// ── resolveWhere —— WHERE 子句解析（MVP：安全拒绝） ──────────────────────────
// M-03 fix: raw-SQL addWhere() is rejected at SyncSelection::Builder::build() time (H-01),
//   （原生 SQL 的 addWhere() 已在 Builder::build() 处被拒绝（H-01），）
// so this path should never be reached in MVP.  If it is, fail safely rather than
//   （故 MVP 下本函数本不应被走到。万一被走到，也要安全失败，）
// executing an arbitrary WHERE clause (SQL injection risk per design §4.4).
//   （绝不执行任意 WHERE 表达式——那是设计 §4.4 指出的 SQL 注入风险。）
// 参数 rconn/whereExpr/out 均刻意未使用（注释化形参名）：本函数恒拒绝，不接触数据库。
// 返回：恒为 false，并把错误写入 *err。这是「双保险」——即便上游校验被绕过也不会泄漏注入面。
bool SelectionResolver::resolveWhere(QSqlDatabase& /*rconn*/, const QString& table,
                                     const QString& /*whereExpr*/, QList<ResolveResult>* /*out*/,
                                     QString* err) {
    if (err)
        *err = QStringLiteral(
                   "addWhere() raw SQL is not supported in MVP for table '%1'; "
                   "use addRecord()/addRecords() instead (design §4.4)")
                   .arg(table);
    return false;
}

// ── resolvePk —— 顶层入口：解析整个选择集 ────────────────────────────────────
// 做什么：先逐条解析精确记录（records），再逐条处理 WHERE 子句（whereClauses）。
// 短路语义：任一条目失败立即返回 false（错误已写入 err），不再处理后续条目。
// 复杂度：O(记录数) 次 SELECT；每条 SELECT 走主键索引近似 O(log n)。
// 正常路径下 whereClauses() 应为空（Builder 已在 build() 拒绝 raw WHERE），故该循环通常不执行；
//   若出现非空项，resolveWhere 会令本函数失败——构成对 H-01 的运行期兜底。
bool SelectionResolver::resolvePk(QSqlDatabase& rconn, const SyncSelection& sel,
                                  QList<ResolveResult>* out, QString* err) {
    for (const auto& rec : sel.records()) {
        if (!resolveRecord(rconn, rec.table, rec.primaryKey, out, err))
            return false;
    }
    for (const auto& wc : sel.whereClauses()) {
        if (!resolveWhere(rconn, wc.table, wc.whereExpr, out, err))
            return false;
    }
    return true;
}

}  // namespace dbridge::sync
