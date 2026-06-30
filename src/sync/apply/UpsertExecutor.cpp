#include "UpsertExecutor.h"

#include <QSqlError>

#include "sql/SqlBuilder.h"

// ============================================================================
// UpsertExecutor.cpp — UPSERT 批量写入的实现
// ============================================================================
//
// 【主循环结构】apply() 对每条 RowMutation 做四步：
//   ① 防御：列为空 → 记一条逐行错误并跳过（无法构造合法 SQL）。
//   ② 构造 + 取缓存：用整条 SQL 当键，命中则复用、否则 prepare 后入缓存。
//   ③ 绑定本行的值（占位符 ? 与 m.values 一一对应）。
//   ④ 执行；失败 → 记逐行错误并继续（非致命），成功 → 进入下一条。
// buildUpsertSql() 则纯粹负责「把列名/主键/模式拼成一条安全的 UPSERT 文本」。
// ============================================================================

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

bool UpsertExecutor::apply(QSqlDatabase& db, const QList<RowMutation>& rows,
                           QList<dbridge::RowError>* errors, QString* err) {
    // 前置硬条件：连接必须已打开。未打开属「致命」，立即返回 false（无法做任何写）。
    if (!db.isOpen()) {
        if (err)
            *err = QStringLiteral("database is not open");
        return false;
    }

    for (const RowMutation& m : rows) {
        // ① 空列防御：没有任何列就无法生成 "(col...) VALUES (?...)"。
        //    归类为逐行约束错误（E_SYNC_APPLY_CONSTRAINT），收集后跳过该行，不影响其它行。
        if (m.columns.isEmpty()) {
            dbridge::RowError re;
            re.sheet = m.table;  // 复用 RowError.sheet 字段承载「表名」（同步无 Excel sheet 概念）
            re.row = 0;  // 0 = 非「具体 Excel 行」的表级/行级错误
            re.code = QStringLiteral("E_SYNC_APPLY_CONSTRAINT");
            re.message = QStringLiteral("empty columns for table %1").arg(m.table);
            if (errors)
                errors->append(re);
            continue;
        }

        // ② M-04 fix: 用「完整 SQL 字符串」做缓存键，使同一张表的不同列集 / 不同主键集
        //    始终落到不同的缓存项，绝不会串用到一条参数语义不符的预编译语句。
        //    旧实现以 (table:mode) 为键，必须再靠脆弱的 lastQuery() 比对来辨别，已废弃。
        const QString sql = buildUpsertSql(m.table, m.columns, m.pkColumns, m.mode);
        const QString key = sql;  // 键即 SQL 本身

        bool needPrepare = !cache_.contains(key);
        if (needPrepare) {
            QSqlQuery q(db);
            if (!q.prepare(sql)) {
                // prepare 失败属「致命」：通常意味着目标表根本不存在（或 SQL 非法）。
                // 此时无法继续，整批中止，返回 false 让上层回滚事务。
                if (err)
                    *err = q.lastError().text();
                return false;
            }
            cache_.insert(key, q);  // 编译成功 → 入缓存供后续同形 SQL 复用
        }

        QSqlQuery& q = cache_[key];  // 取缓存里的语句引用（注意是引用，下面绑定会作用到它）
        // ③ 为本行重新绑定值：m.values 与 SQL 里的 ? 占位符按顺序一一对应。
        //    （上一行执行后，QSqlQuery 的绑定会被新一轮 addBindValue 覆盖。）
        for (const QVariant& v : m.values)
            q.addBindValue(v);

        // ④ 执行本行。
        if (!q.exec()) {
            // 逐行失败（约束冲突 / 外键违反等）：记一条错误，但「不」中断整批——
            //   是否因这条失败而回滚，由更上层（CapturedWriteTemplate 的 C-09 守卫）决定。
            dbridge::RowError re;
            re.sheet = m.table;
            re.row = 0;
            re.code = QStringLiteral("E_SYNC_APPLY_CONSTRAINT");
            re.message = q.lastError().text();
            // 若该行携带了来源元数据，取出 rawValue 一并回报，便于诊断「是哪条原始数据出的错」。
            if (!m.originMeta.isEmpty())
                re.rawValue = m.originMeta.value(QStringLiteral("rawValue")).toString();
            if (errors)
                errors->append(re);
            // 继续处理下一行——逐行失败是非致命的。
            continue;
        }
    }

    return true;  // 走到这里：没有致命错误（逐行错误已在 *errors 中，由上层裁决）
}

// 清空预编译语句缓存：连接重建或表结构迁移后，旧语句失效，必须丢弃避免误用。
void UpsertExecutor::clearPreparedCache() {
    cache_.clear();
}

// ---------------------------------------------------------------------------
// private
// ---------------------------------------------------------------------------

// buildUpsertSql —— 按 (表名, 列集, 主键集, 模式) 拼出一条参数化 UPSERT SQL（只拼串、不执行）。
// 标识符全部经 quoteIdent 转义；占位符用 ?，由调用方按 cols 顺序 addBindValue 绑值。
QString UpsertExecutor::buildUpsertSql(const QString& table, const QStringList& cols,
                                       const QStringList& pkCols, UpsertMode mode) {
    // M-05 fix: use SqlBuilder::quoteIdent so embedded double-quotes are properly escaped.
    // 【M-05 修复】统一用 SqlBuilder::quoteIdent 给标识符加引号——它会把列名/表名里内嵌的
    //   双引号正确转义成两个双引号，既防 SQL 注入，又兼容含特殊字符的列名。
    auto quote = [](const QString& s) { return detail::SqlBuilder::quoteIdent(s); };

    // Build column list: "c1","c2",...
    // 拼带引号的列名清单："c1","c2",...（用于 INSERT 的 (列...) 部分）。
    QStringList quotedCols;
    quotedCols.reserve(cols.size());
    for (const QString& c : cols)
        quotedCols << quote(c);
    const QString colList = quotedCols.join(QStringLiteral(", "));

    // Build placeholder list: ?,?,...
    // 拼等长的占位符清单：?,?,...（与列一一对应，绑值时顺序匹配）。
    QStringList placeholders;
    placeholders.reserve(cols.size());
    for (int i = 0; i < cols.size(); ++i)
        placeholders << QStringLiteral("?");
    const QString phList = placeholders.join(QStringLiteral(", "));

    if (mode == UpsertMode::DoNothing) {
        // DoNothing：撞冲突键则什么都不做（保留本地旧值）→ INSERT OR IGNORE。
        return QStringLiteral("INSERT OR IGNORE INTO %1 (%2) VALUES (%3)")
            .arg(quote(table), colList, phList);
    }

    // DoUpdate: build conflict target and SET clauses (non-pk columns only).
    // DoUpdate：撞冲突键则用新值覆盖。先拼「冲突目标」(主键列清单) 与 SET 子句(仅非主键列)。
    QStringList quotedPk;
    quotedPk.reserve(pkCols.size());
    for (const QString& pk : pkCols)
        quotedPk << quote(pk);
    const QString pkConflict = quotedPk.join(QStringLiteral(", "));

    // SET 子句：每个非主键列写成 "col=excluded.col"，excluded 即本次试图插入的新值。
    // 主键列不在 SET 里——它们是冲突判定依据，不应被改写。
    QStringList setClauses;
    for (const QString& c : cols) {
        if (!pkCols.contains(c))
            setClauses << QStringLiteral("%1=excluded.%2").arg(quote(c), quote(c));
    }

    if (setClauses.isEmpty()) {
        // All columns are PK columns — fall back to DO NOTHING.
        // 所有列都是主键列（没有可 UPDATE 的非主键列）→ DO UPDATE 的 SET 会是空的、无意义，
        //   退化为 INSERT OR IGNORE（等价 DoNothing），避免拼出非法 SQL。
        return QStringLiteral("INSERT OR IGNORE INTO %1 (%2) VALUES (%3)")
            .arg(quote(table), colList, phList);
    }

    // 正常 UPSERT：撞主键则原地更新非主键列。%4=冲突目标(主键列)，%5=SET 子句。
    return QStringLiteral(
               "INSERT INTO %1 (%2) VALUES (%3) "
               "ON CONFLICT (%4) DO UPDATE SET %5")
        .arg(quote(table), colList, phList, pkConflict, setClauses.join(QStringLiteral(", ")));
}

// cacheKey() is no longer used (M-04 fix replaced it with SQL-as-key). Removed.

}  // namespace dbridge::sync
