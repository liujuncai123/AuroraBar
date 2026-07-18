/**
 * @file mainwindow.h
 * @brief 主窗口——系统托盘管理 + 边框叠加层控制 + 设置面板入口
 * @date 2026-07-06
 */

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>

class SettingsPanel;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    /// @brief 切换边框叠加层可见性
    void toggleOverlay();

signals:
    void overlayToggled(bool visible);

private:
    void setupTrayIcon();
    void openSettings();

    Ui::MainWindow    *ui;
    QSystemTrayIcon   *m_trayIcon     = nullptr;
    QMenu             *m_trayMenu     = nullptr;
    QAction           *m_toggleAction = nullptr;
    QAction           *m_settingsAction = nullptr;

    SettingsPanel*     m_settingsPanel = nullptr;
};
#endif // MAINWINDOW_H
