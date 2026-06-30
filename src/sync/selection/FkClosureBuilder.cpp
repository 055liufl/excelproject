// ============================================================================
// FkClosureBuilder.cpp — 外键闭包构建与拓扑排序的实现
// ============================================================================
// 详见 FkClosureBuilder.h 头注释。本文件实现三件事：
//   ① buildClosure：以 BFS 沿外键向父表方向递归补全被引用的父行（构成闭包）；
//   ② topoSort：用 Kahn 算法把闭包排成「父先子后」，供对端按序无障碍 UPSERT；
//   ③ build：把以上两步串成对外的一次性入口，并夹带去重、上限校验、一致性剪枝。
// 关键不直观处（闭包递归、指纹比对、一致性剪枝、Kahn 入度法）在各处逐行解释。
// ============================================================================
#include "FkClosureBuilder.h"

#include "dbridge/Errors.h"

#include <QCryptographicHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

#include "sql/SqlBuilder.h"  // detail::SqlBuilder::quoteIdent —— 安全地为标识符加引号

namespace dbridge::sync {

// ── entryKey —— 生成「表+主键」的稳定去重键 ───────────────────────────────────
// Stable key for dedup set: "table\x1fpk"
//   （去重集合用的稳定键，形如 "表\x1f主键"。）
// 为什么用 \x1f（ASCII 单元分隔符 US）：它几乎不会出现在表名/主键里，用作分隔符可避免
//   "ab"+"c" 与 "a"+"bc" 撞键的歧义；比用普通字符（如冒号）更安全。
static QString entryKey(const QString& table, const QString& pk) {
    return table + QLatin1Char('\x1f') + pk;
}

// ── rowFingerprint —— 计算一行内容的粗粒度指纹（SHA-1） ───────────────────────
// Rough fingerprint for a row (used for consistency check when localFp is unknown).
//   （行内容的粗略指纹；在「本地指纹未知」时用于一致性比对。）
// 做什么：把整行的「列名\0值\0列名\0值…」拼成字节串再做 SHA-1。
// 为什么这样拼：列名与值都用 '\0' 分隔，避免相邻字段拼接产生的歧义碰撞；
//   QVariantMap 的 key 天然按字典序有序遍历，因此同样内容必得同样指纹（顺序稳定）。
// 注意（与 ChunkStreamer 的指纹算法不同）：此处把每个值 toString() 后比对，是「粗粒度」
//   一致性判断——只为剪枝服务，能命中即省一次推送，命中失败最多多推一行，无正确性风险。
// 复杂度：O(列数 × 字段长)。
static QByteArray rowFingerprint(const QVariantMap& row) {
    QByteArray buf;
    for (auto it = row.constBegin(); it != row.constEnd(); ++it) {
        buf += it.key().toUtf8();               // 列名
        buf += '\0';                            // 字段分隔符（防拼接歧义）
        buf += it.value().toString().toUtf8();  // 该列的值（统一以字符串形态参与指纹）
        buf += '\0';
    }
    return QCryptographicHash::hash(buf, QCryptographicHash::Sha1);
}

// ── getPkColumn —— 查某表的「单列主键」列名（结果缓存进 pkColCache_） ──────────
// 做什么：先查缓存；未命中则 PRAGMA table_info(<表>) 取出 pk 标志==1 的那一列列名。
// 为什么缓存：闭包构建期间同一张父表会被反复回查（多行引用同一父表），缓存省去重复 PRAGMA。
// PRAGMA table_info 结果列含义：[0]=cid [1]=name [2]=type [3]=notnull [4]=dflt [5]=pk。
//   pk 字段：0=非主键；单列主键恒为 1；复合主键为 1,2,3…。本函数只认 ==1，故复合主键表
//   会「找不到主键」返回空串——与「同步仅支持单列主键」的设计约束一致。
// 返回：主键列名；无单列主键 / PRAGMA 失败时返回空串（空串同样会被缓存，避免反复重查）。
QString FkClosureBuilder::getPkColumn(QSqlDatabase& rconn, const QString& table) {
    auto it = pkColCache_.constFind(table);
    if (it != pkColCache_.constEnd())
        return *it;  // 命中缓存（含「曾查过、确认无主键」的空串）

    QSqlQuery q(rconn);
    // M-11 fix: quote identifier (escapes embedded double-quotes).
    //   （用 quoteIdent 给表名加引号并转义内嵌双引号，取代裸拼，纵深防注入。）
    q.prepare(QStringLiteral("PRAGMA table_info(") + detail::SqlBuilder::quoteIdent(table) +
              QLatin1Char(')'));
    if (!q.exec())
        return {};  // PRAGMA 失败（如表不存在）→ 空串（注意：此分支故意不写缓存，留待下次重试）
    while (q.next()) {
        if (q.value(5).toInt() == 1) {  // 第 5 列是 pk 标志；==1 即单列主键
            const QString col = q.value(1).toString();  // 第 1 列是列名
            pkColCache_.insert(table, col);             // 写正缓存
            return col;
        }
    }
    pkColCache_.insert(table, {});  // 遍历完未见 pk==1 → 缓存「无单列主键」为空串
    return {};
}

// ── fetchRow —— 按主键抓取父行（闭包递归的「向上一步」） ──────────────────────
// 做什么：确定主键列 → 参数化 SELECT * WHERE pk=? LIMIT 1 → 命中则把整行写入 *row。
// 参数：table/pk 要回查的父表与父主键；row 输出整行（命中填满，未命中清空）；err 出参错误。
// 返回：true=查询成功执行（无论是否命中）；false=无主键列 / SQL 失败。
// 关键约定：「查无此行」返回 true 且 row 为空——把「父行是否真的存在」的判断交给调用方
//   （buildClosure 据此区分：空=闭包破损 E_SYNC_FK_CLOSURE_MISSING，非空=继续展开）。
//   这样设计是为了区分「执行错误」与「数据语义上的缺失」两类不同后果。
// 副作用：对 rconn 执行 PRAGMA（经 getPkColumn）+ 一次 SELECT。
bool FkClosureBuilder::fetchRow(QSqlDatabase& rconn, const QString& table, const QString& pk,
                                QVariantMap* row, QString* err) {
    const QString pkCol = getPkColumn(rconn, table);
    if (pkCol.isEmpty()) {
        if (err)
            *err = QStringLiteral("No PK column for table '%1'").arg(table);
        return false;  // 无单列主键（复合/无主键表）→ 无法按主键回查
    }

    QSqlQuery q(rconn);
    // M-11 fix: quote table and column identifiers.
    //   （表名、列名均加引号转义；主键值用占位符 ? 参数化绑定，三处都不裸拼用户数据。）
    // LIMIT 1：主键唯一，至多一行；显式 LIMIT 让意图更清晰，也防御异常多行。
    q.prepare(QStringLiteral("SELECT * FROM ") + detail::SqlBuilder::quoteIdent(table) +
              QStringLiteral(" WHERE ") + detail::SqlBuilder::quoteIdent(pkCol) +
              QStringLiteral(" = ? LIMIT 1"));
    q.addBindValue(pk);  // 主键值作为绑定参数，杜绝注入
    if (!q.exec()) {
        if (err)
            *err = q.lastError().text();  // 透传底层 SQL 错误
        return false;
    }

    if (!q.next()) {
        row->clear();
        return true;  // not found — caller checks
                      //   （查无此行：清空 row 并返回成功，由调用方判空决定后续。）
    }

    // 命中：把当前行的每个 (列名→值) 物化进 *row。
    const QSqlRecord rec = q.record();
    for (int i = 0; i < rec.count(); ++i)
        row->insert(rec.fieldName(i), q.value(i));
    return true;
}

// ── buildClosure —— 闭包补全的核心：沿外键向父表方向迭代 BFS ──────────────────
// 算法形态：虽名为「闭包递归」，实现上用「迭代式 BFS」而非函数递归，目的是避免
//   深依赖链（A→B→C→…）压爆调用栈。work 既是 BFS 队列也是最终结果容器——cursor 之前
//   是已处理的行，cursor 及其后是待处理的行；新发现的父行 append 到尾部，cursor 推进即可。
// 不变量：seen 记录所有「已入队（含被剪枝跳过）」的 (表,主键)，保证每个父行至多抓取一次，
//   既去重又天然处理「菱形依赖」（多个子行引用同一父行）与有向无环复用。
// 注意：本函数只「补全」闭包，不在这里做拓扑排序（那是 topoSort 的事）；append 进 work 的
//   顺序与最终应用顺序无关。
// 复杂度：O(闭包行数 × 平均每行 FK 数)，每次抓取父行近似 O(log n)（走主键索引）。
bool FkClosureBuilder::buildClosure(QSqlDatabase& rconn,
                                    const dbridge::detail::SchemaCatalog& catalog,
                                    ConsistencyCache& cache, bool pruneConsistent,
                                    QList<Entry>& work, QSet<QString>& seen, QString* err) {
    // Iterative BFS to avoid deep recursion.
    //   （迭代式 BFS，规避深递归爆栈。）
    int cursor = 0;
    while (cursor < work.size()) {  // 注意 work.size() 在循环体内会增长（追加新父行）
        const Entry& e = work[cursor++];  // 取出当前待处理行，并把游标推进一格

        // 查 FK 图：该表声明了哪些外键列指向哪些父表。
        const dbridge::detail::TableInfo* ti = catalog.table(e.table);
        if (!ti)
            continue;  // table not in catalog, skip FK expansion
                       //   （该表不在 catalog 中——视为「无可展开的 FK」，跳过即可。）

        for (const dbridge::detail::FkInfo& fk : ti->foreignKeys) {
            // Get the FK column value from this row.
            //   （从当前行里取出这个外键列的值，即「指向哪个父行」的引用值。）
            auto colIt = e.row.constFind(fk.fromColumn);
            if (colIt == e.row.constEnd() || colIt->isNull())
                continue;  // 该 FK 列缺失或为 NULL → 没有引用任何父行，跳过（合法情形）

            const QString refPk = colIt->toString();           // 被引用的父行主键值
            const QString key = entryKey(fk.refTable, refPk);  // 父行去重键
            if (seen.contains(key))
                continue;  // 该父行已入过队（或已被剪枝）→ 不重复抓取
            seen.insert(key);

            // 去只读连接里把这个父行的整行内容抓出来。
            QVariantMap depRow;
            if (!fetchRow(rconn, fk.refTable, refPk, &depRow, err))
                return false;  // SQL 执行错误（非「查无此行」）→ 直接失败

            if (depRow.isEmpty()) {
                // 子行声明引用了这个父行，但库里查不到它——闭包破损，无法自洽推送。
                if (err)
                    *err = QLatin1String(dbridge::err::E_SYNC_FK_CLOSURE_MISSING);
                return false;
            }

            // Prune if consistent.
            //   （一致性剪枝：若对端中心早已拥有内容相同的该父行，就不必再把它塞进闭包。）
            if (pruneConsistent) {
                const QByteArray fp = rowFingerprint(depRow);  // 算本地父行指纹
                if (cache.isConsistent(fk.refTable, refPk, fp))
                    continue;  // 与对端已确认指纹一致 → 跳过（剪枝），减少冗余推送
                               // 注意：被剪枝的父行仍已记入 seen，故其更上层父行不会再被展开——
                               // 这是有意为之：若父行已一致，约定其祖先也已一致，可整支剪掉。
            }

            // 该父行需要随推送带上：作为「依赖」（非选中）行加入工作表，待后续继续展开其 FK。
            Entry dep;
            dep.table = fk.refTable;
            dep.pk = refPk;
            dep.row = std::move(depRow);
            dep.isSelected = false;  // FK 闭包带入的依赖行（非用户直接选中）
            work.append(std::move(dep));
        }
    }
    return true;
}

// ── topoSort —— Kahn 拓扑排序：把闭包排成「父先子后」的线性序 ─────────────────
// 为什么需要：对端 apply 是逐行 UPSERT，若先写子行、其引用的父行尚未存在，就会触发
//   外键约束失败。把闭包按拓扑序（父表在前）排好，对端按序应用即天然满足外键约束。
// 建图方向：对「子行 i 通过 FK 引用父行 refIdx」这一关系，建有向边 refIdx → i
//   （父指向子），并令子行 i 的入度 +1。这样「入度为 0」恰好等价于「不依赖任何父行」，
//   可以最先应用。
// Kahn 算法：反复取出入度为 0 的节点赋予递增 topoIndex，并把其出边指向的子节点入度 -1，
//   减到 0 即可入队。若最终赋号数 order < n，说明有节点永远减不到 0 → 存在环。
// 错误模式：FK 成环（如 A→B→A）→ 无法定序，报 E_SYNC_FK_CYCLE_UNSUPPORTED。
// 复杂度：O(节点数 + 边数)；建图阶段需对每个节点查 FK 图与一次哈希定位。
// 副作用：成功时原地重排 entries（按 topoIndex 升序）并填好每行的 topoIndex。
bool FkClosureBuilder::topoSort(QList<Entry>& entries,
                                const dbridge::detail::SchemaCatalog& catalog, QString* err) {
    const int n = entries.size();

    // Map "table:pk" -> index in entries.
    //   （先建「(表,主键) → 在 entries 中的下标」索引，供下面按 FK 引用快速定位父行节点。）
    QHash<QString, int> idx;
    idx.reserve(n);
    for (int i = 0; i < n; ++i)
        idx.insert(entryKey(entries[i].table, entries[i].pk), i);

    // Build in-degree and adjacency (dep -> dependant).
    //   （构建入度数组与邻接表；邻接边方向是「被依赖者(父) → 依赖者(子)」。）
    QVector<int> inDegree(n, 0);   // inDegree[i] = 行 i 依赖的、且也在闭包内的父行数
    QVector<QVector<int>> adj(n);  // adj[父] 收集所有「依赖该父」的子行下标

    for (int i = 0; i < n; ++i) {
        const dbridge::detail::TableInfo* ti = catalog.table(entries[i].table);
        if (!ti)
            continue;  // 表不在 catalog → 无 FK 边可建
        for (const dbridge::detail::FkInfo& fk : ti->foreignKeys) {
            auto colIt = entries[i].row.constFind(fk.fromColumn);
            if (colIt == entries[i].row.constEnd() || colIt->isNull())
                continue;  // 该 FK 列缺失/为空 → 无引用，不建边
            const QString refKey = entryKey(fk.refTable, colIt->toString());
            auto refIt = idx.constFind(refKey);
            if (refIt == idx.constEnd())
                continue;  // 被引用父行不在闭包内（已被一致性剪枝）→ 无需建边、也不计入度
            const int refIdx = *refIt;
            // entries[i] depends on entries[refIdx] => refIdx must come first.
            //   （i 依赖 refIdx ⇒ refIdx 必须排在 i 之前：建边 refIdx→i，i 入度 +1。）
            adj[refIdx].push_back(i);
            inDegree[i]++;
        }
    }

    // Kahn's algorithm.
    //   （Kahn 算法主体：以队列驱动「逐层剥离入度为 0 的节点」。）
    QList<int> queue;
    for (int i = 0; i < n; ++i)
        if (inDegree[i] == 0)  // 不依赖任何（闭包内）父行 → 可最先应用
            queue.append(i);

    int order = 0;  // 已成功定序（赋号）的节点数，用作 topoIndex 计数器
    while (!queue.isEmpty()) {
        int cur = queue.takeFirst();
        entries[cur].topoIndex = order++;  // 赋予下一个拓扑序号
        for (int next : adj[cur]) {
            if (--inDegree[next] == 0)  // 父行 cur 已排定，子行 next 的依赖减一
                queue.append(next);     // 子行所有父行都排定后即可入队
        }
    }

    if (order != n) {
        // 仍有节点入度恒大于 0、永远进不了队 ⇒ 这些节点构成环，拓扑序不存在。
        if (err)
            *err = QString::fromLatin1(dbridge::err::E_SYNC_FK_CYCLE_UNSUPPORTED);
        return false;
    }

    // Sort entries by topoIndex.
    //   （按拓扑序号原地重排，使 entries 物理顺序即为「父先子后」的可应用顺序。）
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.topoIndex < b.topoIndex; });
    return true;
}

// ── build —— 对外入口：把「选中行」一步到位变成「拓扑有序的闭包清单」 ──────────
// 流程四步：
//   ① 选中行去重入工作表（isSelected=true）；
//   ② 若开启则递归补 FK 父行闭包（带一致性剪枝）；
//   ③ 校验总行数不超过 maxSize（防一次选择集过大拖垮传输/对端）；
//   ④ 拓扑排序，使父表在前；成功则把结果移交 *out。
// 顺序考量：上限校验放在补完闭包「之后、排序之前」——因为闭包会显著放大行数，必须按
//   最终实际推送规模来卡上限；而排序对规模无影响，故放最后。
// 短路语义：任一步失败立即返回 false（*err 已写明错误码），不产出 *out。
bool FkClosureBuilder::build(QSqlDatabase& rconn,
                             const QList<SelectionResolver::ResolveResult>& selected,
                             const dbridge::detail::SchemaCatalog& catalog, ConsistencyCache& cache,
                             qint64 maxSize, QList<Entry>* out, QString* err, bool includeFkDeps,
                             bool pruneConsistent) {
    QList<Entry> work;              // 既是 BFS 队列也是最终清单（见 buildClosure）
    work.reserve(selected.size());  // 预留至少选中行的容量（闭包会再增长）
    QSet<QString> seen;             // 全局去重集合：选中行与父行共用，杜绝重复

    // ① 把直接选中的行（去重后）作为 BFS 起点放入工作表，标记 isSelected=true。
    for (const auto& r : selected) {
        const QString key = entryKey(r.table, r.pk);
        if (seen.contains(key))
            continue;  // 选择集里重复指定同一行 → 只保留一份
        seen.insert(key);

        Entry e;
        e.table = r.table;
        e.pk = r.pk;
        e.row = r.row;
        e.isSelected = true;  // 用户直接选中（区别于 FK 闭包带入的依赖行）
        work.append(std::move(e));
    }

    // H-02 fix: only expand FK closure when includeFkDeps is true; honour pruneConsistent flag.
    //   （H-02 修复：仅当 includeFkDeps 为真才补 FK 闭包；并把 pruneConsistent 开关透传下去。）
    // ② 沿外键向父表递归补全依赖闭包（work 会在此处增长）。
    if (includeFkDeps) {
        if (!buildClosure(rconn, catalog, cache, pruneConsistent, work, seen, err))
            return false;
    }

    // ③ 闭包补全后，按最终行数卡上限。
    if (static_cast<qint64>(work.size()) > maxSize) {
        if (err)
            *err = QString::fromLatin1(dbridge::err::E_SYNC_SELECTION_TOO_LARGE);
        return false;
    }

    // ④ 拓扑排序（父先子后）；失败说明 FK 成环。
    if (!topoSort(work, catalog, err))
        return false;

    *out = std::move(work);  // 移交结果（移动语义，避免整份清单拷贝）
    return true;
}

}  // namespace dbridge::sync
