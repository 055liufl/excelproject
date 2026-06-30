// ============================================================================
// SyncContext.cpp — SyncContextRegistry 的实现（声明与设计背景见 SyncContext.h）
// ============================================================================
// 本文件实现“按物理库唯一”的上下文登记表：
//   · canonicalKey()    —— 把路径解析成 OS 文件身份字符串（POSIX dev+inode / Win 卷+索引）；
//   · getOrCreate()     —— 命中即 refCount++，否则新建；
//   · getExisting()     —— 只读旁路查询（不增计数）；
//   · release()         —— 减计数、归零即销毁；
//   · ensureContextUuid—— 把上下文 UUID 持久化进库，支持跨进程重启复用。
// 关键不直观处：为何不用路径字符串判等（别名/符号链接问题），见各函数内注释与头文件。
// ============================================================================
#include "SyncContext.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

// 计算 OS 文件身份所需的平台头：Windows 用 Win32 API，POSIX 用 stat()。
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace dbridge::sync {

// instance —— 进程唯一的登记表（Meyers 单例）。
// 局部静态变量的初始化由 C++ 保证线程安全且只发生一次；随进程退出而析构。
SyncContextRegistry& SyncContextRegistry::instance() {
    static SyncContextRegistry reg;
    return reg;
}

// canonicalKey —— 把数据库文件路径解析为“规范的 OS 文件身份键”。
// 做什么：取文件的操作系统级身份（而非路径字符串），作为登记表的 key。
// 为什么：同一物理文件可有多种路径写法（相对/URI/符号链接/盘符别名）；只有用 inode 这类
//   身份判等，所有别名才会映射到同一个 SyncContext，从而维持“单写者”不变量（§4.3/G-07）。
// 参数：path 数据库文件路径；err 失败原因输出（可空）。
// 返回：形如 "posix:<dev>:<ino>" 或 "win:<vol>:<index>" 的键；失败返回空串并写 *err。
// 错误模式：路径为空 / 文件不存在或不可访问（见 M-04：不做路径回退）。
QString SyncContextRegistry::canonicalKey(const QString& path, QString* err) {
    if (path.isEmpty()) {
        if (err)
            *err = QStringLiteral("empty path");
        return {};
    }

#ifdef Q_OS_WIN
    // Windows：以共享读写删方式打开句柄，仅为读取文件身份（不需要任何访问权限位 → 第二参 0）。
    // FILE_FLAG_BACKUP_SEMANTICS 让该调用也能用于目录句柄，OPEN_EXISTING 要求文件已存在。
    HANDLE h = CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        // M-04 fix: database file must exist before we can obtain an OS file identity.
        // A path-string fallback could let two aliases create separate SyncContexts,
        // breaking the single-writer guarantee (design §4.3/G-07).
        // M-04 修复：必须先有数据库文件才能取得 OS 文件身份。若退而用“路径字符串”兜底，
        // 两个别名就可能各建一个 SyncContext，破坏单写者保证（设计 §4.3/G-07）。
        if (err)
            *err =
                QStringLiteral("Cannot open database file for identity resolution: %1").arg(path);
        return {};
    }
    BY_HANDLE_FILE_INFORMATION info;
    bool ok = GetFileInformationByHandle(h, &info);  // 取卷序列号 + 文件索引（= NTFS 上的“inode”）
    CloseHandle(h);                                  // 句柄只用于查身份，立即关闭。
    if (!ok) {
        if (err)
            *err = QStringLiteral("GetFileInformationByHandle failed for: %1").arg(path);
        return {};
    }
    // 卷序列号唯一定位“哪个卷”，64 位文件索引（高 32 位<<32 | 低 32 位）唯一定位“卷内哪个文件”。
    return QStringLiteral("win:%1:%2")
        .arg(info.dwVolumeSerialNumber)
        .arg(((quint64)info.nFileIndexHigh << 32) | info.nFileIndexLow);
#else
    // POSIX：stat() 取 设备号 dev + inode 号 ino，二者合起来唯一标识一个文件实体。
    struct stat st;
    if (::stat(path.toLocal8Bit().constData(), &st) != 0) {
        // M-04 fix: no path fallback — require the file to exist so OS inode is the key.
        // M-04 修复：不做路径回退——要求文件必须存在，从而以 inode 作为键。
        if (err)
            *err = QStringLiteral("Database file not found or not accessible: %1 (errno=%2)")
                       .arg(path)
                       .arg(errno);
        return {};
    }
    return QStringLiteral("posix:%1:%2").arg((quint64)st.st_dev).arg((quint64)st.st_ino);
#endif
}

// getOrCreate —— 取得或新建 sqlitePath 对应的共享上下文。
// 流程：先解析规范键（失败即返回 nullptr）；在锁内查表：命中则 refCount++ 复用同一实例，
//   未命中则新建（refCount=1，分配一个上下文 UUID）。把规范键回填 *canonicalKeyOut 供 release()。
// 线程：全程持 mutex_，并发安全。副作用：可能改写 registry_ 与命中条目的 refCount。
std::shared_ptr<SyncContext> SyncContextRegistry::getOrCreate(const QString& sqlitePath,
                                                              QString* canonicalKeyOut,
                                                              QString* err) {
    QString key = canonicalKey(sqlitePath, err);
    if (key.isEmpty())
        return nullptr;  // 身份解析失败（如文件不存在），*err 已被 canonicalKey 写好。

    if (canonicalKeyOut)
        *canonicalKeyOut = key;  // 回填规范键：调用方析构时须用它 release()。

    QMutexLocker lk(&mutex_);
    auto it = registry_.find(key);
    if (it != registry_.end()) {
        it.value()->refCount++;  // 已有同库上下文 → 复用并增引用。
        return it.value();
    }

    // 首次见到该库 → 新建上下文。
    auto ctx = std::make_shared<SyncContext>();
    ctx->refCount = 1;
    // 先生成一个内存态 UUID；若库中已持久化了别的 UUID，稍后 ensureContextUuid() 会回读采用之。
    ctx->contextUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    registry_.insert(key, ctx);
    return ctx;
}

std::shared_ptr<SyncContext> SyncContextRegistry::getExisting(const QString& sqlitePath) {
    // J-10: Look up an existing context without creating one and without touching refCount.
    // J-10：只查询已有上下文——既不新建条目，也不动 refCount。
    QString key = canonicalKey(sqlitePath, nullptr);  // 旁路查询无需 err（找不到就返回 null）。
    if (key.isEmpty())
        return nullptr;

    QMutexLocker lk(&mutex_);
    auto it = registry_.find(key);
    if (it == registry_.end())
        return nullptr;
    // Return a copy of the shared_ptr — shared ownership keeps the object alive for the
    // duration of the caller's inspection, but we do NOT increment refCount (the internal
    // counter used by release()). The caller must not call release() for this pointer.
    // 返回 shared_ptr 的拷贝——共享所有权可在调用方“查看期间”保活对象，但【不】增加
    // refCount（那是 release() 配对使用的内部计数器）。故经本方法取得的指针不可 release()。
    return it.value();
}

// release —— 减少引用计数；归零即销毁该上下文。
// 注意参数名 canonicalKey_ 末尾下划线是为避开与静态成员函数 canonicalKey() 同名。
// 找不到 key（如重复 release / 从未注册）则安全 no-op。
void SyncContextRegistry::release(const QString& canonicalKey_) {
    QMutexLocker lk(&mutex_);
    auto it = registry_.find(canonicalKey_);
    if (it == registry_.end())
        return;
    if (--it.value()->refCount <= 0) {  // 最后一个使用者撒手 → 从表中抹除。
        registry_.erase(it);  // erase 后该 SyncContext 随其最后一个 shared_ptr 析构。
    }
}

// ensureContextUuid —— 把上下文 UUID 持久化进库内 __sync_context_meta（k/v 元数据表）。
// 目的（H-01）：让“同一物理库”始终关联“同一 UUID”，即便跨进程重启。内存里每次重启都会
//   新生成一个 UUID，但若库中已存了上次的 UUID，就采用库里那个，使重启场景正确衔接。
// 行为分支（详见头文件）：库无 UUID→写入；库已是同值→无操作；库是别的值→回读采用之。
// 参数：db 写连接；uuid 出入参（入为内存态 UUID，出为最终生效 UUID）；err 失败原因。
// 返回：仅当 DB 查询本身失败才返回 false（建表/读/写任一失败）；其余皆 true。
bool SyncContextRegistry::ensureContextUuid(QSqlDatabase& db, QString* uuid, QString* err) {
    // H-01 fix: read-or-write the context UUID in __sync_context_meta.
    // On restart the in-memory UUID is freshly generated; if the DB already holds a UUID
    // from a previous run, we adopt that stored value so restart works correctly.
    // H-01 修复：在 __sync_context_meta 中“读或写”上下文 UUID。
    // 重启时内存 UUID 是新生成的；若库里已持有上一轮运行写入的 UUID，则采用该持久值，使重启正确。
    if (!uuid) {
        if (err)
            *err = QStringLiteral("uuid pointer is null");
        return false;
    }

    // ① 确保元数据表存在（幂等 DDL）。
    QSqlQuery ddl(db);
    if (!ddl.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS __sync_context_meta("
                                 "k TEXT NOT NULL PRIMARY KEY, v TEXT NOT NULL)"))) {
        if (err)
            *err = QStringLiteral("cannot create __sync_context_meta: ") + ddl.lastError().text();
        return false;
    }

    // ② 尝试读取已持久化的 context_uuid。
    QSqlQuery sel(db);
    sel.prepare(QStringLiteral("SELECT v FROM __sync_context_meta WHERE k='context_uuid'"));
    if (!sel.exec()) {
        if (err)
            *err = QStringLiteral("cannot read context_uuid: ") + sel.lastError().text();
        return false;
    }
    if (sel.next()) {
        // UUID already stored — adopt the persisted value (supports restart across processes).
        // 库中已有 UUID → 采用持久值（支持跨进程重启复用，覆盖内存里新生成的那个）。
        *uuid = sel.value(0).toString();
        return true;
    }

    // ③ 库中尚无 → 写入当前内存 UUID（若为空则先生成一个）。
    if (uuid->isEmpty())
        *uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QSqlQuery ins(db);
    ins.prepare(QStringLiteral("INSERT INTO __sync_context_meta(k, v) VALUES('context_uuid', ?)"));
    ins.addBindValue(*uuid);
    if (!ins.exec()) {
        if (err)
            *err = QStringLiteral("cannot persist context_uuid: ") + ins.lastError().text();
        return false;
    }
    return true;
}

}  // namespace dbridge::sync
