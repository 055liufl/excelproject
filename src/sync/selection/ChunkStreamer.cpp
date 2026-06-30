// ============================================================================
// ChunkStreamer.cpp — 把「冻结闭包清单」按字节预算切片成多个分片的实现
// ============================================================================
// 详见 ChunkStreamer.h 头注释。本文件实现三件事：
//   ① entryToFrozen：把 FkClosureBuilder::Entry（闭包清单一行）转成传输用的 FrozenEntry
//      （算主键哈希 pkHash + 行内容指纹 fingerprint，并标记 selected/dependency）；
//   ② estimateRowBytes：对单行做「廉价的体积估算」，作为装箱（packing）依据，避免对每行
//      都做一次完整 JSON 序列化来量体积（那样太贵）；
//   ③ stream：单趟线性扫描清单，用贪心装箱把行分配到各分片，控制单片不超预算，最后统一
//      回填每片的 chunkSeq / totalChunks。
// 关键不直观处（装箱边界判定、单行超预算的处理、两遍式 chunkSeq/totalChunks 计数）在
// stream() 内逐行解释。
// ============================================================================
#include "ChunkStreamer.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>

namespace dbridge::sync {

// ── entryToFrozen —— 把一行闭包清单项「冻结」为可传输的 FrozenEntry ───────────────
// Convert an Entry's row map to a stable FrozenEntry.
//   （把一个 Entry 的行数据转成稳定的 FrozenEntry——含主键哈希与行内容指纹的冻结快照。）
// 做什么：从 Entry 拷出表名/主键/拓扑序/选中标志，并现算两个哈希：pkHash（主键身份）与
//   fingerprint（行内容指纹，供漂移检测/一致性比对）。
// 为什么要「冻结」：发送窗口内被推送的行可能又被改动；记录发送当下的内容指纹，接收/事后
//   即可比对判断是否发生「漂移」（W_SYNC_PUSH_ROW_DRIFTED，见 Errors.h）。
// 返回：填好的 FrozenEntry（值类型，按值返回）。无副作用、不触库、纯函数。
// 复杂度：O(行字节数)——两次 SHA-1 各扫一遍输入。
static FrozenEntry entryToFrozen(const FkClosureBuilder::Entry& e) {
    // pkHash: SHA-1 of "table\x1fpk"
    //   （主键哈希 = 对 "表\x1f主键" 做 SHA-1 取十六进制文本。）
    // 用 \x1f（ASCII 单元分隔符 US）拼接：它几乎不出现在表名/主键里，可避免
    //   "ab"+"c" 与 "a"+"bc" 撞键的歧义（与 FkClosureBuilder::entryKey 同一思路）。
    const QByteArray hashSrc = e.table.toUtf8() + '\x1f' + e.pk.toUtf8();
    const QString pkHash =
        QString::fromLatin1(QCryptographicHash::hash(hashSrc, QCryptographicHash::Sha1).toHex());

    FrozenEntry fe;
    fe.table = e.table;    // 行所属表
    fe.primaryKey = e.pk;  // 行主键（业务可读形式）
    fe.pkHash = pkHash;    // 定长主键哈希（便于做索引/比对的键）
    // 记录种类：用户直接选中 → "selected"；外键闭包带入的父依赖行 → "dependency"。
    fe.recordKind = e.isSelected ? QStringLiteral("selected") : QStringLiteral("dependency");
    fe.topoIndex = e.topoIndex;  // 沿用 FkClosureBuilder 排好的拓扑序下标（父先子后）
    // Compute row fingerprint (SHA-1 of compact JSON).  Must be non-null: the DDL column
    // is BLOB NOT NULL, and binding a default-constructed QByteArray() produces SQL NULL.
    //   （计算行指纹 = 对「紧凑 JSON 形式的整行」做 SHA-1。
    //    指纹必须非空：对应 DDL 列是 BLOB NOT NULL，而绑定默认构造的 QByteArray() 会被
    //    当作 SQL NULL 写入 → 违反非空约束。因此即便是空行也务必得到一个真实的哈希值。）
    // 注意（与 FkClosureBuilder::rowFingerprint 的算法不同）：此处走 JSON 紧凑序列化再哈希，
    //   是「随推送随行的权威指纹」；那边走 toString() 拼接是仅供剪枝的「粗粒度」指纹。两者
    //   用途不同、互不替代——一处求精确一致性，一处只求命中即省一次推送。
    const QByteArray rowJson =
        QJsonDocument(QJsonObject::fromVariantMap(e.row)).toJson(QJsonDocument::Compact);
    fe.fingerprint = QCryptographicHash::hash(rowJson, QCryptographicHash::Sha1);
    return fe;
}

// ── estimateRowBytes —— 廉价估算单行的序列化体积（装箱依据） ──────────────────────
// Estimate byte size of a single row (rough, avoids full JSON serialization per row).
//   （粗略估算单行的字节体积；故意「估」而非真序列化，避免给每一行都跑一遍完整 JSON 编码。）
// 为什么用估算而非精确量体积：装箱只需「足够接近」的尺度来决定切分边界；对每行都
//   QJsonDocument 序列化一遍既慢又会产生大量临时内存。这里用一个简单线性公式逼近，
//   宁可略微高估（偏保守 → 分片更小、更不会撑爆传输上限），也不在热路径上付昂贵代价。
// 估算公式（逐列累加）：
//   · 每列键名：size()*2 + 4 —— *2 粗估 UTF-16/编码后字节，+4 为 JSON 引号/冒号/逗号等结构开销；
//   · 每列值：  value.toString().size()*2 + 4 —— 同理，值统一按字符串长度估，再加结构开销。
// 返回：估算字节数，且不低于 64 的下限（floor）——空行/极小行也按至少 64 字节计，避免把
//   极多的空行误判为「几乎不占体积」而无限堆进同一分片，给每行一个最小占位成本。
// 复杂度：O(列数 × 字段长)；纯计算，无副作用。
static qint64 estimateRowBytes(const QVariantMap& row) {
    qint64 est = 0;
    for (auto it = row.constBegin(); it != row.constEnd(); ++it) {
        est += it.key().size() * 2 + 4;               // key overhead（键名 + 结构开销）
        est += it.value().toString().size() * 2 + 4;  // value overhead（值 + 结构开销）
    }
    return qMax(est, static_cast<qint64>(64));  // floor（下限 64 字节，给极小行一个最小占位）
}

// ── stream —— 顶层入口：单趟扫描清单 → 贪心装箱切片 → 回填批次/序号 ──────────────
// 整体两阶段（注释里的 First pass / 之后的回填循环）：
//   第一遍：顺序遍历 manifest，把行「贪心装箱」进一串临时 RawChunk（只关心体积与归属）；
//   第二遍：临时 RawChunk 个数即 totalChunks，逐个搬进对外的 Chunk 并回填 pushId/chunkSeq/
//          totalChunks（序号/总数必须在第一遍跑完、总片数确定后才能填，故拆成两遍）。
// 关键不变量：本函数严格保持 manifest 的相对顺序（拓扑序，父先子后）——只「按段截断」，
//   绝不重排。对端把各分片按 chunkSeq 顺序应用，即等价于按原拓扑序逐行 UPSERT，外键依赖
//   天然满足（父行所在的更早分片先被应用）。
bool ChunkStreamer::stream(const QList<FkClosureBuilder::Entry>& manifest, const QString& origin,
                           const QString& targetPeer, qint64 chunkBudgetBytes, PayloadCodec& codec,
                           QList<Chunk>* chunks, QString* err) {
    Q_UNUSED(codec)  // used later if wire-encoding is needed here
                     //   （codec 暂未用：将来若需在此就地做线编码以更精确控体积，再启用。）
    Q_UNUSED(targetPeer)  // 暂未用：保留以备将来按对端能力/版本定制分片策略。
    Q_UNUSED(err)  // 注：err 在 H-04 分支里确有写入；此 Q_UNUSED 是为「正常路径不写 err」
                   //   而留的形式声明，避免某些编译期对「未使用形参」的告警，不影响实际写入。

    if (manifest.isEmpty())
        return true;  // 空清单：无可切分，直接成功（*chunks 保持原样，通常为空）。

    // 现生成本批次唯一 id（无花括号形式的 UUID）。同一次 stream() 产出的所有分片共享它，
    // 接收端据 pushId 把散落的分片归并回「同一批推送」（见 SyncTypes.h 计数语义）。
    const QString pushId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    // First pass: assign entries to chunks by byte budget.
    //   （第一遍：按字节预算把各行分配到分片。）
    // RawChunk 是「装箱中」的临时分片：除 entries/rows 外多带一个 estimatedBytes 累加器，
    // 用来 O(1) 判断「再塞一行会不会超预算」。它不含 chunkSeq/totalChunks——那要等总数定了再填。
    struct RawChunk {
        QList<FrozenEntry> entries;  // 本片已收的冻结清单项
        QList<QVariantMap> rows;     // 本片已收的行数据（与 entries 同序对应）
        qint64 estimatedBytes = 0;   // 本片已收行的估算体积累计（装箱判定用）
    };

    QList<RawChunk> raw;
    raw.append(RawChunk{});  // 先开第一个空分片作为装箱起点（保证 raw.last() 始终有效）

    for (const auto& e : manifest) {
        const qint64 rowEst = estimateRowBytes(e.row);  // 本行的估算体积（装箱单位）

        // H-04 fix: a single row that exceeds the chunk budget on its own cannot be split further.
        // Emit E_SYNC_SELECTION_TOO_LARGE so the caller can surface a clear error rather than
        // silently producing an oversized chunk that may violate transport assumptions.
        //   （H-04 修复：单独一行自身就超过分片预算时，无法再细分——切片的最小粒度是「一行」，
        //    没有「半行」可言。此时与其默默产出一个超大分片、破坏传输层的体积假设，不如显式
        //    报 E_SYNC_SELECTION_TOO_LARGE，让调用方拿到清晰可定位的错误（含表名/估算值/预算）。）
        // 注意 chunkBudgetBytes > 0 这个守卫：预算 <=0 视为「不限体积」，此时不触发该上限检查
        //   （也意味着下方装箱永不另起新片，全部塞进一个分片）。
        if (chunkBudgetBytes > 0 && rowEst > chunkBudgetBytes) {
            if (err)
                *err = QStringLiteral(
                           "E_SYNC_SELECTION_TOO_LARGE: single row in table '%1' "
                           "estimated at %2 bytes exceeds chunk budget %3 bytes")
                           .arg(e.table)
                           .arg(rowEst)
                           .arg(chunkBudgetBytes);
            return false;  // 行级粒度无法满足预算 → 整次切片失败
        }

        // If adding this entry would overflow the current chunk, start a new one.
        //   （若把这一行加进「当前分片」会撑破预算，就先封存当前分片、另起一片。）
        // 边界判定的两个条件缺一不可：
        //   ① !raw.last().entries.isEmpty()：当前分片必须「非空」才允许另起新片。否则一个本就
        //      为空的分片遇到大行会立刻空转换片，永远装不下——而该大行已通过上面的 H-04 单行
        //      上限检查，确知它单独能装进一个空分片，所以这里要让它先落进当前空片。
        //   ② estimatedBytes + rowEst > chunkBudgetBytes：装入后会超预算 → 换片。
        // 也正因有条件①兜底：当 chunkBudgetBytes>0 且某行恰好等于预算时（rowEst==budget），它会
        //   独占一个分片（先落进当前空片，下一行再触发换片），符合「不超预算」语义。
        if (!raw.last().entries.isEmpty() &&
            raw.last().estimatedBytes + rowEst > chunkBudgetBytes) {
            raw.append(RawChunk{});  // 封存旧片、开启新片（后续 raw.last() 即指向新片）
        }

        // 把本行落进「当前分片」：转成 FrozenEntry 入 entries、原始行入 rows、体积计入累加器。
        // entries 与 rows 在此处「同步追加」，从而严格保持下标一一对应（rows[i]↔entries[i]）。
        raw.last().entries.append(entryToFrozen(e));
        raw.last().rows.append(e.row);
        raw.last().estimatedBytes += rowEst;
    }

    const int total = raw.size();  // 装箱完毕，临时分片个数即「总片数」totalChunks
    chunks->reserve(total);        // 预留容量，避免逐次 append 触发多次扩容拷贝

    // 第二遍：把每个 RawChunk 转成对外的 Chunk，并回填三件「批次/计数」元数据。
    // chunkSeq/totalChunks 必须在此处（total 已知后）回填，这正是分成两遍的根本原因：
    //   第一遍切分时还不知道总共会切出几片，无从填 totalChunks。
    for (int i = 0; i < total; ++i) {
        Chunk c;
        c.pushId = pushId;  // 同批共享的批次 id
        c.chunkSeq = i;     // 本片序号：直接取下标 i（0..total-1，连续无空洞）
        c.totalChunks = total;  // 总片数（接收端据 [0,total) 是否凑满判断收齐）
        c.entries = std::move(raw[i].entries);  // 移动转移，避免整片清单的深拷贝
        c.rows = std::move(raw[i].rows);        // 同上，移动 rows
        chunks->append(std::move(c));
    }

    return true;  // 全部分片就绪
}

}  // namespace dbridge::sync
