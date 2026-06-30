#pragma once

// ── 场景1 界面：拓扑图示 + 运行按钮 + 实时日志 + 收敛网格 ─────────────────────
//
// 点击「运行」后在后台线程（Scenario1Runner）执行真实多节点 UDP 同步，
// 并通过信号把日志、阶段、最终收敛网格实时刷新到界面。

#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class Scenario1Runner;
class QTextEdit;
class QTableWidget;
class QPushButton;
class QLabel;

class Scenario1Widget : public QWidget {
    Q_OBJECT
   public:
    explicit Scenario1Widget(const QString& ws, QWidget* parent = nullptr);

   private slots:
    void onRun();
    void onLogLine(const QString& line, int sev);
    void onPhase(const QString& title);
    void onGrid(const QStringList& headers, const QVector<QStringList>& rows,
                const QString& caption);
    void onFinished(bool ok);

   private:
    void appendLog(const QString& html);

    QString ws_;
    Scenario1Runner* runner_ = nullptr;

    QPushButton* btnRun_ = nullptr;
    QLabel* phase_ = nullptr;
    QTextEdit* logView_ = nullptr;
    QTableWidget* grid_ = nullptr;
    QLabel* gridCaption_ = nullptr;
};
