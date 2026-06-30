#include "OutboxWriter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>

// M-03 fix: guard POSIX-only headers so the library compiles on non-UNIX targets.
// 译：M-03 fix —— 用 Q_OS_UNIX 包住仅 POSIX 才有的头文件（fsync/open 等），
//   使本库在 Windows 等非 UNIX 平台也能编译（那些平台跳过显式 fsync，依赖系统语义）。
#ifdef Q_OS_UNIX
#include <fcntl.h>   // open / O_RDONLY（为 fsync 目录而打开目录 fd）
#include <unistd.h>  // fsync / close
#endif

// ============================================================================
// OutboxWriter.cpp — 原子发布协议的实现
// 三步发布协议（.tmp → fsync → rename → .ready → fsync 目录）的动机详见
// 配套头文件 OutboxWriter.h 的文件头注释。
// ============================================================================

namespace dbridge::sync {

OutboxWriter::OutboxWriter(const QString& outboxDir) : dir_(outboxDir) {
    // 仅记录目标 outbox 目录；目录不存在不在此报错，留到首次写入时按需 mkpath。
}

// write / writeAck 仅是语义不同的两个对外入口，落地实现完全一致（都走原子发布）。
// 区分二者只为可读性与命名层面的语义（主载荷 vs ACK 确认），文件类型由 finalName 后缀决定。
bool OutboxWriter::write(const QString& artifactName, const QByteArray& data, QString* err) {
    return writeAtomic(artifactName, data, err);
}

bool OutboxWriter::writeAck(const QString& ackName, const QByteArray& data, QString* err) {
    return writeAtomic(ackName, data, err);
}

bool OutboxWriter::writeAtomic(const QString& finalName, const QByteArray& data, QString* err) {
    // 按需创建 outbox 目录（首次写入时若不存在则 mkpath 递归创建）。
    QDir d(dir_);
    if (!d.exists()) {
        if (!d.mkpath(QStringLiteral("."))) {
            if (err)
                *err = QStringLiteral("cannot create outbox dir: %1").arg(dir_);
            return false;
        }
    }

    // 最终文件名 finalPath（搬运层/接收方认这个名字）+ 临时写入名 tmpPath（多一个 .tmp 后缀）。
    // 搬运层只认最终名与 .ready，绝不碰 .tmp，故“正在写一半”的 .tmp 对它不可见。
    const QString finalPath = d.filePath(finalName);
    const QString tmpPath = finalPath + QStringLiteral(".tmp");

    // 1. Write to .tmp（第一步：先把数据完整写到临时文件）
    {
        QFile f(tmpPath);
        // Truncate：若上次因崩溃残留同名 .tmp，这里覆盖清空，避免拼接出脏数据。
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (err)
                *err = QStringLiteral("cannot open tmp file: %1").arg(f.errorString());
            return false;
        }
        // write 返回实际写入字节数；与 data.size() 不等即「部分写」失败，必须清理 .tmp 再退出。
        if (f.write(data) != data.size()) {
            if (err)
                *err = QStringLiteral("partial write to tmp: %1").arg(f.errorString());
            f.remove();
            return false;
        }
        // 2. flush + fsync (POSIX only; Qt flush is best-effort on other platforms)
        // 译：第二步 刷盘。flush 把 Qt/用户态缓冲推给内核；fsync 再把内核页缓存真正落到
        //   物理磁盘（掉电也不丢）。非 POSIX 平台没有 fsync，只能best-effort 依赖 flush。
        if (!f.flush()) {
            if (err)
                *err = QStringLiteral("flush failed: %1").arg(f.errorString());
            f.remove();
            return false;
        }
#ifdef Q_OS_UNIX
        int fd = static_cast<int>(f.handle());  // 取底层文件描述符以调用 ::fsync
        if (fd >= 0 && ::fsync(fd) != 0) {
            // M-04 fix: treat fsync failure as a transport error — data may not be durable.
            // 译：M-04 fix —— fsync 失败按传输错误处理：数据可能并未真正落盘，不能假装成功。
            //   失败路径要：填 errno 诊断 → 关闭文件 → 删除半成品 .tmp → 返回 false。
            if (err)
                *err = QStringLiteral("fsync failed for %1 (errno=%2)").arg(tmpPath).arg(errno);
            f.close();
            QFile::remove(tmpPath);
            return false;
        }
#endif
        f.close();  // 关闭后再 rename，避免某些平台“持有打开句柄时改名”的问题
    }

    // 3. Atomic rename (tmp → final).
    // I-11 fix: POSIX rename() is atomic and replaces the destination if it exists,
    // so the prior QFile::remove(finalPath) window has been removed.
    // 译：第三步 原子改名（tmp → final）。
    // I-11 fix：POSIX 的 rename() 本身是原子的、且目标已存在时会原子替换；因此旧实现里
    //   “先 remove(finalPath) 再 rename”那段「删除与改名之间」的危险时间窗（此刻最终名
    //   短暂消失，可能被并发误判）已被去掉——现在直接 rename 覆盖，无中间真空态。
    if (!QFile::rename(tmpPath, finalPath)) {
        if (err)
            *err = QStringLiteral("rename failed: %1 → %2").arg(tmpPath, finalPath);
        QFile::remove(tmpPath);  // 改名失败：清理 .tmp，不留垃圾
        return false;
    }

    // M-08 fix: once the main payload has been renamed into place, any failure of the .ready
    // marker or directory fsync must clean up BOTH the final payload and the .ready file.
    // Otherwise an orphan payload (visible without its .ready sentinel) is left behind and a
    // same-name retry could fail or be mis-scanned by the third-party mover.
    // 译：M-08 fix —— 主载荷一旦改名就位，后续任何一步（写 .ready / fsync 目录）失败，
    //   都必须把「最终载荷」与「.ready」一并清理掉。否则会留下一个「有主文件却没有 .ready
    //   哨兵」的孤儿载荷——既可能让同名重试失败，也可能被第三方搬运器误扫（它本约定“只有
    //   .ready 出现才算齐全”，但孤儿主文件的存在仍是隐患）。
    const QString readyPath = finalPath + QStringLiteral(".ready");
    // 统一的「清理并失败」收尾 lambda：填错误信息 + 删 .ready + 删主文件 + 返回 false。
    auto cleanupAndFail = [&](const QString& msg) -> bool {
        if (err)
            *err = msg;
        QFile::remove(readyPath);
        QFile::remove(finalPath);
        return false;
    };

    // 4. Write .ready marker (empty file signals receiver), with flush + fsync.
    // 译：第四步 写 .ready 哨兵（一个空文件即可，它的“存在”本身就是信号），同样 flush+fsync。
    //   接收方/搬运层约定：只有当 .ready 出现，同名主文件才被认为“数据齐全、可消费”。
    {
        QFile rf(readyPath);
        if (!rf.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return cleanupAndFail(QStringLiteral("cannot write .ready: %1").arg(rf.errorString()));
        // I-11 fix: flush + fsync the .ready marker so it is durable before return.
        // 译：I-11 fix —— 返回前把 .ready 也 flush+fsync，确保哨兵本身掉电可存活
        //   （否则可能出现“主文件持久了、.ready 还在缓存里丢了”的不一致）。
        if (!rf.flush())
            return cleanupAndFail(QStringLiteral("flush .ready failed: %1").arg(rf.errorString()));
#ifdef Q_OS_UNIX
        int rfd = static_cast<int>(rf.handle());
        if (rfd >= 0 && ::fsync(rfd) != 0) {
            rf.close();
            return cleanupAndFail(QStringLiteral("fsync .ready failed (errno=%1)").arg(errno));
        }
#endif
        rf.close();
    }

    // 5. I-11 fix: fsync the containing directory so the rename and .ready creation
    // survive a crash (required on POSIX; no-op on Windows where QFile::rename uses
    // MoveFileEx which is already durable).
    // 译：第五步 fsync「目录」本身。
    //   关键且不直观：在 POSIX 上，仅 fsync 文件「内容」并不保证「目录项」（即文件名→inode
    //   的映射、rename 的结果、.ready 的创建）也落盘；必须额外对承载它们的目录做一次 fsync，
    //   rename 与新建的 .ready 才能在掉电后存活。Windows 下 QFile::rename 走 MoveFileEx，
    //   本身已是持久的，故此步在 Windows 是 no-op（被 #ifdef 跳过）。
#ifdef Q_OS_UNIX
    {
        // 以只读方式 open 目录拿到其 fd（POSIX 允许对目录 fd 调用 fsync）。
        QByteArray dirPath = d.absolutePath().toLocal8Bit();
        int dirFd = ::open(dirPath.constData(), O_RDONLY);
        if (dirFd < 0) {
            // M-06 fix: directory open failure means we cannot fsync — treat as transport error.
            // 译：M-06 fix —— 连目录都打不开就无从 fsync；不能假装成功，按传输错误清理并失败。
            return cleanupAndFail(QStringLiteral("cannot open dir for fsync: %1 (errno=%2)")
                                      .arg(d.absolutePath())
                                      .arg(errno));
        }
        const int rc = ::fsync(dirFd);
        ::close(dirFd);  // 无论成败都要关闭目录 fd，避免泄漏
        if (rc != 0) {
            return cleanupAndFail(QStringLiteral("dir fsync failed for %1 (errno=%2)")
                                      .arg(d.absolutePath())
                                      .arg(errno));
        }
    }
#endif

    // 五步全部成功：artifact 已完整、持久、且对搬运层“可见且就绪”。
    return true;
}

}  // namespace dbridge::sync
