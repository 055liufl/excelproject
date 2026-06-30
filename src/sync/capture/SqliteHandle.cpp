// ============================================================================
// SqliteHandle.cpp — QSqlDatabase ↔ sqlite3* 桥接与 session 能力探测的实现
// 详见同名头文件 SqliteHandle.h 的总览注释。
// ============================================================================
#include "SqliteHandle.h"

#include <QSqlDriver>
#include <QStringList>
#include <QVariant>

namespace dbridge::sync {

// ── of：从 QSqlDatabase 掏出原生 sqlite3* ────────────────────────────────────
sqlite3* SqliteHandle::of(QSqlDatabase& db) {
    // QSqlDriver::handle() 返回一个不透明的 QVariant；对于 QSQLITE 驱动，其内部装的
    // 实际类型是 `sqlite3**`（指向句柄指针的指针）。
    QVariant v = db.driver()->handle();
    if (!v.isValid())
        return nullptr;  // 驱动未提供句柄（连接未打开 / 非 SQLite 驱动）
    // v.data() 取得 QVariant 内部存储的地址；该地址处存的是一个 sqlite3*，
    // 故强转为 sqlite3** 再解引用一次，得到真正的 sqlite3*。
    return *static_cast<sqlite3**>(v.data());
}

// ── sessionAvailable：编译期能力探测 ─────────────────────────────────────────
bool SqliteHandle::sessionAvailable(sqlite3* h) {
    if (!h)
        return false;
    // sqlite3_compileoption_used 不受任何构建开关影响，永远可调用——所以可用它来
    // 反查“另外两个可选特性的编译宏”是否被打开。两者缺一，session 变更捕获即不可用。
    return sqlite3_compileoption_used("ENABLE_SESSION") &&
           sqlite3_compileoption_used("ENABLE_PREUPDATE_HOOK");
}

// ── libVersion：取 SQLite 版本串（仅日志用）──────────────────────────────────
const char* SqliteHandle::libVersion() {
    return sqlite3_libversion();
}

// ── exerciseSession：运行期实地演练（H-03 fix）───────────────────────────────
// 真正建立一个 session、附着所有同步表、导出一份（空）changeset，借此证明链接进来的
// SQLite 符号确实可用，而非仅仅“编译宏开了”。逐步检查每一个返回码。
bool SqliteHandle::exerciseSession(sqlite3* h, const QStringList& tables, QString* err) {
    if (!h) {
        if (err)
            *err = QStringLiteral("null sqlite3 handle");
        return false;
    }
    // 步骤 1：在 "main" 数据库上创建一个会话对象。失败说明 session 符号根本不可用。
    sqlite3_session* s = nullptr;
    int rc = sqlite3session_create(h, "main", &s);
    if (rc != SQLITE_OK) {
        if (err)
            *err = QStringLiteral("sqlite3session_create failed: rc=%1").arg(rc);
        return false;
    }
    // 步骤 2：把每张同步表附着到会话上，使其变更被纳入捕获范围。
    for (const QString& table : tables) {
        const QByteArray name = table.toUtf8();  // SQLite C API 用 UTF-8 C 字符串
        rc = sqlite3session_attach(s, name.constData());
        if (rc != SQLITE_OK) {
            sqlite3session_delete(s);  // 任一步失败都要删除会话，避免泄漏
            if (err)
                *err = QStringLiteral("sqlite3session_attach failed for '%1': rc=%2")
                           .arg(table)
                           .arg(rc);
            return false;
        }
    }
    // 步骤 3：导出 changeset。此处没有任何业务写入，预期得到空集（n==0），
    // 但关键是这一步必须返回 SQLITE_OK——它真正触达了 changeset 生成路径。
    int n = 0;
    void* p = nullptr;
    rc = sqlite3session_changeset(s, &n, &p);
    sqlite3_free(p);  // 释放 SQLite 分配的 changeset 缓冲（即便为空也需释放）
    sqlite3session_delete(s);  // 演练完毕，删除会话
    if (rc != SQLITE_OK) {
        if (err)
            *err = QStringLiteral("sqlite3session_changeset self-check failed: rc=%1").arg(rc);
        return false;
    }
    return true;  // 全程 OK → session/preupdate-hook 确实可用
}

}  // namespace dbridge::sync
