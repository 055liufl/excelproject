#pragma once

#include <QMainWindow>
#include <QString>

class Scenario1Widget;
class Scenario2Widget;

// 主窗口：用 QTabWidget 承载两个场景的界面。
//
// 标签页1 —— 场景1（多节点指定库同步，含拓扑图示、实时日志、收敛网格）
// 标签页2 —— 场景2（类 Beyond Compare 差异比对与列级同步 GUI）
class MainWindow : public QMainWindow {
    Q_OBJECT
   public:
    // ws：本程序的工作目录；两个场景分别在 ws/scenario1、ws/scenario2 下落地数据。
    explicit MainWindow(const QString& ws, QWidget* parent = nullptr);

   private:
    QString ws_;
    Scenario1Widget* scenario1_ = nullptr;
    Scenario2Widget* scenario2_ = nullptr;
};
