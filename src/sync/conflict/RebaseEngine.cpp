#include "sync/conflict/RebaseEngine.h"

#include <sqlite3.h>

// RebaseEngine 的实现:对 SQLite C 级 rebaser API 的薄封装,严格做到“有创建必有释放”。
// 流程:create → configure(灌入 rebase buffer)→ rebase(改写 changeset)→ delete。

namespace dbridge::sync {

bool RebaseEngine::rebase(const QByteArray& rebaseBuffer, const QByteArray& changeset,
                          QByteArray* rebased, QString* err) {
    // ① 创建 rebaser 句柄。
    sqlite3_rebaser* pRebaser = nullptr;
    int rc = sqlite3rebaser_create(&pRebaser);
    if (rc != SQLITE_OK) {
        if (err)
            *err = "sqlite3rebaser_create failed: " + QString::number(rc);
        return false;
    }

    // ② 配置:把本地冲突处理结果(rebase buffer)灌入 rebaser,作为变基的“权威基础”。
    rc = sqlite3rebaser_configure(pRebaser, rebaseBuffer.size(), rebaseBuffer.constData());
    if (rc != SQLITE_OK) {
        sqlite3rebaser_delete(pRebaser);  // 失败也要释放句柄,避免泄漏
        if (err)
            *err = "sqlite3rebaser_configure failed: " + QString::number(rc);
        return false;
    }

    // ③ 执行变基:输出缓冲由 SQLite 用 sqlite3_malloc 分配,需在用完后 sqlite3_free。
    int nOut = 0;
    void* pOut = nullptr;
    rc = sqlite3rebaser_rebase(pRebaser, changeset.size(), changeset.constData(), &nOut, &pOut);
    sqlite3rebaser_delete(pRebaser);  // rebaser 句柄此后不再需要,立即释放
    if (rc != SQLITE_OK) {
        if (err)
            *err = "sqlite3rebaser_rebase failed: " + QString::number(rc);
        return false;
    }

    // ④ 把 C 缓冲拷贝进 QByteArray(深拷贝),随后释放 SQLite 分配的原始内存。
    *rebased = QByteArray(static_cast<const char*>(pOut), nOut);
    sqlite3_free(pOut);
    return true;
}

}  // namespace dbridge::sync
