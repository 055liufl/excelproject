#include "SchemaGuard.h"

#include <QCryptographicHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include "sql/SqlBuilder.h"  // detail::SqlBuilder::quoteIdent —— 安全地给表名加引号防注入

namespace dbridge::sync {

// ── setLocal —— 登记本地基线 ─────────────────────────────────────────────────
// 做什么：把传入的本地结构版本号与指纹存入实例字段，作为 verifyPayload 的参照系。
// 副作用：覆盖 localVer_/localFp_。无返回值、无校验（信任调用方传入的是本端真实值）。
void SchemaGuard::setLocal(qint64 localVer, const QString& localFp) {
    localVer_ = localVer;
    localFp_ = localFp;
}

// ── verifyPayload —— 应用变更前的一致性闸门 ─────────────────────────────────
// 做什么：把变更包携带的 (payloadVer, payloadFp) 与本地基线逐层比对。
// 为什么：结构不一致时若强行应用变更，会写错列/破坏约束，故此处必须拦下（上层据此报
//         E_SYNC_SCHEMA_MISMATCH）。
// 返回：一致 → true；任一层不一致 → false 并写 *err（人类可读原因，便于排障）。
// 复杂度：O(1)（两次整型/字符串比较）。无副作用、不触库。
bool SchemaGuard::verifyPayload(qint64 payloadVer, const QString& payloadFp, QString* err) {
    // 第 1 层：版本号（代际）必须完全相等。最廉价、最先判，不一致直接拒。
    if (payloadVer != localVer_) {
        if (err)
            *err = QStringLiteral("schema version mismatch: payload=%1 local=%2")
                       .arg(payloadVer)
                       .arg(localVer_);
        return false;
    }
    // 第 2 层：版本号相同的前提下，再比内容指纹（抓「版本号忘升但结构其实变了」）。
    // M-02 fix: skip fingerprint comparison when verifySchemaFingerprint is disabled.
    // M-02 修复（译）：当（构造时）禁用了指纹校验时跳过这一层——此时仅靠版本号把关。
    if (verifyFingerprint_ && payloadFp != localFp_) {
        if (err)
            *err = QStringLiteral("schema fingerprint mismatch: payload=%1 local=%2")
                       .arg(payloadFp)
                       .arg(localFp_);
        return false;
    }
    return true;  // 两层都通过 → 结构一致，允许应用
}

// ── computeFingerprint —— 把一批表的结构压成稳定指纹（本文件核心）───────────
// 做什么：自省给定 tables 的「列定义 + 唯一索引 + 外键关系」，串成一段确定性字节流
//         （material），再取 SHA-256 转十六进制字符串返回。
// 为什么要「确定性」：两端只有在相同结构 → 相同字节流 → 相同哈希，指纹才能用作一致性
//         判据。任何顺序不稳定、漏字段、编码歧义都会让指纹失真。
// 关键设计点（逐一对应下方实现）：
//   1) 先对表名排序（sorted.sort()）—— 消除「调用方给表的先后顺序」带来的差异；
//   2) 每张表内，PRAGMA 各项按 SQLite 固定返回序遍历，并用带前缀的 KEY:VALUE 文本编码，
//      使字节流自描述、无歧义（COL:/UIDX:/FK: 前缀 + 字段名兜底分隔）；
//   3) 纳入 PK / NOT NULL / 默认值 / 唯一索引 / 外键——这些都是「结构是否兼容」的要素，
//      任何一项变了，两端就不该再互相应用变更（M-2 fix 即为补全这些维度）。
// 参数：db 已打开的连接；tables 参与同步的表名列表（可乱序，内部会排序）。
// 返回：64 字符的 SHA-256 十六进制小写串。
// 错误模式：本函数不抛错也不回报 PRAGMA 失败——若某表不存在或查询失败，对应 while 循环
//         拿不到行，该表只贡献 "TABLE:<name>\n" 这一行（这是刻意的轻量实现：上层
//         SchemaEligibility 已先行校验过表的合法性）。
// 线程：操作传入的 db 连接，需在该连接所属线程调用。
// 复杂度：O(表数 × 每表(列+索引+外键)条数)，外加一次 SHA-256；体量很小，可忽略。
QString SchemaGuard::computeFingerprint(QSqlDatabase& db, const QStringList& tables) {
    // 步骤 1：排序表名，消除调用方传参顺序的影响（指纹必须只依赖结构本身）。
    QStringList sorted = tables;
    sorted.sort();

    // material 累积「结构的规范文本编码」，最终对它整体做哈希。
    QByteArray material;
    for (const QString& tbl : qAsConst(sorted)) {
        // 每张表以一行 "TABLE:<表名>" 起头，作为分节锚点（防止跨表字段串扰）。
        material.append("TABLE:");
        material.append(tbl.toUtf8());
        material.append('\n');

        // —— 列信息 ——
        // Column info — M-2 fix: include notnull and default_value so schema changes that
        // add NOT NULL constraints or change defaults are detected as fingerprint differences.
        // M-2 修复（译）：纳入 notnull 与默认值——这样「新增 NOT NULL 约束」或「改默认值」
        //   这类结构变化也会反映成指纹差异，不会被漏判。
        // PRAGMA table_info 返回每列：cid, name, type, notnull, dflt_value, pk（下标 0..5）。
        // quoteIdent(tbl)：给表名安全加双引号，处理含空格/保留字/引号的表名（防注入）。
        QSqlQuery colQ(db);
        colQ.exec(QStringLiteral("PRAGMA table_info(") + detail::SqlBuilder::quoteIdent(tbl) +
                  QLatin1Char(')'));
        while (colQ.next()) {
            // cid, name, type, notnull, dflt_value, pk
            QString colName = colQ.value(1).toString();  // 列名
            QString colType = colQ.value(2).toString();  // 声明类型（如 INTEGER/TEXT）
            int notnull = colQ.value(3).toInt();         // 是否 NOT NULL（1/0）
            QString dflt = colQ.value(4).toString();  // 默认值表达式文本（可能为空）
            int pk = colQ.value(5).toInt();  // 主键序号（>0 表示是 PK 的第几列）
            // 编码成： COL:<name>:<type>:PK=<pk>:NN=<notnull>:DFT=<default>\n
            // 用固定 KEY=VALUE 形式串接，保证「同结构 → 同字节」。
            material.append("COL:");
            material.append(colName.toUtf8());
            material.append(':');
            material.append(colType.toUtf8());
            material.append(":PK=");
            material.append(QByteArray::number(pk));
            material.append(":NN=");
            material.append(QByteArray::number(notnull));
            material.append(":DFT=");
            material.append(dflt.toUtf8());
            material.append('\n');
        }

        // —— 唯一索引 ——
        // Unique indexes — changes to uniqueness constraints affect fingerprint.
        // 唯一约束的增减直接影响 UPSERT/冲突语义，必须计入指纹。
        // PRAGMA index_list 返回每个索引：seq, name, unique, origin, partial（下标 0..4）。
        QSqlQuery idxQ(db);
        idxQ.exec(QStringLiteral("PRAGMA index_list(") + detail::SqlBuilder::quoteIdent(tbl) +
                  QLatin1Char(')'));
        while (idxQ.next()) {
            if (idxQ.value(2).toInt() != 1)  // unique flag —— 只收唯一索引，跳过非唯一索引
                continue;
            QString idxName = idxQ.value(1).toString();
            material.append("UIDX:");
            material.append(idxName.toUtf8());
            material.append('\n');
        }

        // —— 外键关系 ——
        // FK relationships — changes in FK targets affect fingerprint.
        // 外键拓扑决定「应用顺序/级联」，目标表或列一变两端就不兼容，必须计入指纹。
        // PRAGMA foreign_key_list 返回每条外键：id, seq, table(被引用表), from(本表列),
        //   to(被引用列), on_update, on_delete, match（下标 0..7，这里取 2/3/4）。
        QSqlQuery fkQ(db);
        fkQ.exec(QStringLiteral("PRAGMA foreign_key_list(") + detail::SqlBuilder::quoteIdent(tbl) +
                 QLatin1Char(')'));
        while (fkQ.next()) {
            // 编码成： FK:<refTable>:<fromCol>->:<toCol>\n
            material.append("FK:");
            material.append(fkQ.value(2).toString().toUtf8());  // refTable —— 被引用的目标表
            material.append(':');
            material.append(fkQ.value(3).toString().toUtf8());  // from col —— 本表参与外键的列
            material.append("->:");
            material.append(fkQ.value(4).toString().toUtf8());  // to col   —— 目标表被引用的列
            material.append('\n');
        }
    }
    // 步骤末：对整段 material 取 SHA-256，转十六进制小写字符串返回。
    // 用 fromLatin1 是因为 toHex() 产出的是纯 ASCII 十六进制字节，Latin1 与之等价且零开销。
    return QString::fromLatin1(
        QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
}

}  // namespace dbridge::sync
