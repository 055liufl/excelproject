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

class CompareDetailDialog : public QDialog {
    Q_OBJECT
   public:
    CompareDetailDialog(Scenario2Model* model, const QString& table, QWidget* parent = nullptr);

   signals:
    // 暂存状态发生变化时发出，通知父窗口刷新「表对比清单」的红/绿与待保存计数。
    void stagingChanged();

   private slots:
    void onAcceptRemoteRow();   // → 采用中心A 整行
    void onAcceptRemoteCell();  // → 采用中心A 选中字段（列级）
    void onAcceptLocalRow();    // ← 保留本地B
    void onToggleShowSame(bool on);
    void onLeftCurrentChanged(int row, int col, int prevRow, int prevCol);
    void onRightCurrentChanged(int row, int col, int prevRow, int prevCol);

   private:
    void rebuildRows();         // 依据 showSame_ 重新拉取行级 diff
    void buildTables();         // 按 rows_ 填充左右两栏
    void populateRow(int idx);  // 重新渲染某一行（含暂存预览/高亮）
    void refreshStatusLabel();
    QString cellValueString(const dbridge::sync::RowDiff& rd, const QString& column,
                            bool remoteSide) const;
    const dbridge::sync::CellDiff* findCell(const dbridge::sync::RowDiff& rd,
                                            const QString& column) const;
    int currentRowIndex() const;

    Scenario2Model* model_;
    QString table_;
    QStringList columns_;
    QString pkColumn_;
    QList<dbridge::sync::RowDiff> rows_;  // 当前展示的行（差异行，可选含相同行）

    QTableWidget* tableB_ = nullptr;  // 左：子节点B（本地）
    QTableWidget* tableA_ = nullptr;  // 右：中心节点A（远端）
    QLabel* status_ = nullptr;
    QCheckBox* showSame_ = nullptr;

    bool syncGuard_ = false;  // 防止左右选择同步互相递归
};
