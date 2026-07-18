/**
 * @file mainwindow.cpp
 * @brief 主窗口——系统托盘RGB主题 + 图标
 * @date 2026-07-06
 */

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "src/ui/SettingsPanel.h"
#include "src/logging/LoggerManager.h"
#include <QApplication>
#include <QMenu>
#include <QAction>
#include <QSystemTrayIcon>
#include <QStyle>
#include <QScreen>
#include <QGuiApplication>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setupTrayIcon();
    // 安全：预热创建设置面板，避免首次打开时卡顿
    m_settingsPanel = new SettingsPanel();
    connect(m_settingsPanel, &SettingsPanel::minimizeToTray, this, [this]() {
        m_settingsPanel->hide();
    });
    m_settingsPanel->hide();
    hide();
}

MainWindow::~MainWindow() { delete ui; }

void MainWindow::setupTrayIcon() {
    m_trayIcon = new QSystemTrayIcon(this);

    // 图标从 exe 同目录下的 icon.png 加载（CMake 构建后自动复制）
    const QString iconPath = QCoreApplication::applicationDirPath() + "/icon.png";
    QIcon appIcon(iconPath);
    if (appIcon.isNull()) {
        appIcon = QIcon(":/icon.png");  // 回退：尝试 Qt 资源系统
    }
    if (appIcon.isNull()) {
        // 最终回退：生成占位图标（纯色方块）
        QPixmap pm(64, 64);
        pm.fill(QColor(20, 25, 50));
        appIcon = QIcon(pm);
    }
    m_trayIcon->setIcon(appIcon);
    qApp->setWindowIcon(appIcon);
    m_trayIcon->setToolTip("极光条 - 桌面音乐可视化");

    // RGB主题托盘菜单
    m_trayMenu = new QMenu(this);
    m_trayMenu->setStyleSheet(R"(
        QMenu {
            background: #10131e;
            border: 1px solid #203050;
            border-radius: 6px;
            padding: 4px;
        }
        QMenu::item {
            color: #b0c0e0;
            padding: 6px 24px;
            border-radius: 4px;
        }
        QMenu::item:selected {
            background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #1a3060, stop:1 #203870);
            color: #ffffff;
        }
        QMenu::separator {
            height: 1px;
            background: #202840;
            margin: 4px 8px;
        }
    )");

    m_settingsAction = m_trayMenu->addAction("设置");
    m_trayMenu->addSeparator();
    m_toggleAction = m_trayMenu->addAction("隐藏覆盖");
    m_trayMenu->addSeparator();
    QAction* aboutAction = m_trayMenu->addAction("关于");
    QAction* quitAction  = m_trayMenu->addAction("退出");

    m_trayIcon->setContextMenu(m_trayMenu);

    connect(m_settingsAction, &QAction::triggered, this, &MainWindow::openSettings);
    connect(m_toggleAction,  &QAction::triggered, this, &MainWindow::toggleOverlay);
    connect(quitAction,   &QAction::triggered, qApp, &QApplication::quit);
    connect(aboutAction,  &QAction::triggered, [this]() {
        m_trayIcon->showMessage("极光条", "v1.0 - 桌面音乐可视化\n按显存自动选择最佳显卡",
                                QSystemTrayIcon::Information, 2000);
    });

    connect(m_trayIcon, &QSystemTrayIcon::activated, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick) openSettings();
    });

    m_trayIcon->show();
}

void MainWindow::openSettings() {
    m_settingsPanel->refreshAll();
    // 安全：居中显示，避免面板部分在屏幕外被遮挡
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen) {
        QRect sg = screen->availableGeometry();
        m_settingsPanel->move(sg.center() - m_settingsPanel->rect().center());
    }
    m_settingsPanel->showNormal();
    m_settingsPanel->raise();
    m_settingsPanel->activateWindow();
}

void MainWindow::toggleOverlay() {
    static bool visible = true;
    visible = !visible;
    m_toggleAction->setText(visible ? "隐藏覆盖" : "显示覆盖");
    emit overlayToggled(visible);
}