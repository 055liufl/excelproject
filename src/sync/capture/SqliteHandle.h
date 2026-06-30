#pragma once
#include <QSqlDatabase>

#include <sqlite3.h>

// ============================================================================
// SqliteHandle.h — 从 Qt 的 QSqlDatabase 中“掏出”底层原生 sqlite3* 句柄的工具
// ============================================================================
//
// 【这个文件解决什么问题】
//   dbridge 同步子系统的“变更捕获”依赖 SQLite 的 session 扩展（sqlite3session_*
//   系列 C API）。但这套 API 只接受原生的 sqlite3* 指针，而上层全程用 Qt 的
//   QSqlDatabase 管理连接。本类就是这两个世界之间的“桥”：
//     · of()              —— 把 QSqlDatabase 拆出它内部包着的 sqlite3*；
//     · sessionAvailable() —— 编译期能力探测：本 SQLite 是否带 session 扩展；
//     · exerciseSession()  —— 运行期能力探测：真的能创建/附着/导出 changeset 吗；
//     · libVersion()       —— 取版本串供日志记录。
//
// 【为什么需要“编译期 + 运行期”两道探测】
//   session/preupdate-hook 是 SQLite 的可选编译特性。
//     · 编译期：sqlite3_compileoption_used() 只能回答“编译时打开了这个宏吗”，
//       但它无法保证 Qt 的 QSQLITE 驱动链接进来的那份 SQLite 符号确实可用
//       （Qt 可能链接的是另一份未启用该特性的 libsqlite3）。
//     · 运行期：exerciseSession() 真刀真枪地建一个会话、附表、导出空 changeset，
//       只有全程返回 SQLITE_OK 才证明“链接进来的符号确实工作”。这是 H-03 fix
//       引入的“实地演练”校验，规避“宏开了但符号是另一份”的隐蔽陷阱。
//
// 【相关错误码】失败时上层通常回报 err::E_SYNC_SESSION_UNAVAILABLE（见 Errors.h）。
// ============================================================================

namespace dbridge::sync {

// 把 QSqlDatabase / 原生 sqlite3* 之间互转 + 探测 session 能力的纯静态工具类。
// 全部为静态方法，不持有任何状态，无需实例化。
class SqliteHandle {
   public:
    // 从 QSqlDatabase 中提取底层原生 sqlite3* 句柄。
    // 【为什么需要】Qt 的 SQL 层对上层屏蔽了原生句柄，但 session 扩展（C API）只认
    //   原生 sqlite3*，故必须先“掏”出来。
    // 【怎么做到】QSqlDriver::handle() 返回一个 QVariant，内部装的是 `sqlite3**`
    //   （指向句柄指针的指针，由 QSQLITE 驱动约定的封装方式）；故需 data() 拿到
    //   QVariant 内部存储地址，强转为 sqlite3** 后再解引用一次得到 sqlite3*。
    // 【参数】db —— 已 open 的 SQLite 连接。
    // 【返回】成功返回原生句柄；句柄无效（QVariant 不合法）时返回 nullptr。
    // 【线程】Qt 的数据库连接绑定创建它的线程；本函数必须在 db 的“所属线程”上调用。
    static sqlite3* of(QSqlDatabase& db);

    // 编译期能力探测：检查 SQLITE_ENABLE_SESSION 与 SQLITE_ENABLE_PREUPDATE_HOOK
    // 两个编译宏是否都被打开（二者缺一，session 捕获就无法工作）。
    // 【实现】sqlite3_compileoption_used() 是 SQLite 自身提供的内省函数，无论编译
    //   选项如何，它本身永远可用，因此可安全调用来查询其它选项是否开启。
    // 【参数】h —— 原生句柄；为 nullptr 直接判失败。
    // 【返回】两个特性都编译进来 → true；否则 false。
    // 【局限】只代表“编译时打开了宏”，不保证链接进来的符号真能用——后者交给
    //   exerciseSession() 做运行期实测。
    static bool sessionAvailable(sqlite3* h);

    // 运行期能力实测（H-03 fix）：在 h 上创建一个短命的 session、附着 tables 中
    // 列出的全部同步表、再导出一份（预期为空的）changeset。全程 SQLITE_OK 才算通过。
    // 【为什么不只信编译宏】见文件头注释——编译宏开启 ≠ Qt 链接进来的那份 SQLite
    //   符号确实可用。本函数真正“跑一遍 session 流程”来证明 PREUPDATE 钩子可工作。
    // 【参数】h     —— 原生句柄；tables —— 待附着的同步表名列表；
    //         err   —— 失败时填入可读原因（可为 nullptr）。
    // 【返回】演练成功 true；任一步失败 false 并填 *err。
    // 【副作用】函数内部自行创建并删除 session、释放导出的 changeset 缓冲，
    //   结束时不残留任何会话或内存（无副作用泄漏）。
    static bool exerciseSession(sqlite3* h, const QStringList& tables, QString* err);

    // 返回 SQLite 运行库的版本字符串（如 "3.39.0"），仅用于日志记录/诊断。
    static const char* libVersion();
};

}  // namespace dbridge::sync
