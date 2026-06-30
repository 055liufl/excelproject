#pragma once
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

// ============================================================================
// SchemaGuard.h — 同步用「表结构守卫」：版本号 + 指纹双重一致性闸门
// ============================================================================
//
// 【这个文件是什么】
//   同步两端（中心节点与边缘节点、或任意两个 peer）要安全地互相应用变更，前提是
//   双方对「参与同步的那批表长什么样」有一致认知。一旦一端悄悄改了表结构（加列、
//   改类型、动主键/外键/唯一约束……），另一端再把旧结构假设下生成的变更包应用上去，
//   轻则写错列，重则破坏数据完整性。SchemaGuard 就是挡在「应用变更」之前的那道闸门：
//     · 本端先用 computeFingerprint() 把自己这批表的结构压成一个稳定的 SHA-256 指纹；
//     · 通过 setLocal() 把（本地 schemaVersion, 本地指纹）登记为「本地基线」；
//     · 每收到一个变更包，先用 verifyPayload() 拿包里携带的（schemaVer, schemaFp）
//       和本地基线比对，不一致就拒绝应用并报 E_SYNC_SCHEMA_MISMATCH（见 Errors.h）。
//
// 【为什么要「版本 + 指纹」两个维度，而不是只看一个】
//   · schemaVersion 是一个单调递增的整数，由 DDL 迁移流程负责推进，语义是「第几代结构」。
//     它廉价、易比较，能快速判定「对端比我新/旧」（用于隔离区 QuarantineStore 的
//     “等结构追上来再重放”逻辑）。
//   · 指纹（fingerprint）是对真实结构内容做哈希得到的，能抓住「版本号忘了升，但结构
//     其实变了」这类人为疏漏——是更强的内容级校验。
//   二者配合：版本号给出快速的「代际」判断，指纹给出可靠的「内容真的一致吗」判断。
//
// 【与 ETL 那套 schema 的区别（务必区分）】
//   本类属于「同步用」schema：只关心两端结构是否一致、能否安全互相应用变更。
//   它和 ETL 的 src/schema（Excel 导入导出的列映射/类型推断）是两回事，不要混淆。
//
// 【线程模型】
//   实例持有可变状态（localVer_/localFp_），setLocal 与 verifyPayload 不是线程安全的，
//   约定由同一个同步工作线程串行调用。computeFingerprint 是 static 纯函数（除了它操作
//   的 QSqlDatabase 连接），无实例状态，但 QSqlDatabase 本身不可跨线程共享。
// ============================================================================

namespace dbridge::sync {

// ── SchemaGuard —— 最小化的「结构版本 + 指纹」守卫 ───────────────────────────
// 用法：调用方先 setLocal() 设定本地基线，之后 verifyPayload() 会拒绝任何
//       (schemaVer, schemaFp) 偏离该基线的变更包。
class SchemaGuard {
   public:
    // 构造：verifyFingerprint 决定是否启用「指纹比对」这一层。
    // M-02 fix: accept verifyFingerprint flag at construction; when false, fingerprint
    // comparison is skipped in verifyPayload() (version mismatch still rejects).
    // M-02 修复（译）：构造时接收 verifyFingerprint 开关；为 false 时 verifyPayload()
    //   跳过指纹比对（但「版本号不一致」这一层仍然照常拒绝）。
    //   关闭指纹比对的典型场景：调试 / 双方明确容忍指纹漂移、只靠版本号把关。
    explicit SchemaGuard(bool verifyFingerprint = true) : verifyFingerprint_(verifyFingerprint) {
    }

    // 登记本地基线：把「本地结构版本号 + 本地结构指纹」记下，作为后续校验的参照系。
    // 副作用：覆盖内部 localVer_/localFp_。通常在同步初始化、或本端结构迁移后调用一次。
    void setLocal(qint64 localVer, const QString& localFp);

    // 校验一个变更包是否与本地基线匹配。
    // 返回：匹配 → true；不匹配 → false，且（若 err 非空）写入人类可读的失败原因。
    // 错误模式：版本号不符立即返回 false；版本号相同但指纹不符（且启用了指纹比对）也返回 false。
    bool verifyPayload(qint64 payloadVer, const QString& payloadFp, QString* err);

    // 为给定的一批表计算结构指纹：把（排序后的）列名、类型、非空、默认值、唯一索引、
    // 外键关系等串成一段确定性的字节流，再取 SHA-256，返回十六进制字符串。
    // 这是本类最不直观、也最关键的一段——具体串接格式见 .cpp 逐行注释。
    // 之所以是 static：它只依赖传入的 db 与表名，不需要实例的本地基线状态。
    static QString computeFingerprint(QSqlDatabase& db, const QStringList& tables);

    // 取当前本地指纹（即最近一次 setLocal 设入的值）。
    QString fingerprint() const {
        return localFp_;
    }

   private:
    qint64 localVer_ = 0;            // 本地结构版本号（代际），setLocal 设入
    QString localFp_;                // 本地结构指纹（SHA-256 hex），setLocal 设入
    bool verifyFingerprint_ = true;  // 是否启用指纹比对层（构造时决定）
};

}  // namespace dbridge::sync
