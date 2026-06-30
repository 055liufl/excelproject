#include "Scenario1Widget.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

#include "Scenario1Runner.h"

Scenario1Widget::Scenario1Widget(const QString& ws, QWidget* parent) : QWidget(parent), ws_(ws) {
    auto* root = new QVBoxLayout(this);

    // —— 说明 ——
    auto* desc = new QLabel(this);
    desc->setTextFormat(Qt::RichText);
    desc->setWordWrap(true);
    desc->setText(
        QStringLiteral("<b>场景1 · 多节点指定库同步</b>（真实 ISyncEngine + UDP 文件传输层）<br/>"
                       "中心节点A ⇄ 子节点B / C / D 同属一个域；<b>指定数据库</b>为全域权威源。"
                       "演示：① 指定库基线下发全域；② 子节点离线误改与指定库<b>并发冲突</b> → "
                       "按 rank 仲裁，<i>以指定数据为准</i>；③ 子节点重连指定库重导入自我纠正；"
                       "④ 全域收敛校验。"));
    root->addWidget(desc);

    // —— 拓扑图示（等宽字体）——
    auto* topo = new QLabel(this);
    topo->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    topo->setText(
        QStringLiteral("        ┌──────────────┐\n"
                       "        │   指定数据库   │  （权威源，以其数据为准）\n"
                       "        └───────┬──────┘\n"
                       "                │ 连接 / 导入\n"
                       "        ┌───────▼──────┐\n"
                       "   ┌────┤   中心节点A    ├────┐\n"
                       "   │    └───────┬──────┘    │   （双向同步）\n"
                       "   ▼            ▼           ▼\n"
                       "┌──────┐   ┌──────┐    ┌──────┐\n"
                       "│子节点B│   │子节点C│    │子节点D│\n"
                       "└──────┘   └──────┘    └──────┘"));
    topo->setStyleSheet(QStringLiteral(
        "background:#fbfdff;border:1px solid #d7dde5;border-radius:6px;padding:6px;"));
    root->addWidget(topo);

    // —— 运行条 ——
    auto* bar = new QHBoxLayout();
    btnRun_ = new QPushButton(QStringLiteral("▶ 运行场景1演示"), this);
    btnRun_->setStyleSheet(QStringLiteral("font-weight:bold;padding:4px 14px;"));
    connect(btnRun_, &QPushButton::clicked, this, &Scenario1Widget::onRun);
    bar->addWidget(btnRun_);
    phase_ = new QLabel(QStringLiteral("（就绪）"), this);
    phase_->setStyleSheet(QStringLiteral("color:#1565c0;font-weight:bold;"));
    bar->addWidget(phase_);
    bar->addStretch(1);
    root->addLayout(bar);

    // —— 实时日志 ——
    logView_ = new QTextEdit(this);
    logView_->setReadOnly(true);
    logView_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    logView_->setStyleSheet(QStringLiteral("background:#1e1e1e;color:#dcdcdc;"));
    root->addWidget(logView_, 1);

    // —— 收敛网格 ——
    gridCaption_ = new QLabel(QStringLiteral("收敛结果（运行后显示）"), this);
    gridCaption_->setStyleSheet(QStringLiteral("font-weight:bold;"));
    root->addWidget(gridCaption_);
    grid_ = new QTableWidget(this);
    grid_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    grid_->verticalHeader()->setVisible(false);
    grid_->setMaximumHeight(160);
    root->addWidget(grid_);
}

void Scenario1Widget::appendLog(const QString& html) {
    QTextCursor c = logView_->textCursor();
    c.movePosition(QTextCursor::End);
    c.insertHtml(html);
    c.insertBlock();
    logView_->setTextCursor(c);
    logView_->ensureCursorVisible();
}

void Scenario1Widget::onRun() {
    if (runner_)
        return;  // 已在运行
    btnRun_->setEnabled(false);
    btnRun_->setText(QStringLiteral("运行中 ..."));
    logView_->clear();
    grid_->setRowCount(0);
    gridCaption_->setText(QStringLiteral("收敛结果（运行中 ...）"));

    runner_ = new Scenario1Runner(ws_, this);
    connect(runner_, &Scenario1Runner::logLine, this, &Scenario1Widget::onLogLine);
    connect(runner_, &Scenario1Runner::phaseChanged, this, &Scenario1Widget::onPhase);
    connect(runner_, &Scenario1Runner::gridReady, this, &Scenario1Widget::onGrid);
    connect(runner_, &Scenario1Runner::runFinished, this, &Scenario1Widget::onFinished);
    // 线程结束后自动回收。
    connect(runner_, &QThread::finished, runner_, &QObject::deleteLater);
    connect(runner_, &QObject::destroyed, this, [this] { runner_ = nullptr; });
    runner_->start();
}

void Scenario1Widget::onLogLine(const QString& line, int sev) {
    const QString color = sev >= 2   ? QStringLiteral("#ff6b6b")
                          : sev == 1 ? QStringLiteral("#ffd166")
                                     : QStringLiteral("#9cdcfe");
    appendLog(QStringLiteral("<span style='color:%1'>%2</span>").arg(color, line.toHtmlEscaped()));
}

void Scenario1Widget::onPhase(const QString& title) {
    phase_->setText(title);
    appendLog(QStringLiteral("<span style='color:#4ec9b0;font-weight:bold'>━━ %1 ━━</span>")
                  .arg(title.toHtmlEscaped()));
}

void Scenario1Widget::onGrid(const QStringList& headers, const QVector<QStringList>& rows,
                             const QString& caption) {
    gridCaption_->setText(caption);
    grid_->setColumnCount(headers.size());
    grid_->setHorizontalHeaderLabels(headers);
    grid_->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        const QStringList& r = rows.at(i);
        for (int j = 0; j < r.size() && j < headers.size(); ++j) {
            auto* it = new QTableWidgetItem(r.at(j));
            if (j == headers.size() - 1) {  // 收敛列着色
                const bool ok = r.at(j).startsWith(QStringLiteral("✓"));
                it->setBackground(QBrush(ok ? QColor(0xd6, 0xf5, 0xd6) : QColor(0xff, 0xd6, 0xd6)));
            }
            grid_->setItem(i, j, it);
        }
    }
    grid_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
}

void Scenario1Widget::onFinished(bool ok) {
    btnRun_->setEnabled(true);
    btnRun_->setText(QStringLiteral("▶ 重新运行场景1演示"));
    phase_->setText(ok ? QStringLiteral("✅ 演示完成（全域已收敛）")
                       : QStringLiteral("⚠ 演示结束（存在未收敛项，见日志）"));
}
