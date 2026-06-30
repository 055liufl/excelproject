#pragma once

// ── 场景2：单表「子节点B ⇄ 中心节点A」字段级对比对话框（类 Beyond Compare） ──
//
// 双击「表对比清单」中的某一表后弹出。左栏=子节点B（本地/当前节点），
// 右栏=中心节点A（远端）；两栏按主键行对齐，差异单元格红色高亮。
//
// 用户可：
//   · 选中某行 → 「→ 采用中心A · 整行」（acceptRemote）
//   · 选中右栏某个差异单元格 → 「→ 采用中心A · 选中字段（列级）」（stageCell，精确到列）
//   · 「← 保留本地B」撤销该行的采用决策（acceptLocal）
//
// 所有「采用」仅写入比对会话的内存暂存（StagingBuffer），关闭对话框不丢失；
// 真正落库由「表对比清单」上的「保存」按钮触发（model->save()）。

#include "dbridge/sync/IComparisonSession.h"

#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

class Scenario2Model;
class QTableWidget;
class QLabel;
class QCheckBox;

// ── CompareDetailDialog —— 单表字段级对比对话框（Beyond Compare 风格）─────────────
//
// 【做什么】并排展示某张表在「子节点B（本地）」与「中心节点A（远端）」两侧的逐行逐列
//   差异，让用户按行或按列「采用 A 的值」或「保留 B 的值」，决策仅写入比对会话的内存
//   暂存（StagingBuffer），落库延后由父窗口的「保存」触发。
// 【为什么独立成对话框】把「字段级精细对比 + 采用决策」从「表清单总览」中拆出来，
//   一表一弹窗，UI 聚焦、状态自包含；它通过 Scenario2Model* 读取 diff、写入暂存，
//   并以 stagingChanged() 信号回通知父窗口刷新汇总。
class CompareDetailDialog : public QDialog {
    Q_OBJECT
   public:
    // 构造：绑定数据模型与目标表名，搭建全部 UI 并首次拉取/填充差异（详见 .cpp 构造函数）。
    CompareDetailDialog(Scenario2Model* model, const QString& table, QWidget* parent = nullptr);

   signals:
    // 暂存状态发生变化时发出，通知父窗口刷新「表对比清单」的红/绿与待保存计数。
    void stagingChanged();

   private slots:
    // —— 用户操作槽（连到工具条按钮 / 复选框 / 表格选择变化）——
    void onAcceptRemoteRow();  // → 采用中心A 整行（把当前行整体改为 A 版本）
    void onAcceptRemoteCell();  // → 采用中心A 选中字段（仅当前单元格所在列改为 A 值，列级）
    void onAcceptLocalRow();         // ← 保留本地B（撤销该行的采用决策）
    void onToggleShowSame(bool on);  // 切换「显示相同行」：重建行集并重填表
    // 左右两栏的「当前单元格变化」槽：用于把一侧的选择镜像到另一侧（行列对齐联动）。
    void onLeftCurrentChanged(int row, int col, int prevRow, int prevCol);
    void onRightCurrentChanged(int row, int col, int prevRow, int prevCol);

   private:
    void rebuildRows();  // 依据 showSame_ 重新拉取行级 diff（并按主键升序排序）
    void buildTables();  // 按 rows_ 重置左右两栏的行列规模并逐行填充
    void populateRow(int idx);  // 重新渲染某一行（含暂存预览/高亮/差异符号）
    void refreshStatusLabel();  // 刷新底部状态栏：差异行数 / 本表已暂存数 / 全部待保存数
    // 取某行某列在「远端A 或本地B」一侧的显示字符串（找不到该列或值为空 → 空串）。
    QString cellValueString(const dbridge::sync::RowDiff& rd, const QString& column,
                            bool remoteSide) const;
    // 在一行的 cells 列表里按列名查 CellDiff*（找不到返回 nullptr）。
    const dbridge::sync::CellDiff* findCell(const dbridge::sync::RowDiff& rd,
                                            const QString& column) const;
    // 取「当前选中行」的下标：优先取右栏(A)的当前行，A 无选中则退回左栏(B)（-1=无选中）。
    int currentRowIndex() const;

    Scenario2Model* model_;  // 数据/暂存模型（diff 来源 + 采用决策的写入目标，不拥有）
    QString table_;        // 本对话框对比的表名
    QStringList columns_;  // 该表列名（含主键列），决定左右两栏的列序
    QString pkColumn_;     // 主键列名（用于排序/定位行）
    QList<dbridge::sync::RowDiff> rows_;  // 当前展示的行（差异行，可选含相同行）

    QTableWidget* tableB_ = nullptr;  // 左：子节点B（本地）
    QTableWidget* tableA_ = nullptr;  // 右：中心节点A（远端）
    QLabel* status_ = nullptr;        // 底部状态栏标签
    QCheckBox* showSame_ = nullptr;   // 「显示相同行」复选框

    bool syncGuard_ = false;  // 防止左右选择同步互相递归（一侧 setCurrentCell 会触发另一侧的槽）
};
