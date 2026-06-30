#include "ChangesetApplier.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QVariant>
#include <QVariantList>

#include "sql/SqlBuilder.h"
#include <sqlite3.h>

// ============================================================================
// ChangesetApplier.cpp — 把「对端 changeset」解码并应用到本地库的执行体
// ============================================================================
//
// 【职责】
//   实现 ChangesetApplier.h 声明的 apply 流程。核心是包一层 SQLite session 扩展的
//   sqlite3changeset_apply_v2()，并通过两个 C 回调把「dbridge 的冲突仲裁规则」嵌进
//   SQLite 的逐行应用过程：
//     · filterCb  (xFilter)   —— 决定某张表的变更收不收（表白名单 + 拒绝 __sync_* 元表）；
//     · conflictCb(xConflict) —— 决定某一行冲突时怎么办（REPLACE 落库 / OMIT 跳过 / ABORT 中止）。
//   apply 结束后再跑 updateWinnersFromChangeset()：更新 RowWinnerStore 的胜者记录，
//   并对「低 rank DELETE 误删高 rank 胜者行」做事后补救（见下）。
//
// 【在管线中的位置】
//   capture → payload → transport →〔本文件 apply〕→ conflict 仲裁 → peer ACK。
//   本文件位于 apply 阶段最底层，直接操作 SQLite C 句柄与 changeset 迭代器。
//
// 【协作者】
//   · RowWinnerStore  —— 持久化 (table, pkHash) → 当前胜者(rank/seq/origin/内容)；本类查它做仲裁。
//   · ConflictArbiter —— 仲裁「谁赢」的语义在 RowWinnerStore::beats() 中固化；本类内联了同样的
//                        rank/seq/origin 三元组比较（见 conflictCb / DELETE 补救处）。
//   · SqlBuilder      —— quoteIdent() 安全地给表名/列名加引号，拼补救用的 INSERT/UPSERT。
//   · sqlite3 C API   —— changeset 的迭代/取值只能走底层 C 接口，Qt 的 QSqlQuery 拿不到。
//
// 【贯穿全文的关键概念】
//   · origin/seq/rank：每条变更来自某个节点(origin)、在其流中有序号(seq)、该节点有等级(rank)。
//     仲裁三元组 = (rank, seq, origin)，按字典序取「最大」者为胜者（确定性、与到达顺序无关）。
//   · authoritative（权威下行）：中心→边缘的下发视为绝对真理，跳过一切仲裁，冲突恒 REPLACE，
//     且不更新 RowWinnerStore（边缘节点无条件接受，不参与 rank 竞争）。
//   · pkHash / contentHash：主键各列规范化后的 SHA-256 指纹（定位行）；整行内容的指纹（比对同值）。
//   · 各 fix 标记（H-01/H-04/H-05/M-01/M-04/C-2/C-3/C-08/C-11/C-12 …）：历史缺陷修复的留痕，
//     标注「为什么这里要这么写」，务必保留——它们记录了踩过的坑。
// ============================================================================

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// 辅助函数：从 changeset 迭代器的「当前行位置」构造 pkHash 与 contentHash 的原料
// helpers: build pkHash and contentHash from a changeset iterator position
// ---------------------------------------------------------------------------

namespace {

// extractHashMaterials —— 从 changeset 当前行的「新值」（DELETE 取「旧值」）抽取两份哈希原料。
//
// 【做什么】
//   遍历当前行的全部列，把每列的值序列化成字节并以 '\0' 分隔拼接，得到：
//     · pkMaterial      —— 仅由「主键列」(pkMask[i] 为真) 拼成，用于定位行的 pkHash；
//     · contentMaterial —— 由「全部列」拼成，用于判断两行内容是否相同的 contentHash。
// 【为什么用 '\0' 分隔】
//   防止「拼接歧义」：列值 ("ab","c") 与 ("a","bc") 若直接相连都得到 "abc"，加分隔符后
//   变为 "ab\0c\0" vs "a\0bc\0"，可区分。NULL 列写空字节，仅保留分隔符占位（见下）。
// 【参数】
//   iter            当前已定位到某一行的 changeset 迭代器（由调用方 next() 推进）。
//   nCol            该行列数（由 sqlite3changeset_op 给出）。
//   useNew          true 取「新值」(INSERT/UPDATE 之后的值)；false 取「旧值」(DELETE 前的值)。
//   pkMask          长度 nCol 的字节数组，pkMask[i] 非 0 表示第 i 列属于主键；可为 nullptr。
//   pkMaterial/contentMaterial  出参，函数内先 clear 再填充。
// 【返回/副作用】恒返回 true（保留接口对称性）；副作用仅为写两个出参缓冲。
// 【注意】本函数当前未被 apply 流程直接调用（conflictCb / updateWinnersFromChangeset 各自
//   内联了等价逻辑以同时构造 QVariantMap）；保留它作为同一编码约定的参考实现。
bool extractHashMaterials(sqlite3_changeset_iter* iter, int nCol, bool useNew,
                          unsigned char* pkMask, QByteArray* pkMaterial,
                          QByteArray* contentMaterial) {
    pkMaterial->clear();
    contentMaterial->clear();

    for (int i = 0; i < nCol; i++) {
        // 取第 i 列的值：sqlite3changeset_new/old 会把 sqlite3_value* 写进 val。
        sqlite3_value* val = nullptr;
        if (useNew)
            sqlite3changeset_new(iter, i, &val);
        else
            sqlite3changeset_old(iter, i, &val);
        if (!val)
            continue;  // 该列在此变更中「未提供」(UPDATE 只记录被改的列)，跳过

        // 按存储类型把值规范化成字节序列。注意 FLOAT 在此辅助函数中未单独处理
        // （见 conflictCb 中的内联版本会额外处理 SQLITE_FLOAT）。
        QByteArray colBytes;
        const int vtype = sqlite3_value_type(val);
        if (vtype == SQLITE_TEXT) {
            const char* txt = reinterpret_cast<const char*>(sqlite3_value_text(val));
            colBytes = QByteArray(txt ? txt : "");
        } else if (vtype == SQLITE_INTEGER) {
            colBytes = QByteArray::number(static_cast<qint64>(sqlite3_value_int64(val)));
        } else if (vtype == SQLITE_BLOB) {
            const void* b = sqlite3_value_blob(val);
            int blen = sqlite3_value_bytes(val);
            if (b && blen > 0)
                colBytes = QByteArray(static_cast<const char*>(b), blen);
        }
        // NULL 值保持为空字节——仅以下面的 '\0' 分隔符占位参与拼接。
        // NULL stays as empty bytes — included as separator only.

        // 主键列同时进 pkMaterial；所有列都进 contentMaterial。
        if (pkMask && pkMask[i]) {
            pkMaterial->append(colBytes);
            pkMaterial->append('\0');
        }
        contentMaterial->append(colBytes);
        contentMaterial->append('\0');
    }
    return true;
}

// computePkHashStr —— 把原料压成定位行用的 pkHash 十六进制串。
//   优先用 pkMaterial（主键列）；若该表无主键信息（pkMaterial 为空）则退化为用整行
//   contentMaterial 作键。取 SHA-256 的前 16 字节（128 位足够避免碰撞）再转十六进制。
QString computePkHashStr(const QByteArray& pkMaterial, const QByteArray& contentMaterial) {
    const QByteArray& src = pkMaterial.isEmpty() ? contentMaterial : pkMaterial;
    QByteArray h = QCryptographicHash::hash(src, QCryptographicHash::Sha256).left(16);
    return QString::fromLatin1(h.toHex());
}

}  // namespace

// ---------------------------------------------------------------------------
// 公有入口：apply —— 把一段 changeset 应用到本地库（含冲突仲裁与事后补救）
// public: apply
// ---------------------------------------------------------------------------

// apply —— 解码并应用一段来自某个 origin 的 changeset。
//
// 【做什么 / 三步走】
//   ① 准备回调上下文 ConflictCtx（把 origin/rank/seq、胜者表、策略、表白名单等打包进去）；
//   ② 调 sqlite3changeset_apply_v2()，由 SQLite 逐行落库，途中回调 filterCb/conflictCb；
//   ③ 非权威路径再跑 updateWinnersFromChangeset()，更新胜者表并做低 rank DELETE 补救。
// 【为什么这么设计】见 .h 类头：SQLite 无法按行预过滤 changeset，故采用「先应用、后补救」，
//   且补救必须与 apply 处于同一事务中——补救失败即返回 false，逼调用方回滚整个事务。
// 【参数】
//   h          底层 sqlite3* 句柄（changeset API 只认它，与 wconn 须指向同一数据库连接）。
//   wconn      Qt 写连接，用于回调里跑 PRAGMA / 补救 SQL（与 h 同库同事务）。
//   changeset  待应用的二进制变更集。
//   origin/originRank/originSeq  本段变更的来源标识、来源节点等级、来源流序号（仲裁三元组）。
//   winners    行级胜者表（读在位者 / 写挑战者 / 取 winningContent 做恢复）。
//   opts       策略：authoritative（权威下行）与 conflictPolicy（非权威时的冲突策略）。
//   syncTables 表白名单；空 = 接受所有表（仅测试用，见 filterCb）。
//   out        出参：本次应用的统计 ApplyOutcome（applied/conflicts/ignored/rebaseBuffer）。
//   err        出参：失败时写入可读错误文本。
// 【返回】成功 true；rc 失败或补救失败返回 false。
// 【副作用】直接修改本地库行数据与 __sync_row_winner；必须在调用方已开启的写事务内调用。
// 【错误模式】apply_v2 返回非 OK/ROW → false；DELETE 补救失败 → false（调用方须回滚，绝不可
//   提交/ACK 这个错误的终态，否则低 rank DELETE 会「悄悄获胜」）。
// 【线程】不可跨线程并发复用同一 sqlite3 句柄。
bool ChangesetApplier::apply(sqlite3* h, QSqlDatabase& wconn, const QByteArray& changeset,
                             const QString& origin, int originRank, qint64 originSeq,
                             RowWinnerStore& winners, const ApplyOptions& opts,
                             const QStringList& syncTables, ApplyOutcome* out, QString* err) {
    if (!out)
        return false;       // out 是必填出参，缺失视为编程错误
    *out = ApplyOutcome{};  // 清零统计，确保「失败提前返回」时不残留上次的计数

    // ── 组装回调上下文：apply_v2 会把这个 &ctx 原样回传给 filterCb / conflictCb ──
    // 两个回调共用同一份 ctx（即 SQLite 文档里的 pCtx），故表白名单(xFilter 用)与
    // 仲裁状态(xConflict 用)都挂在它身上。
    ConflictCtx ctx;
    ctx.self = this;
    ctx.h = h;
    ctx.wconn = &wconn;
    ctx.origin = origin;
    ctx.rank = originRank;
    ctx.seq = originSeq;
    ctx.winners = &winners;
    ctx.authoritative = opts.authoritative;
    ctx.outcome = out;

    // H-04 fix: pass syncTables into ConflictCtx so the shared pCtx carries both
    // xFilter (table allow-list) and xConflict (row-winner arbitration) state.
    // 【H-04 修复】把 syncTables 传入 ConflictCtx，使同一份 pCtx 同时承载
    //   xFilter（表白名单）与 xConflict（行胜者仲裁）两套状态。
    //   空列表 → 存 nullptr（filterCb 见 nullptr 即「接受所有表」）。
    ctx.syncTables = syncTables.isEmpty() ? nullptr : &syncTables;

    // M-01 fix: carry conflict policy into the callback for non-authoritative resolution.
    // 【M-01 修复】把冲突策略带进回调，供非权威路径据此决定 SourceWins/TargetWins/Manual。
    ctx.conflictPolicy = opts.conflictPolicy;

    // C-11 fix: there is no reliable public SQLite API to rebuild a row-filtered changeset, so
    // we do NOT pre-filter. Instead, low-rank DELETE protection is enforced AFTER apply by
    // updateWinnersFromChangeset(): it restores any high-rank row erased by a dominated DELETE,
    // within the SAME transaction. If the restore fails, apply() returns false and the caller
    // rolls back — so a low-rank DELETE can never win (G-01/FR-6).
    // 【C-11 修复】SQLite 没有可靠的公共 API 能「按行重建一个被过滤的 changeset」，所以
    //   这里不做预过滤。低 rank DELETE 的保护改为 apply 之后由 updateWinnersFromChangeset()
    //   在「同一个事务内」完成：它会把被某个受支配(dominated)的 DELETE 误删的高 rank 行恢复
    //   回去；恢复失败则 apply() 返回 false、调用方回滚——于是低 rank 的 DELETE 永远赢不了
    //   (对应需求 G-01 / FR-6)。
    void* pRebase = nullptr;  // apply_v2 分配的 rebase 缓冲（需我们用 sqlite3_free 释放）
    int nRebase = 0;

    // ── 核心调用：让 SQLite 逐行应用 changeset ──
    // 参数顺序：句柄、字节数、数据指针、xFilter、xConflict、pCtx、pRebase 出参、nRebase
    // 出参、flags。 NOSAVEPOINT：不让 apply_v2 自己开 SAVEPOINT——因为我们要求整个 apply
    // 处于调用方的外层
    //   写事务里，由调用方统一 commit/rollback（与上面的「事后补救须同事务」一致）。
    // const_cast：changeset API 形参非 const，但我们承诺不修改其内容，故安全去 const。
    int rc = sqlite3changeset_apply_v2(
        h, changeset.size(), const_cast<void*>(static_cast<const void*>(changeset.constData())),
        &filterCb,  // H-04: table filter — uses same pCtx as conflictCb
        &conflictCb, &ctx, &pRebase, &nRebase, SQLITE_CHANGESETAPPLY_NOSAVEPOINT);

    // 仅非权威路径才回收 rebase 缓冲：本地有行胜出时，需把这段差异回送给对端令其变基。
    // 权威下行恒 REPLACE、不产生竞争，故其 rebase 输出无意义，直接丢弃。
    if (pRebase && nRebase > 0 && !opts.authoritative) {
        out->rebaseBuffer = QByteArray(static_cast<const char*>(pRebase), nRebase);
    }
    if (pRebase)
        sqlite3_free(pRebase);  // 无论是否采用都必须释放，避免内存泄漏

    // SQLITE_ROW 在此上下文中也算成功（apply_v2 可能以 ROW 形式返回迭代结束态）。
    if (rc != SQLITE_OK && rc != SQLITE_ROW) {
        if (err)
            *err = QStringLiteral("sqlite3changeset_apply_v2 rc=%1").arg(rc);
        return false;
    }

    // C-12 fix: post-apply winner update + low-rank DELETE recovery. Returns false on a failed
    // recovery; the caller must roll back so the bad terminal state is never committed/ACKed.
    // Authoritative (down-link) applies skip winner arbitration entirely.
    // 【C-12 修复】apply 之后：更新胜者表 + 低 rank DELETE 恢复。恢复失败返回 false，调用方
    //   必须回滚，绝不让错误的终态被提交/ACK。权威下行（authoritative）完全跳过胜者仲裁——
    //   边缘无条件接受中心下发，不参与 rank 竞争，也不写 __sync_row_winner。
    if (!opts.authoritative) {
        if (!updateWinnersFromChangeset(changeset, origin, originRank, originSeq, winners, wconn,
                                        syncTables, err))
            return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// 私有：xFilter 回调（H-04）—— 决定「某张表的变更收不收」
// private: xFilter callback (H-04)
// ---------------------------------------------------------------------------

// filterCb —— SQLite apply_v2 的 xFilter 回调。对 changeset 中出现的每张表调用一次。
// 【返回】1 = 接受该表的全部变更；0 = 整张表跳过（其变更不会被应用）。
// 【为什么需要它】changeset 可能携带本节点不参与同步的表，或携带内部元表；这里做白名单门控。
int ChangesetApplier::filterCb(void* ctx, const char* tblName) {
    // M-04 fix: unconditionally reject __sync_* internal tables BEFORE consulting the
    // allow-list.  This prevents a misconfigured allow-list from accidentally allowing
    // writes to sync meta tables, regardless of the syncTables configuration.
    // 【M-04 修复】在查白名单「之前」无条件拒绝 __sync_* 内部元表。这样即便白名单被误配置成
    //   包含了某张同步元表，也绝不会让对端 changeset 直接改写我们的同步元数据（如胜者表、
    //   changelog），从根上堵死这一污染途径。qstrncmp 比较前 7 个字符 "__sync_"。
    if (qstrncmp(tblName, "__sync_", 7) == 0)
        return 0;
    auto* c = static_cast<ConflictCtx*>(ctx);  // 还原成 apply() 里塞进去的上下文
    if (!c->syncTables)
        return 1;  // 白名单为空（nullptr）→ 接受所有业务表（仅测试模式）  accept all when list is
                   // empty
    // 否则只接受白名单内的表。UTF-8 转换以兼容非 ASCII 表名。
    return c->syncTables->contains(QString::fromUtf8(tblName)) ? 1 : 0;
}

// H-01 fix: shared allow-list predicate used by filterCb, updateWinnersFromChangeset,
// and extractMutations (via CapturedWriteTemplate) — all three paths reject the same tables.
// 【H-01 修复】共享的「表是否被允许同步」判定。filterCb（应用阶段）、
//   updateWinnersFromChangeset（胜者更新阶段）、extractMutations（经 CapturedWriteTemplate
//   的捕获阶段）三条路径都调用它，确保三处对「拒绝哪些表」的判断完全一致——否则可能出现
//   「应用了某表但没更新其胜者」之类的状态错位。
// 【规则】① __sync_* 元表一律拒绝；② 白名单为空 = 接受所有（仅测试）；③ 否则按白名单成员判定。
// static
bool ChangesetApplier::isAllowedSyncTable(const QString& table, const QStringList& syncTables) {
    if (table.startsWith(QLatin1String("__sync_")))
        return false;
    if (syncTables.isEmpty())
        return true;  // empty list = accept all (test-only mode)  空列表 = 接受所有（仅测试模式）
    return syncTables.contains(table);
}

// ---------------------------------------------------------------------------
// 私有：冲突回调 —— 决定「某一行冲突时怎么处理」
// private: conflict callback
// ---------------------------------------------------------------------------

// conflictCb —— SQLite apply_v2 的 xConflict 回调。每当某一行无法「干净前进」时调用。
//
// 【什么叫冲突】session 应用一行 UPDATE/DELETE 时会比对「本地当前值」与 changeset 里记录的
//   「old 值（incoming.old）」：
//     · 二者相等 → 干净前进（SQLite 直接应用，不进本回调）；
//     · 二者不等 → DATA 冲突（本回调被调，conflict==SQLITE_CHANGESET_DATA）。
//   其它冲突类型见下面的 case。
// 【返回值的含义】告诉 SQLite 这一行最终怎么办：
//     · SQLITE_CHANGESET_REPLACE —— 用 changeset 的 new 值覆盖本地（挑战者获胜/落库）；
//     · SQLITE_CHANGESET_OMIT    —— 跳过本行，保留本地原值（挑战者判负/被策略否决）；
//     · SQLITE_CHANGESET_ABORT   —— 中止整个 apply（约束冲突等不可恢复的情况）。
// 【线程/重入】由 SQLite 在 apply_v2 调用栈内同步回调，使用 apply() 里准备的同一个 ctx。
int ChangesetApplier::conflictCb(void* ctx, int conflict, sqlite3_changeset_iter* iter) {
    auto* c = static_cast<ConflictCtx*>(ctx);

    switch (conflict) {
        // ── DATA / CONFLICT：本地值与 incoming.old 不一致（真正需要仲裁的「数据冲突」）──
        case SQLITE_CHANGESET_DATA:
        case SQLITE_CHANGESET_CONFLICT: {
            c->outcome->conflicts++;  // 先记一笔冲突计数（无论最终谁赢）

            // 权威下行：绝对真理，无条件覆盖本地、不查胜者表、不参与仲裁。
            if (c->authoritative) {
                c->outcome->applied++;
                return SQLITE_CHANGESET_REPLACE;
            }

            // M-01 fix: apply conflict policy before consulting RowWinnerStore.
            // TargetWins and Manual both OMIT the challenger (i.e. local row wins).
            // 【M-01 修复】在查 RowWinnerStore 之前先看冲突策略：
            //   TargetWins（本地优先）与 Manual（人工处理）都让本地行获胜，即 OMIT 挑战者。
            //   只有 SourceWins（默认）才继续往下走 rank/seq/origin 仲裁。
            if (c->conflictPolicy == ConflictPolicy::TargetWins ||
                c->conflictPolicy == ConflictPolicy::Manual) {
                c->outcome->ignored++;
                return SQLITE_CHANGESET_OMIT;
            }

            // Non-authoritative (SourceWins default): consult RowWinnerStore.
            // 非权威、SourceWins：取出本行的表名/列数/操作类型，准备做胜者仲裁。
            const char* tblName = nullptr;
            int nCol = 0;
            int opOut = 0;
            int bIndirectOut = 0;  // 是否为「间接」变更（触发器引发等）；此处不使用
            sqlite3changeset_op(iter, &tblName, &nCol, &opOut, &bIndirectOut);
            const QString table = QString::fromUtf8(tblName ? tblName : "");

            // Use PK mask to build pkHash from PK columns only (I-06).
            // H-05 fix: use canonical type-tagged encoding via RowWinnerStore::pkHash()
            // so that all pkHash producers (conflict, updateWinnersFromChangeset, baseline)
            // are in the same key space.
            // 取主键掩码：pkMask[i] 非 0 表示第 i 列是主键。只用主键列算 pkHash 来定位行(I-06)。
            // 【H-05 修复】统一走 RowWinnerStore::pkHash() 的「带类型标签的规范化编码」，使三处
            //   pkHash 生产者（本冲突回调、updateWinnersFromChangeset、baseline 基线）落在同一
            //   键空间——否则同一行在不同路径算出不同 hash，胜者表查不到、仲裁失效。
            unsigned char* pkMask = nullptr;
            sqlite3changeset_pk(iter, &pkMask, nullptr);

            // Lazily resolve column names for this table.
            // 惰性解析「列索引 → 列名」映射并缓存（changeset 只给列下标，pkHash 需要列名）。
            // 缓存挂在 ctx 上，按表名 key，同一次 apply 内多行复用，避免每行都跑 PRAGMA。
            auto& cache = c->colNameCache;
            if (!cache.contains(table)) {
                QStringList names;
                QSqlQuery ti(*c->wconn);
                // PRAGMA table_info 返回 (cid, name, type, notnull, dflt, pk)；这里取 cid→name。
                // 表名里的双引号要转义成两个双引号，防止 SQL 注入/语法破坏。
                ti.prepare(
                    QStringLiteral("PRAGMA table_info(\"%1\")")
                        .arg(QString(table).replace(QLatin1Char('"'), QLatin1String("\"\""))));
                if (ti.exec()) {
                    QMap<int, QString> cidMap;
                    while (ti.next())
                        cidMap.insert(ti.value(0).toInt(), ti.value(1).toString());
                    // 按列下标 0..nCol-1 取名；查不到则退化成占位名 "_col_N"。
                    for (int i = 0; i < nCol; ++i)
                        names.append(cidMap.value(i, QStringLiteral("_col_%1").arg(i)));
                } else {
                    // PRAGMA 失败（表已不存在等）→ 全部用占位名，至少保证后续不崩。
                    for (int i = 0; i < nCol; ++i)
                        names.append(QStringLiteral("_col_%1").arg(i));
                }
                cache.insert(table, names);
            }
            const QStringList& colNames = cache.value(table);

            // Build PK QVariantMap and content material for hash computation.
            // 一次遍历同时构造两样东西：
            //   · pkMap          —— {列名: 值}，仅含主键列，交给 RowWinnerStore::pkHash()
            //   算定位哈希； · contentMaterial —— 全列值以 '\0' 分隔拼接，用来算挑战者的
            //   contentHash。
            QVariantMap pkMap;
            QByteArray contentMaterial;
            for (int i = 0; i < nCol; ++i) {
                sqlite3_value* val = nullptr;
                sqlite3changeset_new(iter, i, &val);  // DATA 冲突取「新值」(欲写入的值)
                const QString cname =
                    (i < colNames.size()) ? colNames[i] : QStringLiteral("_col_%1").arg(i);
                QVariant qv;          // 该列值的 Qt 表示（进 pkMap）
                QByteArray colBytes;  // 该列值的字节表示（进 contentMaterial）
                if (val) {
                    // 按存储类型分别转换。注意各类型的字节化要「确定性」——尤其 FLOAT 用 'g',17
                    // 保证全精度且跨平台一致，否则两端算出的 contentHash 会不同。
                    const int vt = sqlite3_value_type(val);
                    if (vt == SQLITE_TEXT) {
                        const char* txt = reinterpret_cast<const char*>(sqlite3_value_text(val));
                        qv = QVariant(QString::fromUtf8(txt ? txt : ""));
                        colBytes = qv.toString().toUtf8();
                    } else if (vt == SQLITE_INTEGER) {
                        const qlonglong iv = static_cast<qlonglong>(sqlite3_value_int64(val));
                        qv = QVariant(iv);
                        colBytes = QByteArray::number(iv);
                    } else if (vt == SQLITE_FLOAT) {
                        qv = QVariant(sqlite3_value_double(val));
                        colBytes = QByteArray::number(qv.toDouble(), 'g', 17);
                    } else if (vt == SQLITE_BLOB) {
                        const void* b = sqlite3_value_blob(val);
                        const int bl = sqlite3_value_bytes(val);
                        qv = (b && bl > 0) ? QVariant(QByteArray(static_cast<const char*>(b), bl))
                                           : QVariant(QByteArray());
                        colBytes = qv.toByteArray();
                    }
                    // 其余（NULL）：qv 留空、colBytes 留空，仅靠下面的 '\0' 占位。
                }
                if (pkMask && pkMask[i])
                    pkMap[cname] = qv;  // 仅主键列进 pkMap
                contentMaterial.append(colBytes);
                contentMaterial.append('\0');  // 分隔符，防拼接歧义
            }

            // pkHash：有主键则用规范化的 pkMap；无主键信息则退化为整行 contentMaterial 的哈希。
            // 两者都取 SHA-256 前 16 字节转十六进制，确保与其它路径同空间(H-05)。
            const QString pkHashStr =
                pkMap.isEmpty()
                    ? QString::fromLatin1(
                          QCryptographicHash::hash(contentMaterial, QCryptographicHash::Sha256)
                              .left(16)
                              .toHex())
                    : RowWinnerStore::pkHash(pkMap);
            // contentHash：挑战者行内容的指纹，存进胜者记录用于后续快速比对是否同值。
            QByteArray contentH =
                QCryptographicHash::hash(contentMaterial, QCryptographicHash::Sha256).left(16);

            // ── 组装「挑战者」：本段 changeset 的来源三元组 + 内容指纹 ──
            RowWinner challenger;
            challenger.origin = c->origin;
            challenger.rank = c->rank;
            challenger.originSeq = c->seq;
            challenger.contentHash = contentH;

            // 读取该行「在位者」（当前胜者）；查无记录则返回 rank==INT_MIN 的哨兵。
            RowWinner incumbent = c->winners->get(*c->wconn, table, pkHashStr);
            // H-01 fix: add originId tie-breaker when rank and seq both match, so the
            // conflict outcome is deterministic regardless of arrival order.
            // 【仲裁判定 / 与 RowWinnerStore::beats() 同义】挑战者获胜当且仅当：
            //   ① 在位者不存在（哨兵 INT_MIN）——空位直接占据；或
            //   ② 挑战者 rank 更高；或
            //   ③ rank 相同且挑战者 seq 更大（更新）；或
            //   ④ 【H-01 修复】rank、seq 都相同时，比 origin 字符串字典序——纯确定性兜底，
            //      保证任意到达顺序下所有节点算出同一胜者。
            bool win =
                (incumbent.rank == INT_MIN) || (challenger.rank > incumbent.rank) ||
                (challenger.rank == incumbent.rank && challenger.originSeq > incumbent.originSeq) ||
                (challenger.rank == incumbent.rank && challenger.originSeq == incumbent.originSeq &&
                 challenger.origin > incumbent.origin);
            if (win) {
                // H-01 fix: do NOT write the incomplete challenger here (winningContent is empty
                // at this point).  updateWinnersFromChangeset() runs after apply_v2 and writes the
                // full RowWinner including winningContent via putOrRefill().
                // 【H-01 修复】此处「不」写胜者记录——因为现在拿不到完整行内容(winningContent
                // 为空)。
                //   真正的胜者写入推迟到 apply_v2 结束后的 updateWinnersFromChangeset()，它带着
                //   完整 JSON 行内容用 putOrRefill() 落库（DELETE 恢复要靠这份内容）。这里只需
                //   告诉 SQLite「让挑战者覆盖本地」。
                c->outcome->applied++;
                return SQLITE_CHANGESET_REPLACE;
            }
            // 挑战者判负：保留本地行，跳过本行写入。
            c->outcome->ignored++;
            return SQLITE_CHANGESET_OMIT;
        }

        // ── NOTFOUND：要 UPDATE/DELETE 的行在本地根本不存在 → 无对象可改，跳过 ──
        case SQLITE_CHANGESET_NOTFOUND:
            c->outcome->ignored++;
            return SQLITE_CHANGESET_OMIT;

        // ── 外键/其它约束冲突：不可恢复，中止整个 apply（调用方回滚，回报 E_SYNC_APPLY_* ）──
        case SQLITE_CHANGESET_FOREIGN_KEY:
        case SQLITE_CHANGESET_CONSTRAINT:
            return SQLITE_CHANGESET_ABORT;

        // ── 其它未预期的冲突类型：保守地跳过本行 ──
        default:
            return SQLITE_CHANGESET_OMIT;
    }
}

// ---------------------------------------------------------------------------
// 私有：updateWinnersFromChangeset (I-03) —— apply 之后的胜者更新 + 低 rank DELETE 补救
// private: updateWinnersFromChangeset (I-03)
// ---------------------------------------------------------------------------

// updateWinnersFromChangeset —— apply_v2 跑完后，再独立遍历一遍同一段 changeset，做两件事：
//   ① INSERT/UPDATE 行 → 把挑战者(及其完整行内容)写入 RowWinnerStore（用 putOrRefill）；
//   ② DELETE 行       → 若该删除来自「被当前胜者支配(dominated)」的低 rank 节点，则把刚被
//      apply_v2 真实删掉的高 rank 胜者行「恢复」回去（见 C-12）。
// 【为什么要再遍历一遍】conflictCb 里拿不到完整行内容（见 H-01），无法在回调中写完整胜者；
//   且 DELETE 补救必须在「行已被真实删除之后」才能判断要不要恢复，所以只能 apply 之后做。
// 【关键不变式】本函数与 apply_v2 共用调用方的同一个写事务：任一恢复失败 → 返回 false →
//   调用方回滚 → 该 DELETE 连同整段 apply 一起被撤销，绝不让低 rank DELETE 偷偷生效。
// 【参数】changeset/origin/rank/seq 同 apply()；winners 胜者表；wconn 写连接；
//   syncTables 表白名单（与 filterCb 同源）；err 出参错误文本。
// 【返回】全部处理成功(含无需恢复)返回 true；任一必需的恢复失败返回 false 并置 *err。
bool ChangesetApplier::updateWinnersFromChangeset(const QByteArray& changeset,
                                                  const QString& origin, int rank, qint64 seq,
                                                  RowWinnerStore& winners, QSqlDatabase& wconn,
                                                  const QStringList& syncTables, QString* err) {
    // 自己开一个 changeset 迭代器从头扫（与 apply_v2 内部的迭代相互独立）。
    sqlite3_changeset_iter* iter = nullptr;
    int rc =
        sqlite3changeset_start(&iter, changeset.size(),
                               const_cast<void*>(static_cast<const void*>(changeset.constData())));
    if (rc != SQLITE_OK || !iter)
        return true;  // 起不了迭代器（如空 changeset）→ 无可遍历，不算失败  nothing to iterate —
                      // not a failure

    // H-04 fix: column name cache is per-call (not static) so schema changes and
    // multi-database use in the same process do not corrupt each other's PK resolution.
    // 【H-04 修复】列名缓存是「每次调用」级（局部变量，非 static）。这样不同调用之间、以及
    //   同进程多库/表结构变更，不会相互污染彼此的列名→主键解析。
    QMap<QString, QStringList> uwColCache;
    bool ok = true;
    // 逐行推进迭代器；sqlite3changeset_next 返回 SQLITE_ROW 表示还有下一行。
    while (sqlite3changeset_next(iter) == SQLITE_ROW) {
        const char* tbl = nullptr;
        int nCol = 0, op = 0, indirect = 0;
        sqlite3changeset_op(iter, &tbl, &nCol, &op, &indirect);  // 取表名/列数/操作/是否间接

        unsigned char* pkMask = nullptr;
        sqlite3changeset_pk(iter, &pkMask, nullptr);  // 取主键掩码

        const QString tableName = QString::fromUtf8(tbl ? tbl : "");

        // H-01 fix: skip tables rejected by the allow-list so __sync_* meta tables and
        // non-sync tables are never written to __sync_row_winner.
        // 【H-01 修复】白名单拒绝的表(含 __sync_* 元表、非同步表)直接跳过，绝不写进
        //   __sync_row_winner——与 filterCb 的判定保持一致，防止胜者表被污染。
        if (!isAllowedSyncTable(tableName, syncTables))
            continue;

        // H-05 fix: use canonical type-tagged pkHash, consistent with conflictCb.
        // 【H-05 修复】pkHash 编码与 conflictCb 完全一致。DELETE 取「旧值」(被删前的值)，
        //   INSERT/UPDATE 取「新值」——pkHash 由此区分。
        const bool useNew = (op != SQLITE_DELETE);

        // Resolve column names for this table (lazily cached across rows in this changeset).
        // 解析本表「列下标 → 列名」并惰性缓存（逻辑同 conflictCb，但缓存是本函数局部的
        // uwColCache）。
        if (!uwColCache.contains(tableName)) {
            QStringList names;
            QSqlQuery ti(wconn);
            ti.prepare(
                QStringLiteral("PRAGMA table_info(\"%1\")")
                    .arg(QString(tableName).replace(QLatin1Char('"'), QLatin1String("\"\""))));
            if (ti.exec()) {
                QMap<int, QString> cidMap;
                while (ti.next())
                    cidMap.insert(ti.value(0).toInt(), ti.value(1).toString());
                for (int i = 0; i < nCol; ++i)
                    names.append(cidMap.value(i, QStringLiteral("_col_%1").arg(i)));
            } else {
                for (int i = 0; i < nCol; ++i)
                    names.append(QStringLiteral("_col_%1").arg(i));
            }
            uwColCache.insert(tableName, names);
        }
        const QStringList& colNames = uwColCache.value(tableName);

        // 同 conflictCb：一次遍历构造 pkMap(定位) 与 contentMaterial(内容指纹)。
        // 区别在于这里按 useNew 取新/旧值——DELETE 行用旧值才能算出它「删的是哪一行」。
        QVariantMap pkMap;
        QByteArray contentMaterial;
        for (int i = 0; i < nCol; ++i) {
            sqlite3_value* val = nullptr;
            if (useNew)
                sqlite3changeset_new(iter, i, &val);
            else
                sqlite3changeset_old(iter, i, &val);
            const QString cname =
                (i < colNames.size()) ? colNames[i] : QStringLiteral("_col_%1").arg(i);
            QVariant qv;
            QByteArray colBytes;
            if (val) {
                const int vt = sqlite3_value_type(val);
                if (vt == SQLITE_TEXT) {
                    const char* txt = reinterpret_cast<const char*>(sqlite3_value_text(val));
                    qv = QVariant(QString::fromUtf8(txt ? txt : ""));
                    colBytes = qv.toString().toUtf8();
                } else if (vt == SQLITE_INTEGER) {
                    const qlonglong iv = static_cast<qlonglong>(sqlite3_value_int64(val));
                    qv = QVariant(iv);
                    colBytes = QByteArray::number(iv);
                } else if (vt == SQLITE_FLOAT) {
                    qv = QVariant(sqlite3_value_double(val));
                    colBytes = QByteArray::number(qv.toDouble(), 'g', 17);  // 全精度，跨平台一致
                } else if (vt == SQLITE_BLOB) {
                    const void* b = sqlite3_value_blob(val);
                    const int bl = sqlite3_value_bytes(val);
                    qv = (b && bl > 0) ? QVariant(QByteArray(static_cast<const char*>(b), bl))
                                       : QVariant(QByteArray());
                    colBytes = qv.toByteArray();
                }
            }
            if (pkMask && pkMask[i])
                pkMap[cname] = qv;
            contentMaterial.append(colBytes);
            contentMaterial.append('\0');
        }

        // 定位行用的 pkHash；与 conflictCb 同算法，保证同空间。
        const QString pkHashStr =
            pkMap.isEmpty() ? QString::fromLatin1(QCryptographicHash::hash(
                                                      contentMaterial, QCryptographicHash::Sha256)
                                                      .left(16)
                                                      .toHex())
                            : RowWinnerStore::pkHash(pkMap);

        if (op == SQLITE_DELETE) {
            // C-12 fix: a DELETE from a node dominated by the current winner must NOT erase the
            // high-rank row. apply_v2 already executed the DELETE; restore the winning row here,
            // in the same transaction. If we cannot restore, return false so the caller rolls
            // back the whole apply (the DELETE is undone, applied_vector is not advanced, no ACK).
            // 【C-12 修复 / 本函数最微妙处】来自「被当前胜者支配」的节点的 DELETE，绝不能抹掉那个
            //   高 rank 行。但 apply_v2 已经把它真删了——所以在这里、在同一事务内，把胜者行重新
            //   插回去。若插不回来 → 返回 false → 调用方回滚整段 apply（DELETE 被撤销、应用进度
            //   向量不前进、不发 ACK），从而保证低 rank DELETE 永远赢不了。
            RowWinner incumbent = winners.get(wconn, tableName, pkHashStr);
            // H-01 fix: account for the originId tie-breaker when rank and seq match.
            // 【支配判定 dominated】——本 DELETE 是否「输给」当前在位胜者（即应被否决）：
            //   在位者存在(非哨兵)，且本 DELETE 的三元组 (rank,seq,origin) 严格小于在位者
            //   ——rank 更低，或 rank 同而 seq 更小，或都同而 origin 字典序更小(H-01 兜底)。
            //   注意这是「严格小于」：等于在位者(同一来源)时不算支配，DELETE 照常生效。
            const bool dominated =
                incumbent.rank != INT_MIN &&
                ((rank < incumbent.rank) || (rank == incumbent.rank && seq < incumbent.originSeq) ||
                 (rank == incumbent.rank && seq == incumbent.originSeq &&
                  origin < incumbent.origin));
            if (dominated) {
                // 需要恢复，但胜者表里没有缓存这一行的内容 → 无从恢复。
                // 此时宁可让整段 apply 失败回滚，也不能让这个低 rank DELETE「悄悄获胜」。
                if (incumbent.winningContent.isEmpty()) {
                    // We know the row should survive but have no content to restore it with.
                    // Fail the apply so the DELETE is rolled back rather than silently winning.
                    if (err)
                        *err = QStringLiteral(
                                   "E_SYNC_APPLY_CONSTRAINT: low-rank DELETE on %1 would erase "
                                   "high-rank winner (origin=%2 rank=%3) with no stored content "
                                   "to restore")
                                   .arg(tableName, incumbent.origin)
                                   .arg(incumbent.rank);
                    ok = false;
                    break;
                }
                // winningContent 是一段 JSON 对象（列名→类型标签值），解码出来准备拼 INSERT。
                QJsonDocument doc = QJsonDocument::fromJson(incumbent.winningContent.toUtf8());
                if (!doc.isObject()) {
                    // 缓存内容损坏/格式异常 → 同样以失败回滚处理，绝不放任 DELETE 生效。
                    if (err)
                        *err = QStringLiteral(
                                   "E_SYNC_APPLY_CONSTRAINT: winning_content for %1 is "
                                   "not a JSON object")
                                   .arg(tableName);
                    ok = false;
                    break;
                }
                const QJsonObject obj = doc.object();
                // 把 JSON 对象的每个键值展开为「带引号列名 / 占位符 ? / 实际绑定值」三列对齐结构，
                // 准备拼成参数化的 INSERT/UPSERT。
                QStringList cols, placeholders;
                QVariantList vals;
                for (auto it = obj.begin(); it != obj.end(); ++it) {
                    cols << detail::SqlBuilder::quoteIdent(
                        it.key());  // 列名加引号防关键字/特殊字符
                    placeholders << QStringLiteral("?");
                    // C-2/C-3 fix: decode type-tagged values stored by the winner serializer.
                    // "__i64:<n>" → qlonglong, "__b64:<base64>" → QByteArray, else → as-is.
                    // 【C-2/C-3 修复】胜者序列化时给特殊类型打了标签，这里按标签解码还原：
                    //   · "__i64:<n>"     → 64 位整数（JSON double 无法精确表示 >2^53
                    //   的整数，故用此标签）； · "__b64:<base64>" → BLOB（base64 解码回
                    //   QByteArray，Qt/SQLite 会映射回 BLOB 列）； · 其它字符串      → 原样作文本；
                    //   · 非字符串(double/null) → 直接 toVariant()。
                    QVariant val;
                    if (it.value().isString()) {
                        const QString s = it.value().toString();
                        if (s.startsWith(QLatin1String("__i64:"))) {
                            bool ok = false;
                            qint64 iv = s.mid(6).toLongLong(&ok);  // 去掉 6 字符前缀 "__i64:"
                            val = ok ? QVariant(iv) : QVariant(s);  // 解析失败则退回原字符串
                        } else if (s.startsWith(QLatin1String("__b64:"))) {
                            val = QVariant(QByteArray::fromBase64(s.mid(6).toLatin1()));
                        } else {
                            val = QVariant(s);
                        }
                    } else {
                        val = it.value().toVariant();
                    }
                    vals << val;
                }
                // 空对象（无任何列）无法构造 INSERT → 失败回滚。
                if (cols.isEmpty()) {
                    if (err)
                        *err = QStringLiteral(
                                   "E_SYNC_APPLY_CONSTRAINT: winning_content for %1 has no columns")
                                   .arg(tableName);
                    ok = false;
                    break;
                }
                // C-3 fix: use ON CONFLICT DO UPDATE instead of INSERT OR REPLACE to avoid
                // triggering DELETE+INSERT cascade effects that can break FK child rows.
                // Query PK columns so we can build the proper conflict target.
                // 【C-3 修复】恢复用 INSERT ... ON CONFLICT DO UPDATE，而非 INSERT OR REPLACE。
                //   因为 INSERT OR REPLACE 会先 DELETE 再 INSERT，可能触发外键级联删除而误伤子表
                //   行；DO UPDATE 则是原地更新，不引发级联。为此先查出本表的主键列作冲突目标。
                QStringList pkCols;
                {
                    QSqlQuery ti2(wconn);
                    ti2.prepare(QStringLiteral("PRAGMA table_info(") +
                                detail::SqlBuilder::quoteIdent(tableName) + QLatin1Char(')'));
                    if (ti2.exec()) {
                        // table_info 第 6 列(index 5)是 pk：>0 表示该列属于主键。
                        while (ti2.next()) {
                            if (ti2.value(5).toInt() > 0)
                                pkCols << detail::SqlBuilder::quoteIdent(ti2.value(1).toString());
                        }
                    }
                }
                // SET 子句：除主键列外，其余列都用 excluded.<列>（即本次试图插入的新值）覆盖。
                QStringList setClauses;
                QSet<QString> pkSet = QSet<QString>::fromList(pkCols);
                for (const QString& c : cols) {
                    if (!pkSet.contains(c))
                        setClauses << c + QStringLiteral("=excluded.") + c;
                }
                QString restoreSql;
                const QString quotedTable = detail::SqlBuilder::quoteIdent(tableName);
                if (pkCols.isEmpty() || setClauses.isEmpty()) {
                    // No PK info or all-PK table: use INSERT OR IGNORE (safe, no cascade).
                    // 没有主键信息，或全是主键列(没有可 UPDATE 的非主键列)→ 用 INSERT OR IGNORE：
                    //   行已存在则什么都不做，行不存在则插入；同样不触发 DELETE 级联，安全。
                    restoreSql = QStringLiteral("INSERT OR IGNORE INTO %1 (%2) VALUES (%3)")
                                     .arg(quotedTable, cols.join(QLatin1Char(',')),
                                          placeholders.join(QLatin1Char(',')));
                } else {
                    // 正常情形：主键冲突时原地 UPDATE 非主键列，把胜者行内容写回去。
                    restoreSql =
                        QStringLiteral(
                            "INSERT INTO %1 (%2) VALUES (%3) "
                            "ON CONFLICT (%4) DO UPDATE SET %5")
                            .arg(quotedTable, cols.join(QLatin1Char(',')),
                                 placeholders.join(QLatin1Char(',')), pkCols.join(QLatin1Char(',')),
                                 setClauses.join(QLatin1Char(',')));
                }
                // 参数化绑定：按 cols 的顺序逐个 addBindValue（与上面 placeholders 一一对应）。
                QSqlQuery restoreQ(wconn);
                restoreQ.prepare(restoreSql);
                for (const QVariant& v : vals)
                    restoreQ.addBindValue(v);
                if (!restoreQ.exec()) {
                    // 恢复执行失败 → 失败回滚（同样不允许低 rank DELETE 生效）。
                    if (err)
                        *err = QStringLiteral(
                                   "E_SYNC_APPLY_CONSTRAINT: failed to restore high-rank "
                                   "row on %1: %2")
                                   .arg(tableName, restoreQ.lastError().text());
                    ok = false;
                    break;
                }
                // Restore succeeded — the winner entry is unchanged and remains authoritative.
                // 恢复成功——胜者表里的那条记录保持不变，依旧权威（我们只是把行数据补回库里）。
            }
            continue;  // 不用 DELETE 信息更新胜者表（DELETE 不产生新胜者）  do not update winner
                       // with DELETE info
        }

        // ── INSERT / UPDATE：记一笔写入，并把整行内容序列化存进胜者表，供将来 DELETE 恢复用 ──
        // 行内容指纹（与 conflictCb 一致）。
        QByteArray contentH =
            QCryptographicHash::hash(contentMaterial, QCryptographicHash::Sha256).left(16);

        // C-08 fix: use real column names (not indices) so the DELETE recovery path can build
        // a valid INSERT SQL. Re-use the cached colNames from the pkHash computation above.
        // 【C-08 修复】rowJson 的键必须是「真实列名」而非列下标，否则上面 DELETE 恢复路径据此
        //   拼出的 INSERT SQL 列名无效。复用前面算 pkHash 时已缓存的 colNames。
        // 把整行新值序列化成 JSON 对象（即将存为 winningContent）：
        QJsonObject rowJson;
        for (int ci = 0; ci < nCol; ++ci) {
            sqlite3_value* newVal = nullptr;
            sqlite3changeset_new(iter, ci, &newVal);
            if (!newVal)
                continue;  // 该列未提供（UPDATE 只记被改列）→ 不写进 JSON
            // Use real column name from cache; fall back to "col_N" if cache missed.
            const QString colKey =
                (ci < colNames.size()) ? colNames[ci] : QStringLiteral("col_%1").arg(ci);
            switch (sqlite3_value_type(newVal)) {
                case SQLITE_TEXT:
                    rowJson[colKey] = QString::fromUtf8(
                        reinterpret_cast<const char*>(sqlite3_value_text(newVal)));
                    break;
                case SQLITE_INTEGER: {
                    // C-2 fix: JSON doubles cannot represent all 64-bit integers exactly
                    // (integers > 2^53 lose precision). Store as a tagged string to preserve
                    // full int64 precision for row-winner restore.
                    // 【C-2 修复】JSON 数字本质是 double，无法精确表示所有 64 位整数(>2^53
                    // 会丢精度)。
                    //   故整数存成带标签的字符串 "__i64:<n>"，恢复路径再按标签解回 qint64(见上)。
                    qint64 iv = sqlite3_value_int64(newVal);
                    rowJson[colKey] = QStringLiteral("__i64:") + QString::number(iv);
                    break;
                }
                case SQLITE_FLOAT:
                    // 浮点数 JSON 能原生表示，直接存（恢复时 toVariant 还原为 double）。
                    rowJson[colKey] = sqlite3_value_double(newVal);
                    break;
                case SQLITE_BLOB: {
                    // C-3 fix: BLOB values must be preserved for row-winner restore.
                    // Encode as base64 with a type tag so the restore path can rebind as
                    // QByteArray (which Qt/SQLite driver maps back to a BLOB column).
                    // 【C-3 修复】BLOB 必须无损保留以便恢复。base64 + "__b64:" 标签编码，恢复路径
                    //   解码回 QByteArray（Qt/SQLite 驱动会把它映射回 BLOB 列）。
                    const void* blobPtr = sqlite3_value_blob(newVal);
                    int blobLen = sqlite3_value_bytes(newVal);
                    QByteArray ba = (blobPtr && blobLen > 0)
                                        ? QByteArray(static_cast<const char*>(blobPtr), blobLen)
                                        : QByteArray();
                    rowJson[colKey] = QStringLiteral("__b64:") + QString::fromLatin1(ba.toBase64());
                    break;
                }
                default:
                    rowJson[colKey] = QJsonValue::Null;  // NULL → JSON null
            }
        }

        // 组装本行挑战者：来源三元组 + 内容指纹 + 完整行内容(紧凑 JSON)。
        RowWinner challenger;
        challenger.origin = origin;
        challenger.rank = rank;
        challenger.originSeq = seq;
        challenger.contentHash = contentH;
        challenger.winningContent =
            QString::fromUtf8(QJsonDocument(rowJson).toJson(QJsonDocument::Compact));

        // H-01 fix: use putOrRefill() so that if conflictCb() was called for this row
        // (and left an empty winningContent record), the full content written here takes
        // precedence even when rank/seq are identical.
        // M-01 fix: check the return value and propagate failure so the caller can roll back.
        // 【H-01 修复】用 putOrRefill() 而非 put()：若 conflictCb 之前为这行抢先写过一条
        //   winningContent 为空的残缺记录，这里带完整内容的写入要能「填满」它——即便 rank/seq
        //   完全相同也覆盖（普通 put 在打平时不会覆盖，会漏掉内容）。
        // 【M-01 修复】检查返回值并向上传播失败，让调用方据此回滚整段 apply。
        {
            QString putErr;
            if (!winners.putOrRefill(wconn, tableName, pkHashStr, challenger, &putErr)) {
                if (err)
                    *err = QStringLiteral(
                               "E_SYNC_APPLY_CONSTRAINT: putOrRefill failed for %1 pk=%2: %3")
                               .arg(tableName, pkHashStr, putErr);
                ok = false;
                break;
            }
        }
    }
    sqlite3changeset_finalize(iter);  // 释放迭代器（与 sqlite3changeset_start 配对，必须调用）
    return ok;
}

}  // namespace dbridge::sync
