#pragma once

// ── 场景2 顶层界面：「表对比清单」+ 取消/保存 ────────────────────────────────
//
// 对应设计图场景2 的主表格：列出参与比对的每张表，标注 子节点B 表名、
// 中心节点A 表名、以及「差异属性」（相同=绿，不同=红）。双击某行进入
// CompareDetailDialog 做字段级对比与列级采用。底部「保存」把内存暂存写回 B 库，
// 「取消」放弃暂存，「重置演示数据」恢复初始的差异状态。

#include <QString>
#include <QWidget>

#include "Scenario2Model.h"
#include <memory>

class QTableWidget;
class QLabel;
class QPushButton;

class Scenario2Widget : public QWidget {
    Q_OBJECT
   public:
    explicit Scenario2Widget(const QString& ws, QWidget* parent = nullptr);

   private slots:
    void onRowDoubleClicked(int row, int column);
    void onSave();
    void onCancel();
    void onReset();

   private:
    void refreshTable();  // 依据 model_ 的最新差异刷新清单与按钮状态

    QString ws_;
    std::unique_ptr<Scenario2Model> model_;
    bool ready_ = false;

    QTableWidget* tableList_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* btnSave_ = nullptr;
    QPushButton* btnCancel_ = nullptr;
    QPushButton* btnReset_ = nullptr;
};
