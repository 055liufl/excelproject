#include "CapturedWriteTemplate.h"

#include "dbridge/Errors.h"  // 机器可读错误码字典（err::E_SYNC_* 等）

#include <QCryptographicHash>  // 行哈希/校验和（TableStateStore::rowHash 等依赖）
#include <QDateTime>           // 取当前毫秒时间戳（记 push_chunk 的 applied_ms）
#include <QSqlError>           // 读取 QSqlQuery 的失败详情文本
#include <QSqlQuery>           // 执行 SQL（预扫描、push 进度记账等）
#include <QSqlRecord>          // 读取 SELECT 结果的列名/列值（预扫描旧行）
#include <QStringList>

#include "sql/SqlBuilder.h"  // quoteIdent：把表名/列名安全加引号转义，防注入、容忍特殊字符
#include <sqlite3.h>  // changeset 迭代 C API（sqlite3changeset_*）

// ============================================================================
// CapturedWriteTemplate.cpp — “被捕获的写”模板的实现
// ----------------------------------------------------------------------------
// 本文件实现头文件声明的三分支写模板（详见 CapturedWriteTemplate.h 文件头）：
//   execute()           —— 统一入口，按 WriteKind 路由。
//   branchA()           —— 入站 changeset：整块 apply + 转存（不重新经 session 捕获）。
//   branchBC()          —— 选择性推送 / 本地写：逐行 UPSERT + session 重新捕获成本地 changeset。
//   extractMutations()  —— 把一段 changeset 解析成逐行 TableMutation（供 table_state 记账）。
//   extractMutationsStatic() —— 上者的免实例化静态版（导入路径复用）。
//
// 阅读提示：分支 A 与分支 B/C 都遵循同一骨架——开启 WriteTxn → 一系列检查与写操作 →
// 任何一步失败立即 rollback 并填错误码返回 → 全部成功才 commit。请始终用“apply 三件套
// 必须同事务原子化”（业务数据 + changelog/applied_vector + table_state）这把尺子去理解
// 每一处 rollback 的必要性。
// ============================================================================

namespace dbridge::sync {

// ---------------------------------------------------------------------------
// constructor —— 构造函数
// ---------------------------------------------------------------------------
// 仅做成员初始化（把注入的协作者引用、原生句柄、本节点身份元信息存起来），
// 不做任何 I/O，也不开启事务。所有引用成员均为“借用”，本类不负责其释放。

CapturedWriteTemplate::CapturedWriteTemplate(QSqlDatabase& wconn, sqlite3* h,
                                             AppliedVectorStore& av, RowWinnerStore& rw,
                                             TableStateStore& ts, ChangelogStore& clog,
                                             SessionRecorder& rec, SchemaGuard& guard,
                                             ChangesetApplier& applier, const QString& nodeId,
                                             qint64 streamEpoch, const QString& schemaFp,
                                             qint64 schemaVer)
    : wconn_(wconn),
      h_(h),
      av_(av),
      rw_(rw),
      ts_(ts),
      clog_(clog),
      rec_(rec),
      guard_(guard),
      applier_(applier),
      nodeId_(nodeId),
      streamEpoch_(streamEpoch),
      schemaFp_(schemaFp),
      schemaVer_(schemaVer) {
}

// ---------------------------------------------------------------------------
// public —— 对外公开方法
// ---------------------------------------------------------------------------

// execute —— 统一写入口：纯路由，按写的种类分派到对应分支。
// 做什么：根据 params.kind 调用 branchA（入站 changeset）或 branchBC（推送/本地写）。
// 返回  ：对应分支的 WriteResult；理论上 switch 已覆盖所有枚举值，末尾的兜底分支只为
//         防御“将来新增了 WriteKind 却忘了在此处理”——此时返回内部码 "UNKNOWN_KIND"。
// 副作用：无（副作用全在被调用的分支内部，由其各自管理事务）。
WriteResult CapturedWriteTemplate::execute(const WriteParams& params) {
    switch (params.kind) {
        case WriteKind::InboundChangeset:  // 收到对端原始 changeset
            return branchA(params);
        case WriteKind::InboundSelectionPush:  // 收到选择性推送的行
        case WriteKind::LocalWrite:            // 本节点自己发起的写
            return branchBC(params);
    }
    // 防御性兜底：正常不可达（上面已枚举所有 case）。仅当枚举被扩展而漏处理时命中。
    WriteResult r;
    r.errorCode = QStringLiteral("UNKNOWN_KIND");
    return r;
}

// ---------------------------------------------------------------------------
// Branch A: InboundChangeset (I-07) —— 应用对端发来的“原始 changeset”
// ---------------------------------------------------------------------------
//
// 【本分支与 B/C 的本质区别】
//   分支 A 收到的是对端 session 已经录好的 changeset 二进制（blob）。我们不需要、也不应
//   重新经 SessionRecorder 去“捕获”这次写——因为这次写的本质就是“把别人录好的差异原样
//   播放一遍”，我们直接把原始 blob 存进 changelog（appendForward 转存）即可继续向下游转发。
//   故本分支不调用 rec_.begin()/sealInto()，而是走 applier_.apply() + clog_.appendForward()。
//
// 【七步流水线（任一步失败即 rollback 整个 WriteTxn）】
//   ① 开事务   ② 连续序列检查（去重/补洞） ③ 表结构校验
//   ④ apply changeset（含冲突仲裁）         ⑤ 推进 applied_vector 水位
//   ⑥ 用 changeset 增量更新 table_state（I-07）
//   ⑦ 原始 blob 转存进 changelog → commit
WriteResult CapturedWriteTemplate::branchA(const WriteParams& p) {
    WriteResult result;

    // ① 开启写事务（WriteTxn 内部走 BEGIN IMMEDIATE，立刻取写锁）。
    //    自此往下，apply 三件套的所有写都在这一个事务内，要么一起 commit、要么一起 rollback。
    WriteTxn txn(wconn_);
    QString err;
    if (!txn.begin(&err)) {
        result.errorCode =
            QStringLiteral("TXN_BEGIN");  // 内部码：开事务失败（多为库被占用/E_BUSY）
        result.errorMsg = err;
        return result;
    }

    // ② 连续序列检查（去重 / 补洞判定，设计 G-05）。
    //    依据本地为该 (origin, epoch) 记录的水位，判断这条 seq 该如何处理：
    SeqCheckResult sc = av_.check(wconn_, p.origin, p.epoch, p.seq, &err);
    if (sc == SeqCheckResult::NoOp) {
        // seq <= 水位：这条早已应用过。回滚（本事务什么都没做）并报“幂等成功”——
        // 重复投递是正常现象，绝不能当错误，否则会触发无谓的重试/告警。
        txn.rollback();
        result.ok = true;  // already applied – idempotent success（已应用过——幂等成功）
        return result;
    }
    if (sc == SeqCheckResult::Gap) {
        txn.rollback();
        // H-03 fix: gap means a predecessor seq is missing. Per design G-05 / plan S-01,
        // the artifact should remain in InboxLedger as 'seen' (pending) and be re-scanned
        // on the next tick; E_SYNC_GAP is only emitted after the gap timeout expires
        // (InboxLedger::stalePending). Returning a Gap-specific code (not a hard Error) lets
        // processArtifact skip markConsumed so the ledger stays 'seen'.
        // H-03 修复：Gap 表示“前面缺了一条更早的 seq”（出现空洞）。按设计 G-05 / 计划 S-01，
        //   此 artifact 应继续以 'seen'（待补齐/pending）状态留在 InboxLedger 里，下个 tick 再扫；
        //   只有当空洞超时（InboxLedger::stalePending）后才真正发 E_SYNC_GAP。这里特意返回一个
        //   “非硬错误”的特殊码 GAP_PENDING——让 processArtifact 据此跳过 markConsumed，
        //   从而保持账本为 'seen' 等待后续补齐，而不是把它当失败丢弃。
        result.errorCode =
            QStringLiteral("GAP_PENDING");  // not a Errors.h error code — handled by
                                            // processArtifact 这不是 Errors.h 里的错误码，而是供
                                            // processArtifact 内部识别的约定码。
        result.errorMsg = QStringLiteral("gap for origin=%1 seq=%2; keeping artifact pending")
                              .arg(p.origin)
                              .arg(p.seq);
        return result;
    }
    // 走到这里 sc == Apply：seq 正好是水位+1，可以应用。

    // ③ 表结构校验：来件携带的 schemaVer/schemaFp 必须与本地一致，否则按对端结构写本地库
    //    可能列错位/缺列 → 数据损坏。不一致即拒绝（E_SYNC_SCHEMA_MISMATCH）。
    if (!guard_.verifyPayload(p.schemaVer, p.schemaFp, &err)) {
        txn.rollback();
        result.errorCode = QLatin1String(err::E_SYNC_SCHEMA_MISMATCH);
        result.errorMsg = err;
        return result;
    }

    // ④ 应用 changeset（不经 SessionRecorder——我们要保存的是原始 blob 本身）。
    //    Apply changeset (no SessionRecorder – we store the raw blob)
    ApplyOptions opts;
    // H-01 fix: honour the authoritative flag set by processChangesetArtifact when the
    // changeset originates from the center node (center→edge down-link).
    // H-01 修复：尊重 processChangesetArtifact 在“中心→边缘下行”时设置的 authoritative 标志——
    //   权威下发恒为 REPLACE，跳过 rank/seq 仲裁，不更新 RowWinnerStore。
    opts.authoritative = p.authoritative;
    // M-01 fix: propagate conflict policy so conflictCb can apply TargetWins/Manual.
    // M-01 修复：透传冲突策略，使 conflictCb 在非权威路径上能执行 TargetWins/Manual。
    opts.conflictPolicy = p.conflictPolicy;
    if (!applier_.apply(h_, wconn_, p.changesetBlob, p.origin, p.originRank, p.seq, rw_, opts,
                        p.syncTables, &result.applyOutcome, &err)) {
        // apply 失败：回滚，并据错误文本粗分两类错误码（便于上层归类处理/重试策略）。
        txn.rollback();
        const QString lowerErr = err.toLower();
        // 启发式判别：错误文本含 "foreign" 或 "fk" → 视为外键约束失败，否则视为其它约束失败。
        if (lowerErr.contains(QLatin1String("foreign")) || lowerErr.contains(QLatin1String("fk")))
            result.errorCode = QLatin1String(err::E_SYNC_APPLY_FK);
        else
            result.errorCode = QLatin1String(err::E_SYNC_APPLY_CONSTRAINT);
        result.errorMsg = err;
        return result;
    }

    // ⑤ 推进 applied_vector 水位：把该 (origin, epoch) 的水位推进到本 seq。
    //    Advance applied vector
    //    必须在 apply 成功之后、与之同事务推进——否则水位与实际已应用数据会不一致。
    if (!av_.advance(wconn_, p.origin, p.epoch, p.seq, &err)) {
        txn.rollback();
        result.errorCode = QStringLiteral("AV_ADVANCE");  // 内部码：水位推进失败
        result.errorMsg = err;
        return result;
    }

    // ⑤a 用 changeset 增量更新 table_state（设计 I-07）。
    //    Update table_state from changeset mutations (I-07)
    //    把刚 apply 的 changeset 解析成逐行 TableMutation（before/after/pk 哈希），喂给
    //    TableStateStore 增量维护各表校验和——这样无需全表重扫即可让本地校验和与数据同步。
    QList<TableMutation> muts = extractMutations(p.changesetBlob, p.syncTables);
    if (!muts.isEmpty()) {
        // M-03 fix: applyMutations() failure must roll back the entire apply transaction
        // so that changeset, applied_vector, and table_state remain in sync (apply三件套
        // must stay atomic). A stale table_state that cannot be corrected would silently
        // diverge checksums across nodes.
        // M-03 修复：applyMutations() 失败必须回滚整个 apply 事务，让 changeset、applied_vector、
        //   table_state 三件套保持同步原子（apply 三件套必须原子）。一个无法被纠正的陈旧
        //   table_state 会让各节点校验和悄悄发散（diverge），破坏一致性判断——故绝不能放过。
        if (!ts_.applyMutations(wconn_, muts, p.epoch, p.schemaFp, p.seq, &err)) {
            txn.rollback();
            result.errorCode =
                QStringLiteral("TABLE_STATE_UPDATE");  // 内部码：table_state 更新失败
            result.errorMsg = err;
            return result;
        }
    }

    // ⑤b 把原始 changeset blob 转存进 changelog（appendForward，前向转发）。
    //    Store raw blob in changelog (appendForward)
    //    appendForward 专用于“转发别人的变更”——原样保存来源元信息(origin/seq/epoch/schema)与
    //    原始字节，使本节点能继续把它广播给下游对端（向上游/全域传播）。localSeq 为本地分配的序号。
    qint64 localSeq = 0;
    if (!clog_.appendForward(wconn_, p.origin, nodeId_, p.seq, p.epoch, p.schemaVer, p.schemaFp,
                             p.changesetBlob, &localSeq, &err)) {
        txn.rollback();
        result.errorCode = QStringLiteral("CLOG_FORWARD");  // 内部码：changelog 转存失败
        result.errorMsg = err;
        return result;
    }

    // ⑦ 提交：三件套（业务数据 + applied_vector/changelog + table_state）一次性原子落地。
    if (!txn.commit(&err)) {
        result.errorCode = QStringLiteral("TXN_COMMIT");
        result.errorMsg = err;
        return result;
    }

    result.ok = true;
    result.localChangelogSeq = localSeq;  // 回带本地分配的 changelog 序号（供广播屏障等使用）
    result.tableMutations = muts;  // 回带本次涉及的逐行表变更
    return result;
}

// ---------------------------------------------------------------------------
// Branch B/C: InboundSelectionPush or LocalWrite (I-08) —— 逐行 UPSERT + session 重捕获
// ---------------------------------------------------------------------------
//
// 【B 与 C 为何共用一个方法】
//   两者的“落库 + 捕获”机制完全相同：都拿到一批行级 RowMutation，先开 session 捕获，
//   再逐行 UPSERT 落库，最后把 session 录下的 changeset 封存进 changelog。差异仅在“同步
//   元信息从哪来”：
//     · 分支 B（InboundSelectionPush，isInbound=true）：origin/epoch/seq/schema 来自来件 p.*；
//     · 分支 C（LocalWrite，isInbound=false）：origin=本节点 nodeId_、epoch=streamEpoch_、
//       schema=本地 schemaFp_/schemaVer_。
//   下文凡涉及元信息处都以 isInbound 三元选择，注释会逐处点明。
//
// 【为什么这里要“重新经 session 捕获”（与分支 A 相反）】
//   分支 B/C 我们手里只有 RowMutation（行级意图），没有现成的 changeset blob。要让这批写
//   既落本地库、又能被打包广播给对端，就必须在写之前 rec_.begin() 开启 session，让 SQLite
//   把这批 UPSERT 实际产生的行变更录成 changeset，再 sealInto 封存。这正是“被捕获的写”。
//
// 【关键时序（务必按此顺序，错序会漏捕获或读到脏数据）】
//   开事务 → (入站) 分片幂等检查 + 表结构校验 → rec_.begin() 开 session
//   → 【H-05】UPSERT 之前预扫描旧行（行是否存在 + beforeHash）→ UpsertExecutor 逐行写
//   → sealInto 封存 changeset 进 changelog → (入站) 记 push 分片进度
//   → 用“预扫描结果”增量更新 table_state → commit
//   其中“预扫描必须在 UPSERT 之前”是本方法最易踩坑处：UPSERT 后旧行已被覆盖，
//   再去读“旧值”读到的会是新值（详见 H-05 注释）。
WriteResult CapturedWriteTemplate::branchBC(const WriteParams& p) {
    WriteResult result;
    QString err;

    // isInbound：true=分支 B（入站选择性推送）；false=分支 C（本地写）。决定元信息来源。
    const bool isInbound = (p.kind == WriteKind::InboundSelectionPush);

    // 开启写事务（同分支 A：以下所有写同事务原子）。
    WriteTxn txn(wconn_);
    if (!txn.begin(&err)) {
        result.errorCode = QStringLiteral("TXN_BEGIN");
        result.errorMsg = err;
        return result;
    }

    // 入站：分片幂等检查（push_chunk_progress）。
    // Inbound: check push_chunk_progress idempotency
    // 选择性推送会被切成多个 chunk，可能因网络重投而重复到达。这里先查“该 (pushId, chunkSeq)
    // 是否已 applied”，已应用过则按校验和判定：相同→幂等跳过；不同→载荷损坏，拒绝。
    if (isInbound && !p.pushId.isEmpty()) {
        QSqlQuery chk(wconn_);
        chk.prepare(
            QStringLiteral("SELECT status, checksum FROM __sync_push_chunk_progress "
                           "WHERE push_id = ? AND chunk_seq = ?"));
        chk.addBindValue(p.pushId);
        chk.addBindValue(p.chunkSeq);
        if (chk.exec() && chk.next()) {
            const QString st = chk.value(0).toString();
            if (st == QLatin1String("applied")) {
                // H-03 fix: verify checksum matches the already-applied chunk.
                // Identical checksum → idempotent no-op (safe to skip).
                // Different checksum → the payload is corrupt or mis-routed; quarantine.
                // H-03 修复：校验“重投的 chunk”与“已应用记录”的校验和是否一致。
                //   校验和相同 → 这是同一份数据的重复投递，幂等空操作，安全跳过。
                //   校验和不同 → 同一个 (pushId, chunkSeq) 位置却来了不同内容：载荷损坏或错投，
                //     必须隔离（返回 E_SYNC_PAYLOAD_CORRUPT），绝不能覆盖已应用的正确数据。
                const QString storedCs = chk.value(1).toString();
                if (!storedCs.isEmpty() && !p.checksum.isEmpty() && storedCs != p.checksum) {
                    txn.rollback();
                    result.errorCode = QLatin1String(err::E_SYNC_PAYLOAD_CORRUPT);
                    result.errorMsg =
                        QStringLiteral(
                            "chunk %1 of push %2 was already applied with checksum %3 "
                            "but re-delivered with different checksum %4")
                            .arg(p.chunkSeq)
                            .arg(p.pushId, storedCs, p.checksum);
                    return result;
                }
                // 校验和相同（或一方为空无法比对）→ 幂等跳过：回滚空事务并报成功。
                txn.rollback();
                result.ok = true;  // same checksum → idempotent skip（同校验和——幂等跳过）
                return result;
            }
        }
    }

    // 入站：表结构校验（同分支 A 第③步，仅入站方向需要——本地写的结构本就是本地的）。
    // Inbound: schema guard
    if (isInbound) {
        if (!guard_.verifyPayload(p.schemaVer, p.schemaFp, &err)) {
            txn.rollback();
            result.errorCode = QLatin1String(err::E_SYNC_SCHEMA_MISMATCH);
            result.errorMsg = err;
            return result;
        }
    }

    // 开启全新的 session 捕获。
    // Begin fresh session capture
    // 【关键时序】必须在任何业务写（下面的 UPSERT）之前调用——否则 attach 之前发生的改动
    //   不会被纳入 changeset，导致漏捕获、漏广播。只捕获 p.syncTables 白名单内的表。
    //   失败常意味着当前 QSQLITE 驱动未启用 session 扩展（见项目 README/插件方案）。
    if (!rec_.begin(h_, p.syncTables, &err)) {
        txn.rollback();
        result.errorCode = QStringLiteral("SESSION_BEGIN");  // 内部码：session 开启失败
        result.errorMsg = err;
        return result;
    }

    // ── H-05：在 UPSERT 之前预扫描“行是否已存在”与“旧行哈希(beforeHash)” ───────────
    // H-05 fix: pre-scan row existence and old-hash BEFORE the UPSERT.
    // After UPSERT the old row is gone; reading "old" content post-write gives the new value.
    // H-05 修复：必须在 UPSERT 之前预扫描“该行是否已存在”及其旧值哈希。
    //   原因：UPSERT 一旦执行，旧行就被覆盖/替换了；写之后再去读“旧内容”读到的其实是新值。
    //   而 table_state 记账需要知道：这次写是 INSERT 还是 UPDATE（取决于行原本是否存在），
    //   以及 UPDATE 的 beforeHash（旧行哈希，用于从校验和里“减去旧行、加上新行”）。故须先扫。
    struct PreScan {
        bool rowExists = false;  // 写之前该主键对应的行是否已存在（决定 isInsert/isUpdate）
        QByteArray beforeHash;  // empty when row didn't exist (INSERT
                                // case)（旧行哈希；不存在时为空，即纯插入）
        QByteArray pkHash;  // 该行主键的规范哈希（与 RowWinnerStore 键空间一致）
        QByteArray afterHash;  // 写之后该行（新值）的哈希（由 RowMutation 的列值算出）
    };
    // 小工具①：为某条 RowMutation 构造 WHERE 子句各片段，形如 ["pk1"=?, "pk2"=?]。
    auto buildWhereParts = [](const RowMutation& m) {
        QStringList parts;
        for (const QString& pk : m.pkColumns)
            // H-1 fix: use quoteIdent to handle column names with embedded double-quotes.
            // H-1 修复：用 quoteIdent 给列名加引号转义，正确处理含内嵌双引号等特殊字符的列名。
            parts << detail::SqlBuilder::quoteIdent(pk) + QStringLiteral("=?");
        return parts;
    };
    // 小工具②：按 pkColumns 的顺序，把各主键列对应的值依次绑定到查询占位符上。
    auto bindPkValues = [](QSqlQuery& q, const RowMutation& m) {
        for (const QString& pk : m.pkColumns) {
            int idx = m.columns.indexOf(pk);  // 在 columns 里定位该主键列，取其在 values 中的值
            if (idx >= 0)
                q.addBindValue(m.values[idx]);
        }
    };

    // 逐条 RowMutation 预扫描，结果与 p.mutations 一一对应（同序、同长），存入 preScan。
    QList<PreScan> preScan;
    preScan.reserve(p.mutations.size());
    for (const RowMutation& m : p.mutations) {
        PreScan ps;
        // —— pkHash：用“类型标记”的规范编码（与 RowWinnerStore::pkHash 同一套）——
        // H-04 fix: use canonical type-tagged encoding (same as RowWinnerStore::pkHash)
        // so TableMutation.pkHash is consistent with the winner-store key space.
        // H-04 修复：主键哈希采用与 RowWinnerStore::pkHash 完全相同的“带类型标记”规范编码，
        //   使本处算出的 TableMutation.pkHash 与“胜者账本”的键空间一致（同一行算出同一键）。
        {
            QVariantMap pkMap;
            for (int i = 0; i < m.columns.size(); ++i) {
                if (m.pkColumns.contains(m.columns[i]))  // 只取主键列进 pkMap
                    pkMap[m.columns[i]] = m.values[i];
            }
            const QString pkHex = RowWinnerStore::pkHash(pkMap);  // 返回十六进制字符串
            ps.pkHash = QByteArray::fromHex(pkHex.toLatin1());    // 转回原始字节存放
        }
        // —— afterHash：新行哈希，必须与 TableStateStore::rowHash 的格式严格一致 ——
        // M-02 fix: afterHash must use the same column-name-sorted QVariantMap format as
        // TableStateStore::rowHash() so that incremental mutations produce checksums
        // consistent with resetFromBaseline().  Build a QVariantMap keyed by column name
        // (QVariantMap is sorted by key automatically) and delegate to rowHash().
        // M-02 修复：afterHash 必须采用与 TableStateStore::rowHash() 相同的“按列名排序的
        //   QVariantMap”格式，这样“增量写”算出的校验和才能与“全量 resetFromBaseline()”一致。
        //   做法：以列名为 key 建 QVariantMap（QMap 自动按 key 排序），再委托 rowHash() 计算。
        {
            QVariantMap afterMap;
            for (int i = 0; i < m.columns.size(); ++i)
                afterMap.insert(m.columns[i], m.values[i]);
            ps.afterHash = TableStateStore::rowHash(afterMap);
        }
        // —— rowExists + beforeHash：在 UPSERT 之前查一次旧行 ——
        // rowExists + beforeHash: query BEFORE UPSERT
        if (!m.pkColumns.isEmpty() && !m.table.isEmpty()) {  // 无主键或无表名则无法定位旧行，跳过
            QStringList wp = buildWhereParts(m);
            QSqlQuery existQ(wconn_);
            // H-2 fix: use quoteIdent for table name.
            // H-2 修复：表名同样用 quoteIdent 加引号转义。SELECT * … LIMIT 1 只为判存在性+取旧值。
            existQ.prepare(
                QStringLiteral("SELECT * FROM %1 WHERE %2 LIMIT 1")
                    .arg(detail::SqlBuilder::quoteIdent(m.table), wp.join(QLatin1String(" AND "))));
            bindPkValues(existQ, m);
            // M-01 fix: exec() failure must abort immediately so __sync_table_state is not
            // stamped with a wrong insert/update classification (misread → wrong checksum).
            // M-01 修复：预扫描 exec() 失败必须立刻中止，绝不能带着“没读准”继续往下走——否则
            //   会把这次写错误地分类成 INSERT/UPDATE（误判 → table_state 校验和错误 → 节点发散）。
            //   中止前要 rec_.abort() 拆掉已开的 session，再 rollback
            //   事务（顺序：先拆会话后回滚）。
            if (!existQ.exec()) {
                rec_.abort();
                txn.rollback();
                result.errorCode = QStringLiteral("E_DB_UPSERT");
                result.errorMsg = QStringLiteral("pre-scan failed for '%1': %2")
                                      .arg(m.table, existQ.lastError().text());
                return result;
            }
            if (existQ.next()) {
                ps.rowExists = true;  // 查到了 → 这次写是 UPDATE（覆盖既有行）
                // M-02 fix: beforeHash must also use column-name-sorted QVariantMap so it
                // matches the format produced by extractMutations (changeset path) and
                // resetFromBaseline (full scan path). Build QVariantMap from QSqlRecord.
                // M-02 修复：beforeHash 也要用“按列名排序的 QVariantMap”，以便与 extractMutations
                //   （changeset 路径）和 resetFromBaseline（全表扫描路径）算出的格式完全一致。
                //   做法：从 QSqlRecord 逐列取“列名→列值”建 map，再交给 rowHash()。
                QVariantMap beforeMap;
                QSqlRecord rec = existQ.record();
                for (int ci = 0; ci < rec.count(); ++ci)
                    beforeMap.insert(rec.fieldName(ci), existQ.value(ci));
                ps.beforeHash = TableStateStore::rowHash(beforeMap);
            }
            // 未 next()（查无此行）→ ps.rowExists 保持 false → 这次写是 INSERT，beforeHash 留空。
        }
        preScan.append(ps);
    }

    // ── 逐行 UPSERT 落库（设计 I-08）───────────────────────────────────────────
    // Execute row mutations via UpsertExecutor (I-08).
    // 此刻 session 已开启：下面这批 UPSERT 实际产生的行变更会被 SQLite session 逐条录进
    //   changeset（稍后由 sealInto 封存）。UpsertExecutor 会把逐行的约束/外键失败收集进
    //   rowErrors（非致命、不中断），只有“致命”错误（如 prepare 失败/库未打开）才返回 false。
    UpsertExecutor upsertEx;
    QList<dbridge::RowError> rowErrors;
    if (!upsertEx.apply(wconn_, p.mutations, &rowErrors, &err)) {
        // 致命写失败：先 rec_.abort() 拆掉已开的 session（否则会话会悬挂在连接上），再回滚事务。
        //   顺序固定为“先拆会话、后回滚事务”——两者都作用于同一连接/句柄。
        rec_.abort();
        txn.rollback();
        result.errorCode = QStringLiteral("E_DB_UPSERT");
        result.errorMsg = err;
        return result;
    }
    // C-09 fix: any row-level errors (FK violation, constraint) must abort the whole chunk.
    // Committing with partial row errors would let the receiver ACK a broken chunk.
    // 【C-09 修复 / 全有或全无】任何一条逐行错误（外键违反 / 约束冲突）都必须让“整个 chunk”
    //   失败回滚。为什么不能只丢掉出错那几行、提交其余？因为一旦提交，接收方就会对这个
    //   chunk 回 ACK，发送端便认为“整块已成功送达”，那几条失败的行将永远补不回来——形成
    //   静默丢更。故这里只要 rowErrors 非空，就连同已成功的行一起回滚。
    if (!rowErrors.isEmpty()) {
        // 同样先拆 session、再回滚整块。错误码按“首条失败”的性质粗分：含 "FK"/"foreign"
        //   归为外键失败 E_SYNC_APPLY_FK，否则归为一般约束失败 E_SYNC_APPLY_CONSTRAINT。
        //   errorMsg 汇总失败行数并附上首条详情，便于诊断。
        rec_.abort();
        txn.rollback();
        result.errorCode = rowErrors.first().code.contains(QLatin1String("FK")) ||
                                   rowErrors.first().message.contains(QLatin1String("foreign"))
                               ? QLatin1String(err::E_SYNC_APPLY_FK)
                               : QLatin1String(err::E_SYNC_APPLY_CONSTRAINT);
        result.errorMsg = QStringLiteral("%1 row(s) failed; first: %2")
                              .arg(rowErrors.size())
                              .arg(rowErrors.first().message);
        return result;
    }

    // ── 封存 changeset 进 changelog ────────────────────────────────────────────
    // Seal changeset into changelog
    // 到这里 UPSERT 已全部成功，session 里录着这批写产生的 changeset。sealInto 会：收集
    //   session 的 changeset → 分离(detach)会话 → 把 changeset 连同来源元信息写进
    //   __sync_changelog，并回带本地分配的 local_seq。这一步与业务写同事务（见 §15.7.1
    //   “短命 session、同事务”），保证“已落库”与“已记 changelog”是同一次原子提交。
    qint64 localSeq = 0;
    qint64 parentSeq = 0;
    // originSeq / origin / epoch 三者按方向选择来源（见方法头“B 与 C 为何共用”）：
    //   · 入站(B)：沿用来件里的 origin/epoch/seq（转发别人的变更，保留其身份）；
    //   · 本地(C)：origin=本节点 nodeId_、epoch=本节点 streamEpoch_、seq 为本地预分配序号。
    // 注：这里 isInbound?p.seq:p.seq 两支相同，是因为无论哪支 originSeq 都取自 p.seq
    //     （本地写的序号在进入本方法前已被上游分配进 p.seq），保留三元写法只为对齐可读性。
    qint64 originSeq = isInbound ? p.seq : p.seq;
    const QString origin = isInbound ? p.origin : nodeId_;
    const qint64 epoch = isInbound ? p.epoch : streamEpoch_;
    // 分支 C 的不变式：本地写必须“先分配好 origin_seq 再来封存”。若 seq<=0 说明上游漏了
    //   分配步骤，这是编程/时序错误——直接拆会话、回滚并报 E_SYNC_INIT，绝不带着非法序号入库
    //   （否则该本地变更的连续序列会从错误的起点计数，破坏对端的 gap 判定）。
    if (!isInbound && originSeq <= 0) {
        rec_.abort();
        txn.rollback();
        result.errorCode = QLatin1String(err::E_SYNC_INIT);
        result.errorMsg = QStringLiteral("local origin_seq must be allocated before branch C seal");
        return result;
    }

    // H-01 fix: pass p.pushId so selection-push changesets (InboundSelectionPush) have their
    // push_id recorded in the changelog, enabling the broadcast barrier to filter by specific push.
    // 【H-01 修复】把 p.pushId 一并传给 sealInto，让“选择性推送(分支 B)”产生的 changelog 记录
    //   带上 push_id。这样下游广播时的“半截不外泄”屏障（broadcastToPeer 里按 push 过滤，
    //   见 §15.5.3）才能识别“这条变更属于哪次 push”，在该 push 未 done 前推迟外发。
    //   本地写(分支 C) p.pushId 为空，不受影响。
    if (!rec_.sealInto(h_, clog_, wconn_, txn, origin, epoch, isInbound ? p.schemaVer : schemaVer_,
                       isInbound ? p.schemaFp : schemaFp_, parentSeq, originSeq, &localSeq, &err,
                       /*pushId=*/p.pushId)) {
        // 注意：sealInto 内部若失败通常已 detach 会话，这里只需回滚事务（不再 rec_.abort()）。
        txn.rollback();
        result.errorCode = QStringLiteral("SEAL");  // 内部码：封存失败
        result.errorMsg = err;
        return result;
    }

    // ── 入站：标记本 chunk 已应用，并在集齐全部 chunk 时把整个 push 置为 done ──────────
    // Mark push chunk applied
    if (isInbound && !p.pushId.isEmpty()) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        // 在 __sync_push_chunk_progress 里把本 (pushId, chunkSeq) 记为 'applied'，并存下其
        //   checksum（供将来重投时比对，见前面的“分片幂等检查”）。用 UPSERT：首见则插入，
        //   重放则更新。与前面的幂等检查配对——这里“落状态”，那里“读状态”。
        QSqlQuery upsert(wconn_);
        upsert.prepare(
            QStringLiteral("INSERT INTO __sync_push_chunk_progress "
                           "(push_id, chunk_seq, status, checksum, applied_ms) "
                           "VALUES (?, ?, 'applied', ?, ?) "
                           "ON CONFLICT(push_id, chunk_seq) DO UPDATE SET "
                           "  status = 'applied', checksum = excluded.checksum, "
                           "  applied_ms = excluded.applied_ms"));
        upsert.addBindValue(p.pushId);
        upsert.addBindValue(p.chunkSeq);
        upsert.addBindValue(p.checksum);
        upsert.addBindValue(nowMs);
        if (!upsert.exec()) {
            // 分片进度记账失败：与业务写同事务，故必须整体回滚（不能只落数据不记进度，否则
            //   幂等/续传状态会与实际数据不符）。归为传输类错误 E_SYNC_TRANSPORT。
            txn.rollback();
            result.errorCode = QLatin1String(err::E_SYNC_TRANSPORT);
            result.errorMsg = upsert.lastError().text();
            return result;
        }

        // H-01 fix: after marking this chunk applied, check whether ALL chunks for this push
        // are now applied. If so, promote push_progress.status to 'done'.
        // This is critical for center nodes which receive every chunk from the originator
        // but never send push-chunk ACKs back to themselves — without this, the push barrier
        // in broadcastToPeer (status != 'done') permanently blocks downstream broadcast.
        // 【H-01 修复 / 为何中心节点必须在此处判 done】选择性推送被切成 total_chunks 个分片。
        //   普通对端靠“收全部分片 ACK”把整个 push 置 done；但中心节点是自己“接收并应用”来自
        //   发起者的每一个分片，它不会给自己发分片 ACK——若没有下面这段“集齐即置 done”的逻辑，
        //   broadcastToPeer 里的“半截不外泄”屏障（要求 status=='done' 才放行下游广播，见 §15.5.3）
        //   会永久卡住，导致这批直选变更永远发不到下游。故这里每应用一个分片就自查一次：
        //   已 applied 的分片数是否达到 total_chunks，达到则把 __sync_push_progress 置 'done'。
        {
            // 子查询统计“该 push 已 applied 的分片数”，与登记的 total_chunks 比较。
            //   只在 push 尚未 done 且未 failed 时才检查（避免重复/无谓更新）。
            QSqlQuery doneQ(wconn_);
            doneQ.prepare(
                QStringLiteral("SELECT pp.total_chunks, "
                               "  (SELECT COUNT(*) FROM __sync_push_chunk_progress "
                               "   WHERE push_id = pp.push_id AND status = 'applied') "
                               "  AS applied_chunks "
                               "FROM __sync_push_progress pp "
                               "WHERE pp.push_id = ? AND pp.status != 'done' "
                               "  AND pp.status != 'failed'"));
            doneQ.addBindValue(p.pushId);
            if (doneQ.exec() && doneQ.next()) {
                const int total = doneQ.value(0).toInt();
                const int applied = doneQ.value(1).toInt();
                // total>0 排除“总数未知/未登记”的情形；applied>=total 即“全片到齐”。
                if (total > 0 && applied >= total) {
                    QSqlQuery markDone(wconn_);
                    markDone.prepare(
                        QStringLiteral("UPDATE __sync_push_progress "
                                       "SET status = 'done', updated_ms = ? "
                                       "WHERE push_id = ?"));
                    markDone.addBindValue(nowMs);
                    markDone.addBindValue(p.pushId);
                    if (!markDone.exec()) {
                        txn.rollback();  // 置 done 失败也同事务回滚
                        result.errorCode = QLatin1String(err::E_SYNC_TRANSPORT);
                        result.errorMsg = markDone.lastError().text();
                        return result;
                    }
                }
            }
        }
    }

    // ── 用“预扫描结果”增量更新 table_state（H-05 的收尾）─────────────────────────
    // Update table_state from RowMutations using pre-scanned (correct) old-row info (H-05).
    // 这里把每条 RowMutation 翻译成一条 TableMutation（before/after/pk 哈希 + insert/delete
    // 标记），
    //   喂给 TableStateStore 增量维护各表校验和。关键是 beforeHash/rowExists 全部取自“UPSERT 之前”
    //   的预扫描 preScan（见上），而非现读——否则读到的是被覆盖后的新值，校验和会算错。
    // M-01 fix: skip table_state update for DoNothing (INSERT OR IGNORE) mutations when the
    // row already existed before the UPSERT. In that case the INSERT is a no-op — SQLite does
    // not modify the row — so the actual session changeset contains no entry for it. Adding an
    // afterHash for a no-op write would pollute the checksum and cause divergence with peers
    // that compute table_state from the real changeset (branchA / extractMutations path).
    // 【M-01 修复 / 跳过“空操作”的 DoNothing】若某条是 DoNothing(INSERT OR IGNORE) 且该行本就存在，
    //   则这次 INSERT 是彻底的空操作：SQLite 不改任何数据，session 里也不会为它录任何变更条目。
    //   若我们仍给它记一笔 afterHash 去更新校验和，本地校验和就会“凭空多算一行”，而其它用真实
    //   changeset 算 table_state 的对端（分支 A / extractMutations 路径）不会多算——两端校验和从此
    //   发散(diverge)。所以这种空操作行必须在记账时跳过。
    QList<TableMutation> tmuts;
    tmuts.reserve(p.mutations.size());
    for (int i = 0; i < p.mutations.size(); ++i) {
        const RowMutation& m = p.mutations[i];
        const PreScan& ps = preScan[i];  // 与 p.mutations[i] 同下标对应的预扫描结果
        if (m.mode == UpsertMode::DoNothing && ps.rowExists) {
            // INSERT OR IGNORE was a no-op: row already existed and was not changed.
            // Do not update table_state for this row.
            // 空操作（行已存在、IGNORE 未改动）→ 不为它更新 table_state（见上 M-01 说明）。
            continue;
        }
        TableMutation tm;
        tm.table = m.table;
        tm.isInsert = !ps.rowExists;  // 预扫描时行不存在 → 本次是 INSERT；存在 → UPDATE
        tm.isDelete = false;  // 分支 B/C 的 RowMutation 只做 UPSERT，从不产生 DELETE
        tm.pkHash = QString::fromLatin1(ps.pkHash.toHex());  // 定位行的规范 pk 哈希（十六进制）
        tm.afterHash = ps.afterHash;                         // 新行哈希（写入后的内容）
        tm.beforeHash = ps.beforeHash;  // correctly from pre-UPSERT scan（正确地取自写前扫描）
        tmuts.append(tm);
    }

    if (!tmuts.isEmpty()) {
        // M-03 fix: applyMutations() failure must roll back the entire write transaction
        // so that the upserted rows, changelog entry, and table_state remain atomic.
        // 【M-03 修复】table_state 更新失败必须回滚整个写事务，让“已 UPSERT 的行 + changelog 记录 +
        //   table_state”三者保持原子。理由同分支 A：一个无法纠正的陈旧 table_state 会让各节点
        //   校验和悄悄发散。epoch/schemaFp 按方向选择（入站用来件的、本地用本节点的）。
        if (!ts_.applyMutations(wconn_, tmuts, isInbound ? p.epoch : streamEpoch_,
                                isInbound ? p.schemaFp : schemaFp_, originSeq, &err)) {
            txn.rollback();
            result.errorCode = QStringLiteral("TABLE_STATE_UPDATE");
            result.errorMsg = err;
            return result;
        }
    }

    // ── 提交：业务数据 + changelog + push 进度 + table_state 一次性原子落地 ──────────
    if (!txn.commit(&err)) {
        result.errorCode = QStringLiteral("TXN_COMMIT");
        result.errorMsg = err;
        return result;
    }

    result.ok = true;
    result.localChangelogSeq = localSeq;  // 回带本次封存所得的本地 changelog 序号（可为 0=无变更）
    result.tableMutations = tmuts;  // 回带本次涉及的逐行表变更
    return result;
}

// ---------------------------------------------------------------------------
// private: extractMutations — parse changeset blob into TableMutation list (I-07)
// 私有：extractMutations —— 把一段 changeset 二进制解析成逐行 TableMutation（供 table_state 记账）
// ---------------------------------------------------------------------------
//
// 【做什么】遍历 changeset 里的每一行变更，按 INSERT/UPDATE/DELETE 算出该行的 pkHash（定位）、
//   beforeHash（旧内容，DELETE/UPDATE 有）、afterHash（新内容，INSERT/UPDATE 有），组装成一条
//   TableMutation。分支 A 用它把“刚 apply 的对端 changeset”翻译成表校验和的增量输入。
// 【为什么与预扫描路径分开】分支 A 手里有现成的 changeset blob（对端录好的），可直接解析拿到
//   before/after 值；分支 B/C 手里只有 RowMutation，才需要 UPSERT 前预扫描（见 branchBC）。
//   两条路径殊途同归，但都必须用“相同的哈希编码”，否则各节点校验和会发散——这正是下面各处
//   反复强调“与 TableStateStore::rowHash / RowWinnerStore::pkHash 同格式”的原因。
// 【参数/返回】changeset=原始字节；syncTables=同步表白名单（不在白名单/__sync_* 元表跳过）。
//   返回逐行 TableMutation；空 changeset 或无法起迭代器 → 返回空列表（不算错误）。
// 【副作用】仅对每张表跑一次 PRAGMA table_info（结果缓存）；不写任何数据。
QList<TableMutation> CapturedWriteTemplate::extractMutations(const QByteArray& changeset,
                                                             const QStringList& syncTables) {
    QList<TableMutation> muts;
    if (changeset.isEmpty())
        return muts;  // 空 changeset：无行可解析

    // 起一个 changeset 迭代器从头遍历；起不来（如损坏/空）→ 返回空列表，不视为失败。
    sqlite3_changeset_iter* iter = nullptr;
    if (sqlite3changeset_start(
            &iter, changeset.size(),
            const_cast<void*>(static_cast<const void*>(changeset.constData()))) != SQLITE_OK)
        return muts;

    // M-03 fix: cache column-name lists per table so that row hashes use the same
    // "key=value\n sorted by column name" format as TableStateStore::rowHash().
    // This ensures beforeHash / afterHash computed here are directly comparable to
    // the checksums produced by resetFromBaseline(), preventing checksum divergence
    // after a baseline reset followed by incremental UPDATE/DELETE.
    // 【M-03 修复】按表名缓存“列下标→列名”映射。changeset 迭代器只给列的下标，不给列名，但
    //   TableStateStore::rowHash() 要求以“按列名排序的 QVariantMap”算哈希。必须拿到真实列名，
    //   本处算出的 before/after 哈希才能与 resetFromBaseline()（全表重扫）算出的校验和逐位可比——
    //   否则“基线重置后再来若干增量 UPDATE/DELETE”会让本地校验和与基线校验和发散。
    QMap<QString, QStringList> colNameCache;

    // getColNames —— 惰性取某表的列名列表（首次查 PRAGMA table_info 并缓存，后续直接命中缓存）。
    //   查不到的列下标退化为占位名 "_col_N"，保证返回列表长度恒为 nCol、不为空。
    auto getColNames = [&](const QString& tableName, int nCol) -> QStringList {
        auto it = colNameCache.find(tableName);
        if (it != colNameCache.end())
            return it.value();
        QStringList names;
        QSqlQuery ti(wconn_);
        // 表名内嵌的双引号要转义成两个双引号，防止 PRAGMA 语法被破坏/注入。
        ti.prepare(QStringLiteral("PRAGMA table_info(\"%1\")")
                       .arg(QString(tableName).replace(QLatin1Char('"'), QLatin1String("\"\""))));
        if (ti.exec()) {
            // table_info 返回 (cid, name, ...)；先建 cid→name 映射，再按 0..nCol-1 取名。
            QMap<int, QString> cidMap;
            while (ti.next())
                cidMap.insert(ti.value(0).toInt(), ti.value(1).toString());
            for (int i = 0; i < nCol; ++i)
                names.append(cidMap.value(i, QStringLiteral("_col_%1").arg(i)));
        } else {
            // Fallback: use positional names so the cache is not empty.
            // PRAGMA 失败（表已不存在等）→ 全用占位名，保证缓存非空、后续逻辑不崩。
            for (int i = 0; i < nCol; ++i)
                names.append(QStringLiteral("_col_%1").arg(i));
        }
        colNameCache.insert(tableName, names);
        return names;
    };

    // 逐行推进迭代器（next 返回 SQLITE_ROW 表示还有下一行）。
    while (sqlite3changeset_next(iter) == SQLITE_ROW) {
        const char* tbl = nullptr;
        int nCol = 0, op = 0, indirect = 0;
        sqlite3changeset_op(iter, &tbl, &nCol, &op, &indirect);  // 取表名/列数/操作类型/是否间接

        unsigned char* pkMask = nullptr;
        sqlite3changeset_pk(iter, &pkMask,
                            nullptr);  // 取主键掩码：pkMask[i] 非 0 表示第 i 列是主键

        const QString tableName = QString::fromUtf8(tbl ? tbl : "");

        // H-01 fix: skip tables rejected by the allow-list so __sync_* meta tables and
        // non-sync tables are never written to __sync_table_state.
        // 【H-01 修复】白名单拒绝的表（含 __sync_* 元表、非同步表）直接跳过——与 filterCb /
        //   updateWinnersFromChangeset 使用同一判定，绝不让它们污染 __sync_table_state 校验和。
        if (!ChangesetApplier::isAllowedSyncTable(tableName, syncTables))
            continue;

        const QStringList colNames = getColNames(tableName, nCol);

        // M-03 fix: build a QVariantMap keyed by column name (sorted automatically by QMap),
        // then delegate to TableStateStore::rowHash() for a consistent hash format.
        // rowHashFromIter —— 计算当前行的“整行内容哈希”。useNew=true 取新值(INSERT/UPDATE 后)，
        //   false 取旧值(UPDATE/DELETE 前)。做法：把每列按列名塞进 QVariantMap（QMap 自动按 key
        //   排序，从而与 TableStateStore::rowHash 的“按列名排序”格式一致），再委托 rowHash()
        //   出结果。 逐类型转换与 conflictCb 一致（FLOAT 走 rowHash 内部规范化），NULL 列记入
        //   QVariant()。
        auto rowHashFromIter = [&](bool useNew) -> QByteArray {
            QVariantMap rowMap;
            for (int i = 0; i < nCol; i++) {
                sqlite3_value* val = nullptr;
                if (useNew)
                    sqlite3changeset_new(iter, i, &val);
                else
                    sqlite3changeset_old(iter, i, &val);
                const QString colName =
                    (i < colNames.size()) ? colNames[i] : QStringLiteral("_col_%1").arg(i);
                if (!val) {
                    rowMap.insert(colName, QVariant());
                    continue;
                }
                const int vt = sqlite3_value_type(val);
                if (vt == SQLITE_TEXT) {
                    const char* txt = reinterpret_cast<const char*>(sqlite3_value_text(val));
                    rowMap.insert(colName, QString::fromUtf8(txt ? txt : ""));
                } else if (vt == SQLITE_INTEGER) {
                    rowMap.insert(colName,
                                  QVariant(static_cast<qlonglong>(sqlite3_value_int64(val))));
                } else if (vt == SQLITE_FLOAT) {
                    rowMap.insert(colName, sqlite3_value_double(val));
                } else if (vt == SQLITE_BLOB) {
                    const void* b = sqlite3_value_blob(val);
                    const int bl = sqlite3_value_bytes(val);
                    rowMap.insert(colName, (b && bl > 0) ? QVariant(QByteArray(
                                                               static_cast<const char*>(b), bl))
                                                         : QVariant(QByteArray()));
                } else {
                    rowMap.insert(colName, QVariant());
                }
            }
            return TableStateStore::rowHash(rowMap);
        };

        // H-04 fix: use canonical type-tagged encoding via RowWinnerStore::pkHash()
        // so pkHash in extractMutations() is consistent with the winner-store key space.
        // pkHashStr —— 计算当前行的“定位哈希 pkHash”。只收集主键列(pkMask[i] 为真)进 pkMap，
        //   委托 RowWinnerStore::pkHash()
        //   出规范化(带类型标签)的哈希，确保与胜者账本同键空间(H-04)。 若该表无主键信息(pkMap
        //   为空)，退化为整行内容哈希作键（与 conflictCb 的退化规则一致）。
        auto pkHashStr = [&](bool useNew) -> QString {
            QVariantMap pkMap;
            for (int i = 0; i < nCol; i++) {
                if (!pkMask || !pkMask[i])
                    continue;  // 非主键列跳过
                const QString cname =
                    (i < colNames.size()) ? colNames[i] : QStringLiteral("_col_%1").arg(i);
                sqlite3_value* val = nullptr;
                if (useNew)
                    sqlite3changeset_new(iter, i, &val);  // INSERT/UPDATE 用新值定位
                else
                    sqlite3changeset_old(iter, i, &val);  // DELETE 用旧值定位（新值不存在）
                if (!val) {
                    pkMap[cname] = QVariant();  // 该列未提供 → 记 NULL 占位
                    continue;
                }
                // 按存储类型转成 QVariant（与 rowHashFromIter 相同的类型分派）。
                const int vt = sqlite3_value_type(val);
                if (vt == SQLITE_TEXT) {
                    const char* txt = reinterpret_cast<const char*>(sqlite3_value_text(val));
                    pkMap[cname] = QString::fromUtf8(txt ? txt : "");
                } else if (vt == SQLITE_INTEGER) {
                    pkMap[cname] = QVariant(static_cast<qlonglong>(sqlite3_value_int64(val)));
                } else if (vt == SQLITE_FLOAT) {
                    pkMap[cname] = sqlite3_value_double(val);
                } else if (vt == SQLITE_BLOB) {
                    const void* b = sqlite3_value_blob(val);
                    const int bl = sqlite3_value_bytes(val);
                    pkMap[cname] = (b && bl > 0)
                                       ? QVariant(QByteArray(static_cast<const char*>(b), bl))
                                       : QVariant(QByteArray());
                } else {
                    pkMap[cname] = QVariant();  // NULL
                }
            }
            return pkMap.isEmpty() ? QString::fromLatin1(rowHashFromIter(useNew).toHex())
                                   : RowWinnerStore::pkHash(pkMap);
        };

        // 按操作类型组装 TableMutation：
        //   · INSERT —— 只有新值：填 pkHash(新) + afterHash(新)，isInsert=true。
        //   · DELETE —— 只有旧值：填 pkHash(旧) + beforeHash(旧)，isDelete=true。
        //   · UPDATE —— 新旧都有：填 pkHash(旧) + beforeHash(旧) + afterHash(新)。
        //     （UPDATE 不允许改主键，故用旧值算 pkHash 与新值等价，取旧值即可。）
        //   · 其它未知 op —— 保守跳过（不产生记账）。
        TableMutation tm;
        tm.table = tableName;

        if (op == SQLITE_INSERT) {
            tm.pkHash = pkHashStr(true);
            tm.afterHash = rowHashFromIter(true);
            tm.isInsert = true;
            tm.isDelete = false;
        } else if (op == SQLITE_DELETE) {
            tm.pkHash = pkHashStr(false);
            tm.beforeHash = rowHashFromIter(false);
            tm.isInsert = false;
            tm.isDelete = true;
        } else if (op == SQLITE_UPDATE) {
            tm.pkHash = pkHashStr(false);  // PK must not change across UPDATE（UPDATE 不改主键）
            tm.beforeHash = rowHashFromIter(false);
            tm.afterHash = rowHashFromIter(true);
            tm.isInsert = false;
            tm.isDelete = false;
        } else {
            continue;  // 未知/间接操作：跳过
        }
        muts.append(tm);
    }
    sqlite3changeset_finalize(iter);  // 释放迭代器（与 sqlite3changeset_start 配对，必须调用）
    return muts;
}

// ---------------------------------------------------------------------------
// public static: extractMutationsStatic —— extractMutations 的“免实例化”静态版
// ---------------------------------------------------------------------------
//
// M-01 fix: static public wrapper so submitImportSync can call extractMutations without
// instantiating a full CapturedWriteTemplate.
// 【M-01 修复 / 为何要一个静态版】导入路径的 submitImportSync 想直接从“导入产生的 changeset”
//   里抽取增量 TableMutation 去更新 table_state，但它并不持有 CapturedWriteTemplate 所需的
//   一整套 store 引用（av_/rw_/ts_/...）。为省去“为解析一段 changeset 而构造整个模板”的沉重
//   依赖，这里提供一个静态方法：只额外接收一个 db 连接（用于 PRAGMA table_info 取列名），
//   逻辑与实例版 extractMutations() 完全一致——差别仅在“列名查询用传入的 db 而非成员 wconn_”。
// 【为何不直接转调实例版】实例版依赖成员 wconn_，静态上下文没有 this。与其伪造一个残缺实例，
//   不如把核心循环原样重实现一遍（下方两处 lambda 与 op 分派均与实例版逐字对应）。两份代码
//   必须保持同步演进——任一处修改哈希/编码规则，另一处也要同改，否则导入路径与同步路径算出的
//   table_state 会发散。
// 【参数/返回】同实例版，只是 db 显式传入。返回逐行 TableMutation；空/损坏 changeset → 空列表。
QList<TableMutation> CapturedWriteTemplate::extractMutationsStatic(const QByteArray& changeset,
                                                                   QSqlDatabase& db,
                                                                   const QStringList& syncTables) {
    // Delegate to the instance method by constructing a thin temporary adapter.
    // We only need wconn_ from the instance for PRAGMA table_info lookups in extractMutations.
    // Rather than instantiating the full template (which requires all store references), create
    // a minimal local context using a placement-new-free helper class.
    QList<TableMutation> muts;
    if (changeset.isEmpty())
        return muts;

    // Re-implement the core of extractMutations here to avoid a full-template dependency.
    // This shares the same logic as extractMutations() with db passed explicitly.
    // 下面这段与实例版 extractMutations() 是“同一套逻辑”的镜像，仅把 wconn_ 换成参数 db。
    //   详细的“为什么这么算哈希/为什么要真实列名”等解释见实例版注释，此处不再重复。
    sqlite3_changeset_iter* iter = nullptr;
    if (sqlite3changeset_start(
            &iter, changeset.size(),
            const_cast<void*>(static_cast<const void*>(changeset.constData()))) != SQLITE_OK)
        return muts;  // 起不了迭代器（空/损坏）→ 空列表，不算错误

    // 列名缓存 + 惰性查询（同实例版；连接用参数 db）。
    QMap<QString, QStringList> colNameCache;
    auto getColNames = [&](const QString& tableName, int nCol) -> QStringList {
        auto it = colNameCache.find(tableName);
        if (it != colNameCache.end())
            return it.value();
        QStringList names;
        QSqlQuery ti(db);  // 唯一区别：用参数 db 而非成员 wconn_
        ti.prepare(QStringLiteral("PRAGMA table_info(\"%1\")")
                       .arg(QString(tableName).replace(QLatin1Char('"'), QLatin1String("\"\""))));
        if (ti.exec()) {
            QMap<int, QString> cidMap;
            while (ti.next())
                cidMap.insert(ti.value(0).toInt(), ti.value(1).toString());
            for (int i = 0; i < nCol; ++i)
                names.append(cidMap.value(i, QStringLiteral("_col_%1").arg(i)));
        } else {
            // 退化占位名，保证缓存非空（同实例版）。
            for (int i = 0; i < nCol; ++i)
                names.append(QStringLiteral("_col_%1").arg(i));
        }
        colNameCache.insert(tableName, names);
        return names;
    };

    // 逐行遍历（同实例版）：取表名/列数/操作/主键掩码，过白名单，再算 pk/before/after 哈希。
    while (sqlite3changeset_next(iter) == SQLITE_ROW) {
        const char* tbl = nullptr;
        int nCol = 0, op = 0, indirect = 0;
        sqlite3changeset_op(iter, &tbl, &nCol, &op, &indirect);
        unsigned char* pkMask = nullptr;
        sqlite3changeset_pk(iter, &pkMask, nullptr);
        const QString tableName = QString::fromUtf8(tbl ? tbl : "");
        // 白名单门控：__sync_* 元表与非同步表一律跳过（与 filterCb 同判定），防污染校验和。
        if (!ChangesetApplier::isAllowedSyncTable(tableName, syncTables))
            continue;
        const QStringList colNames = getColNames(tableName, nCol);

        // 整行内容哈希：以列名为 key 建 QVariantMap（自动排序）→ 委托 rowHash()，
        //   保证与全量 resetFromBaseline() 同格式（同实例版说明）。
        auto rowHashFromIter = [&](bool useNew) -> QByteArray {
            QVariantMap rowMap;
            for (int i = 0; i < nCol; i++) {
                sqlite3_value* val = nullptr;
                if (useNew)
                    sqlite3changeset_new(iter, i, &val);
                else
                    sqlite3changeset_old(iter, i, &val);
                const QString colName =
                    (i < colNames.size()) ? colNames[i] : QStringLiteral("_col_%1").arg(i);
                if (!val) {
                    rowMap.insert(colName, QVariant());
                    continue;
                }
                const int vt = sqlite3_value_type(val);
                if (vt == SQLITE_TEXT) {
                    const char* txt = reinterpret_cast<const char*>(sqlite3_value_text(val));
                    rowMap.insert(colName, QVariant(QString::fromUtf8(txt ? txt : "")));
                } else if (vt == SQLITE_INTEGER) {
                    rowMap.insert(colName,
                                  QVariant(static_cast<qlonglong>(sqlite3_value_int64(val))));
                } else if (vt == SQLITE_FLOAT) {
                    rowMap.insert(colName, QVariant(sqlite3_value_double(val)));
                } else if (vt == SQLITE_BLOB) {
                    const void* b = sqlite3_value_blob(val);
                    const int bl = sqlite3_value_bytes(val);
                    rowMap.insert(colName, (b && bl > 0) ? QVariant(QByteArray(
                                                               static_cast<const char*>(b), bl))
                                                         : QVariant(QByteArray()));
                } else {
                    rowMap.insert(colName, QVariant());
                }
            }
            return TableStateStore::rowHash(rowMap);
        };

        // H-04 fix: same canonical encoding as extractMutations() above.
        // 定位哈希 pkHash：只收主键列 → RowWinnerStore::pkHash() 规范化，与胜者账本同键空间；
        //   无主键则退化为整行哈希作键（编码与上方实例版逐字一致）。
        auto pkHashStr = [&](bool useNew) -> QString {
            QVariantMap pkMap;
            for (int i = 0; i < nCol; i++) {
                if (!pkMask || !pkMask[i])
                    continue;  // 非主键列跳过
                const QString cname =
                    (i < colNames.size()) ? colNames[i] : QStringLiteral("_col_%1").arg(i);
                sqlite3_value* val = nullptr;
                if (useNew)
                    sqlite3changeset_new(iter, i, &val);
                else
                    sqlite3changeset_old(iter, i, &val);
                if (!val) {
                    pkMap[cname] = QVariant();
                    continue;
                }
                const int vt = sqlite3_value_type(val);
                if (vt == SQLITE_TEXT) {
                    const char* txt = reinterpret_cast<const char*>(sqlite3_value_text(val));
                    pkMap[cname] = QString::fromUtf8(txt ? txt : "");
                } else if (vt == SQLITE_INTEGER) {
                    pkMap[cname] = QVariant(static_cast<qlonglong>(sqlite3_value_int64(val)));
                } else if (vt == SQLITE_FLOAT) {
                    pkMap[cname] = sqlite3_value_double(val);
                } else if (vt == SQLITE_BLOB) {
                    const void* b = sqlite3_value_blob(val);
                    const int bl = sqlite3_value_bytes(val);
                    pkMap[cname] = (b && bl > 0)
                                       ? QVariant(QByteArray(static_cast<const char*>(b), bl))
                                       : QVariant(QByteArray());
                } else {
                    pkMap[cname] = QVariant();
                }
            }
            return pkMap.isEmpty() ? QString::fromLatin1(rowHashFromIter(useNew).toHex())
                                   : RowWinnerStore::pkHash(pkMap);
        };

        TableMutation tm;
        tm.table = tableName;
        if (op == SQLITE_INSERT) {
            tm.pkHash = pkHashStr(true);
            tm.afterHash = rowHashFromIter(true);
            tm.isInsert = true;
        } else if (op == SQLITE_DELETE) {
            tm.pkHash = pkHashStr(false);
            tm.beforeHash = rowHashFromIter(false);
            tm.isDelete = true;
        } else if (op == SQLITE_UPDATE) {
            tm.pkHash = pkHashStr(false);
            tm.beforeHash = rowHashFromIter(false);
            tm.afterHash = rowHashFromIter(true);
        } else {
            continue;
        }
        muts.append(tm);
    }
    sqlite3changeset_finalize(iter);
    return muts;
}

// L-02 fix: execMutation() was dead code with unquoted identifiers. Removed.

}  // namespace dbridge::sync
