/**
 * @file SettingsPanel.h
 * @brief 设置面板 — RGB连续取色主题
 * @date 2026-07-06
 */

#pragma once

#include <QWidget>
#include <QTabWidget>
#include <QSlider>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QFormLayout>
#include <QLabel>
#include <QEvent>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QPoint>
#include "../params/ParamDef.h"
#include <unordered_map>
#include <vector>

class SettingsPanel : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPanel(QWidget* parent = nullptr);
    ~SettingsPanel() override;

    void refreshAll();

signals:
    void minimizeToTray();

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onRemoteParamChanged();

private:
    void buildBorderTab(QTabWidget* tabs);
    void buildVisualPerfTab(QTabWidget* tabs);
    void buildPhysicsDormantTab(QTabWidget* tabs);
    void buildModeTab(QTabWidget* tabs);
    void buildCycleTab(QTabWidget* tabs);
    void buildBounceBallTab(QTabWidget* tabs);
    void buildConcertoTab(QTabWidget* tabs);

    QWidget* createIntRow(const ParamDef& def);
    QWidget* createDoubleRow(const ParamDef& def);
    QWidget* createPercentRow(const ParamDef& def);
    QWidget* createEnumRow(const ParamDef& def);
    void syncWidgetFromStore(const std::string& key, QWidget* row);

    QColor posColor(int y, int rangeH) const;

    QLabel* m_profileNameLabel = nullptr;
    QPushButton* m_customColorBtn = nullptr;
    QPoint m_dragPos;
    std::unordered_map<std::string, QWidget*> m_rows;
    std::vector<int> m_subIds;

    /// @brief 根据 ParamStore 脏标记更新方案标签（显示/隐藏"未保存"提示）
    void updateProfileLabel();
};
