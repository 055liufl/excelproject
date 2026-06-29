# 自定义 QSQLITE（session-enabled）插件集成方案 —— 无 main 可改场景

> 记录日期：2026-06-29
> 关联提交：`7470403`（fix(sync): 集成 session-enabled QSQLITE 插件，彻底修复 E_SYNC_SESSION_UNAVAILABLE）
> 关联内存：`project_qsqlite_session.md`

---

## 一、提问

> 如果只有主程序的可执行文件，所以也找不到 main 函数，应该如何把自定义 QSQLITE 插件作为子项目集成到构建系统？

背景：此前的修复方案依赖在 `main()` 中调用 `QCoreApplication::setLibraryPaths()` 把应用目录提到插件搜索路径首位。但真实部署中，主程序可能是一个独立可执行文件、开发者拿不到其 `main` 源码，无法插入该代码。此时如何让自定义 QSQLITE 插件生效。

---

## 二、核心认识

**插件能否被加载，只取决于"哪个 `sqldrivers/libqsqlite.so` 在 Qt 的搜索路径里先被命中"，与有没有 `main` 函数完全无关。**

改 `main.cpp`（`setLibraryPaths`）只是"把应用目录提到最前"的一种手段。拿不到 `main` 时，改用 **部署位置 / 环境变量 / qt.conf** 这三层纯构建 & 配置手段，一行源码都不用碰。

### 前置判断：主程序运行时从哪里加载插件

| 形态 | 运行时插件来源 | 默认谁赢 |
|------|----------------|----------|
| **A 自包含部署**（exe 旁自带 `sqldrivers/`，不连系统 Qt，如 linuxdeployqt 产物） | exe 旁的 `sqldrivers/` | 旁边的 .so（唯一来源） |
| **B 依赖系统 Qt**（运行时从 `/opt/Qt.../plugins` 加载） | 系统 `sqldrivers/` 先于 appDir | 系统官方版（demo 原本踩的坑） |

> 关键事实：Qt 5.12 的 `libraryPaths()` 把 `applicationDirPath()` **append 到列表末尾**（`qcoreapplication.cpp:599`），系统插件路径在它之前；且 `QSqlDatabase` 选驱动时**先扫描到的目录里的同名插件赢**。这就是为什么"只把 .so 放到 exe 旁"在 B 形态下默认无效，需要额外手段提前它。

---

## 三、三种方案

### 方案 1 —— 构建系统覆盖部署位置（A 形态 / 生产首选）

自定义插件的 `TARGET=qsqlite`、类名 `QSQLiteDriverPlugin`、驱动 key `"QSQLITE"` 都与官方**完全一致**。只要让 session 版 `libqsqlite.so` 出现在那个 exe 唯一会扫的 `sqldrivers/` 里，Qt 加载的自然就是我们的版本，**零运行时配置、零源码改动**。

给 `3rdparty/qsqlite_session/qsqlite_session.pro` 加一段可被命令行覆盖的 `INSTALLS`：

```qmake
# 部署目标目录，qmake 时可覆盖：
#   qmake DBRIDGE_PLUGIN_INSTALL_DIR=/path/to/app/sqldrivers ...
isEmpty(DBRIDGE_PLUGIN_INSTALL_DIR) {
    DBRIDGE_PLUGIN_INSTALL_DIR = $$OUT_PWD/../../app/sqldrivers   # 合理默认
}
target.path = $$DBRIDGE_PLUGIN_INSTALL_DIR
INSTALLS   += target
```

之后 `make install` 就把 `libqsqlite.so` 覆盖到主程序的 `sqldrivers/`。若主程序也在本构建树里，直接让它的部署目录 == 这个路径即可（或在主程序 .pro 里用 `QMAKE_POST_LINK` 复制，跟当前 `sync-demo.pro` 写法一致）。

### 方案 2 —— `QT_PLUGIN_PATH` 环境变量（B 形态 / 开发期最省事）

Qt 在 `libraryPaths()` 里把 `QT_PLUGIN_PATH` 的路径 **append 在系统插件路径之前**（`qcoreapplication.cpp:2697`），所以它天然优先于系统官方版——这正是之前 `/tmp/custom_plugins` 能生效的原因。

- **Qt Creator**：在「运行 → 环境」里加一条
  `QT_PLUGIN_PATH = <构建目录>/deploy`（该目录下要有 `sqldrivers/libqsqlite.so`）
- 或部署一个 wrapper 启动脚本，`export QT_PLUGIN_PATH=...` 再 `exec` 主程序。

> 注意：指向的是**含 `sqldrivers/` 的父目录**，不是 .so 本身。

### 方案 3 —— `qt.conf`（B 形态 / 纯文件部署、不依赖环境）

在 exe 同目录放一个 `qt.conf`，重定向插件根目录：

```ini
[Paths]
Plugins = plugins
```

则 Qt 改去 exe 旁的 `plugins/` 找插件，其下需有 `sqldrivers/libqsqlite.so`。

> **代价**：它**替换**整个 `PluginsPath`，所以 `platforms/`、`imageformats/` 等其它必需插件也得一并放进这个目录——否则程序起不来。适合本来就做自包含部署的场景。

由构建系统生成它：

```qmake
QTCONF = $$OUT_PWD/app/qt.conf
write_file($$QTCONF, $$list("[Paths]" "Plugins = plugins"))
```

---

## 四、小结建议

- **真实生产部署** → 方案 1：让 `qsqlite_session` 子项目通过 `INSTALLS` 把 session 版 `libqsqlite.so` 覆盖进主程序 `sqldrivers/`。最干净，主程序无感知。
- **开发期在 IDE 里跑别人的 exe** → 方案 2：`QT_PLUGIN_PATH` 一条环境变量搞定。
- demo 里改 `main`（`setLibraryPaths`）只是因为 demo 是 B 形态又恰好能改源码，**不是唯一解，也不是推荐解**。

---

## 五、附：当前已落地的内容（commit 7470403）

1. **新建 `3rdparty/qsqlite_session/qsqlite_session.pro`** —— 从 Qt 5.12 自带 QSQLITE 插件源码（`qsql_sqlite.cpp` / `smain.cpp`）+ Qt bundled `sqlite3.c` 重编，叠加 `SQLITE_ENABLE_SESSION + SQLITE_ENABLE_PREUPDATE_HOOK`，使插件内嵌 SQLite 与 `libdbridge_sqlite3.a` 保持相同结构体布局。
2. **`dbridge.pro`** —— 将 `qsqlite_session` 加为子项目，在 `cli/syncdemo/diffdemo` 之前构建。
3. **`sync-demo.pro` / `diff-demo.pro`** —— 增加 `QMAKE_POST_LINK`，链接后把 `libqsqlite.so` 复制到 `$OUT_PWD/sqldrivers/`。
4. **`sync-demo/main.cpp` / `diff-demo/main.cpp`** —— 在首次 SQL 驱动加载前调用 `QCoreApplication::setLibraryPaths([appDir] + existing)`，将应用目录置于搜索列表首位（**必须用 `setLibraryPaths`，不能用 `addLibraryPath`**：Qt 5.12 中 appDir 已在列表里，`addLibraryPath` 会判定重复直接返回，是彻底的 no-op）。
