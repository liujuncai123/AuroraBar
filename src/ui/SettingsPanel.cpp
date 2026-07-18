/**
 * @file SettingsPanel.cpp
 * @brief RGB位置连续取色 — 所有控件颜色由Y坐标决定
 * @date 2026-07-06
 */
#include "SettingsPanel.h"
#include "../params/ParamStore.h"
#include "../config/ConfigManager.h"
#include "../core/CommandTypes.h"
#include "../core/SPSCQueue.h"
#include "../logging/LoggerManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QScrollArea>
#include <QMessageBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QScreen>
#include <QColorDialog>
#include <QPainter>
#include <QGuiApplication>
#include <cmath>

extern SPSCQueue<RenderCommand, 64> g_renderQueue;
extern SPSCQueue<ControlCommand, 16> g_controlQueue;

// HSV→QColor  (hue 0-360, sat 0-255, val 0-255)
static QColor hsv(int h, int s, int v) {
    QColor c; c.setHsv(h % 360, std::clamp(s,0,255), std::clamp(v,0,255)); return c;
}
// Y位置→色相
QColor SettingsPanel::posColor(int y, int rangeH) const {
    double t = std::clamp(static_cast<double>(y) / std::max(1, rangeH), 0.0, 1.0);
    int hue = static_cast<int>(std::fmod(t * 360.0 + 210.0, 360.0));
    return hsv(hue, 255, 255);
}

// 生成描述/推荐标签（颜色由外层传入）
static QLabel* makeLabel(const QString& t, const QColor& c) {
    auto* l = new QLabel(t);
    l->setStyleSheet(QString("color:%1;font-size:11px;padding-left:100px;background:transparent;").arg(c.name()));
    l->setWordWrap(true); return l;
}

// ── 全局样式：所有控件背景透明/暗色，颜色由代码动态设置 ──
static const char* baseSS = R"(
    QWidget { background: transparent; border: none; }
    QTabWidget::pane { background: transparent; border: none; }
    QTabBar::tab { background: transparent; padding: 8px 14px; margin-right: 2px; font-size:13px; }
    QScrollBar:vertical { background: transparent; width: 6px; }
    QScrollBar::handle:vertical { background: rgba(255,255,255,0.1); border-radius: 3px; min-height:30px; }
    QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical { height:0; }
    QSlider::groove:horizontal { height: 6px; border-radius: 3px; }
    QSlider::handle:horizontal { width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; }
    QSlider::sub-page:horizontal { border-radius: 3px; }
    QSpinBox,QDoubleSpinBox { border-radius:4px; padding:4px 8px; min-width:70px; font-size:13px; }
    QComboBox { border-radius:4px; padding:5px 10px; min-width:100px; font-size:13px; }
    QComboBox::drop-down { border-radius:0 4px 4px 0; width:24px; }
    QComboBox QAbstractItemView { border-radius:4px; outline:none; }
    QComboBox QAbstractItemView::item { padding:5px 10px; min-height:24px; }
    QPushButton { border-radius:5px; padding:6px 16px; font-weight:bold; font-size:13px; }
    QLineEdit { border-radius:4px; padding:4px 8px; }
)";

SettingsPanel::SettingsPanel(QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool)
{
    setWindowTitle("极光条 - 设置");
    setMinimumSize(560, 500); resize(580, 540);
    setAttribute(Qt::WA_TranslucentBackground);
    setStyleSheet(baseSS);

    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(3,3,3,3);
    auto* mainW = new QWidget;
    auto* ml = new QVBoxLayout(mainW); ml->setContentsMargins(16,8,16,10); ml->setSpacing(8);

    // ── 标题栏（所有颜色由 posColor 驱动） ──
    {
        auto* tb = new QWidget; tb->setFixedHeight(36);
        auto* tl = new QHBoxLayout(tb); tl->setContentsMargins(0,0,4,0);
        auto* title = new QLabel("AuroraBar");
        QColor tc = posColor(0, height()); tc = hsv(tc.hue(), 200, 200);
        title->setStyleSheet(QString("font-size:16px;font-weight:bold;color:%1;background:transparent;").arg(tc.name()));
        tl->addWidget(title, 1);

        auto mkBtn = [&](const QString& t, int dy) {
            auto* b = new QPushButton(t); b->setFixedSize(100,36);
            int h = posColor(dy,height()).hue();
            b->setStyleSheet(QString("QPushButton{background:%1;color:%2;border:1px solid %3;border-radius:5px;font-size:16px;font-weight:bold;} QPushButton:hover{background:%4;}")
                .arg(hsv(h,50,40).name(), hsv(h,100,240).name(), hsv(h,100,100).name(), hsv(h,70,65).name()));
            return b;
        };
        auto* minBtn = mkBtn("最小化", 30);
        auto* closeBtn = mkBtn("关闭", 30);
        connect(minBtn, &QPushButton::clicked, this, &SettingsPanel::minimizeToTray);
        connect(closeBtn, &QPushButton::clicked, this, &QWidget::hide);
        tl->addWidget(minBtn); tl->addWidget(closeBtn);
        ml->addWidget(tb);
    }

    // ── 方案栏 ──
    {
        auto* r = new QHBoxLayout;
        QColor plc = posColor(50, height()); plc = hsv(plc.hue(), 180, 200);
        auto* pl = new QLabel("方案:");
        pl->setStyleSheet(QString("color:%1;font-weight:bold;font-size:13px;background:transparent;").arg(plc.name()));
        m_profileNameLabel = new QLabel(QString::fromStdString(ConfigManager::currentProfile()));
        m_profileNameLabel->setStyleSheet(QString("color:%1;font-weight:bold;background:transparent;").arg(plc.lighter(120).name()));

        r->addWidget(pl); r->addWidget(m_profileNameLabel, 1);
        auto mk = [&](const QString& t, int dy) {
            auto* b = new QPushButton(t); b->setFixedSize(40,24);
            QColor bc = posColor(dy, height()); bc = hsv(bc.hue(), 80, 50);
            b->setStyleSheet(QString("QPushButton{font-size:11px;padding:0 4px;background:%1;border:1px solid %2;border-radius:3px;color:%3;} QPushButton:hover{background:%4;}")
                .arg(bc.name(), hsv(bc.hue(),120,80).name(), hsv(bc.hue(),100,200).name(), hsv(bc.hue(),100,70).name()));
            return b;
        };
        auto *sav=mk("保存",55),*imp=mk("导入",55),*exp=mk("导出",55),
             *nw=mk("新建",55),*sw=mk("切换",55),*del=mk("删除",55);
        r->addWidget(sav); r->addWidget(imp); r->addWidget(exp);
        r->addWidget(nw); r->addWidget(sw); r->addWidget(del);
        ml->addLayout(r);

        connect(sav,&QPushButton::clicked,[this](){ConfigManager::SaveCurrent();updateProfileLabel();});
        connect(imp,&QPushButton::clicked,[this](){
            // 安全：parent=nullptr 隔离 SettingsPanel 样式表，防止级联导致对话框背景纯黑
            QString p = QFileDialog::getOpenFileName(nullptr, "导入", "", "JSON(*.json)");
            if(p.isEmpty()) return;
            bool ok;
            QString n = QInputDialog::getText(nullptr, "名称", "名称:", QLineEdit::Normal, "", &ok);
            if(!ok || n.isEmpty()) return;
            auto x = ConfigManager::ImportProfile(p.toStdString(), n.toStdString());
            if(x.isErr()) QMessageBox::warning(nullptr, "失败", QString::fromStdString(x.error().message));
            else {
                auto x2 = ConfigManager::SwitchProfile(n.toStdString());
                if(x2.isOk()) { refreshAll(); updateProfileLabel(); }
            }
        });
        connect(exp,&QPushButton::clicked,[this](){
            QString p = QFileDialog::getSaveFileName(nullptr, "导出", "aurorabar_config.json", "JSON(*.json)");
            if(p.isEmpty()) return;
            ConfigManager::ExportProfile(ConfigManager::currentProfile(), p.toStdString());
        });
        // 新建方案
        connect(nw,&QPushButton::clicked,[this](){
            bool ok;
            QString n = QInputDialog::getText(nullptr, "新建方案", "方案名称:", QLineEdit::Normal, "", &ok);
            if(!ok || n.isEmpty()) return;
            auto x = ConfigManager::CreateProfile(n.toStdString());
            if(x.isErr()) QMessageBox::warning(nullptr, "失败", QString::fromStdString(x.error().message));
            else { refreshAll(); updateProfileLabel(); }
        });
        // 切换方案
        connect(sw,&QPushButton::clicked,[this](){
            auto ps = ConfigManager::ListProfiles();
            if(ps.empty()) { QMessageBox::information(nullptr, "提示", "暂无方案，请先新建或导入。"); return; }
            QStringList it;
            for(auto& x : ps) it << QString::fromStdString(x);
            bool ok;
            QString c = QInputDialog::getItem(nullptr, "切换", "选择:", it, 0, false, &ok);
            if(!ok || c.isEmpty()) return;
            auto x = ConfigManager::SwitchProfile(c.toStdString());
            if(x.isOk()) { refreshAll(); updateProfileLabel(); }
        });
        // 删除方案
        connect(del,&QPushButton::clicked,[this](){
            auto ps = ConfigManager::ListProfiles();
            if(ps.size() <= 1) { QMessageBox::warning(nullptr, "提示", "至少保留一个方案。"); return; }
            QStringList it;
            for(auto& x : ps) it << QString::fromStdString(x);
            bool ok;
            QString c = QInputDialog::getItem(nullptr, "删除", "选择要删除的方案:", it, 0, false, &ok);
            if(!ok || c.isEmpty()) return;
            if(c.toStdString() == ConfigManager::currentProfile()) {
                QMessageBox::warning(nullptr, "提示", "不能删除当前正在使用的方案，请先切换到其他方案。");
                return;
            }
            auto x = ConfigManager::DeleteProfile(c.toStdString());
            if(x.isErr()) QMessageBox::warning(nullptr, "失败", QString::fromStdString(x.error().message));
            else { refreshAll(); updateProfileLabel(); }
        });
    }

    auto* tabs = new QTabWidget(this);
    tabs->setStyleSheet("QTabBar::tab { color: #8090a0; } QTabBar::tab:selected { color: #d0e0f0; font-weight:bold; }");
    ml->addWidget(tabs);

    buildBorderTab(tabs); buildVisualPerfTab(tabs); buildPhysicsDormantTab(tabs);
    buildModeTab(tabs); buildCycleTab(tabs); buildBounceBallTab(tabs); buildConcertoTab(tabs);

    auto* br = new QHBoxLayout; br->addStretch();
    auto* rst = new QPushButton("重置"); rst->setFixedHeight(32);
    rst->setStyleSheet("QPushButton{background:#181228;border:1px solid#281848;color:#9080b0;padding:4px 16px;border-radius:4px;} QPushButton:hover{background:#281840;}");
    connect(rst,&QPushButton::clicked,[this](){
        ParamStore::Instance().ResetAll();
        // 不再自动保存——用户看到"未保存"提示后自行决定
        refreshAll();
        updateProfileLabel();
    });
    br->addWidget(rst); ml->addLayout(br);
    outer->addWidget(mainW);

    auto& ps = ParamStore::Instance();
    auto sr = [this](const std::string&,double){QMetaObject::invokeMethod(this,"onRemoteParamChanged",Qt::QueuedConnection);};
    m_subIds.push_back(ps.Subscribe("concerto.columnsPerSeg",sr));
    m_subIds.push_back(ps.Subscribe("concerto.maxHeight",sr));
    m_subIds.push_back(ps.Subscribe("cycle.sizeMin",sr));
    m_subIds.push_back(ps.Subscribe("cycle.sizeMax",sr));
    updateProfileLabel();  // 初始状态：显示方案名，无"未保存"标记
}
SettingsPanel::~SettingsPanel(){auto&ps=ParamStore::Instance();for(int id:m_subIds)ps.Unsubscribe(id);}
void SettingsPanel::onRemoteParamChanged(){refreshAll();}
void SettingsPanel::refreshAll(){for(auto&[k,r]:m_rows)syncWidgetFromStore(k,r);updateProfileLabel();}

void SettingsPanel::updateProfileLabel() {
    auto& ps = ParamStore::Instance();
    QString name = QString::fromStdString(ConfigManager::currentProfile());
    if (ps.isDirty()) {
        m_profileNameLabel->setText(name + "  (未保存)");
        // 警告色：橙色，区别于正常蓝紫色
        m_profileNameLabel->setStyleSheet(
            QString("color:#ffb040;font-weight:bold;font-size:13px;background:transparent;"));
    } else {
        m_profileNameLabel->setText(name);
        QColor plc = posColor(50, height());
        m_profileNameLabel->setStyleSheet(
            QString("color:%1;font-weight:bold;font-size:13px;background:transparent;")
                .arg(hsv(plc.hue(), 180, 220).lighter(120).name()));
    }
}
void SettingsPanel::changeEvent(QEvent* e){
    if(e->type()==QEvent::WindowStateChange&&isMinimized()){emit minimizeToTray();e->ignore();return;}
    QWidget::changeEvent(e);
}
void SettingsPanel::closeEvent(QCloseEvent*e){
    hide();  // 立即隐藏，用户无感知
    e->ignore();
}
void SettingsPanel::mousePressEvent(QMouseEvent*e){if(e->button()==Qt::LeftButton){m_dragPos=e->globalPosition().toPoint()-frameGeometry().topLeft();e->accept();}}
void SettingsPanel::mouseMoveEvent(QMouseEvent*e){if(e->buttons()&Qt::LeftButton){move(e->globalPosition().toPoint()-m_dragPos);e->accept();}}

void SettingsPanel::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    int w=width(), h=height();
    QLinearGradient g(0,0,0,h);
    g.setColorAt(0.0, hsv(posColor(0,h).hue(),150,55));
    g.setColorAt(0.5, hsv(posColor(h/2,h).hue(),130,48));
    g.setColorAt(1.0, hsv(posColor(h,h).hue(),150,55));
    p.fillRect(rect(), g);
    QColor bd=posColor(h/2,h); bd.setHsv(bd.hue(),180,100);
    p.setPen(QPen(bd,2)); p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(1,1,w-2,h-2),10,10);
}

static QSize getScreenSize(){auto*s=QGuiApplication::primaryScreen();return s?s->size():QSize(1920,1080);}

// ── 为单个 row 应用 posColor ──
static void applyRowColor(QWidget* row, QFormLayout* form, int rowIdx) {
    if (!row || !form) return;
    // 用 row 的 mapTo 来算出全局 Y（近似：form 内部累加）
    QColor c = hsv((210 + rowIdx * 15) % 360, 255, 255);  // 每行色相偏移15°
    // 找 QLabel
    auto* lo = row->layout();
    if (!lo) return;
    for (int i=0;i<lo->count();++i) {
        auto* w = lo->itemAt(i)->widget();
        if (!w) continue;
        if (auto* lb = qobject_cast<QLabel*>(w)) {
            lb->setStyleSheet(QString("color:%1;font-size:13px;background:transparent;").arg(hsv(c.hue(),180,220).name()));
            continue;
        }
        QColor bg = hsv(c.hue(), 60, 40);
        QColor fg = hsv(c.hue(), 100, 200);
        QColor bd = hsv(c.hue(), 120, 70);
        QColor hov = hsv(c.hue(), 100, 60);
        if (qobject_cast<QSlider*>(w)) {
            w->setStyleSheet(QString(
                "QSlider::groove:horizontal{background:%1;} QSlider::handle:horizontal{background:qlineargradient(x1:0,y1:0,x2:0,y2:1,stop:0 %2,stop:1 %3);border:1px solid %4;}"
                "QSlider::sub-page:horizontal{background:qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 %5,stop:1 %6);}"
            ).arg(hsv(c.hue(),50,30).name(), hsv(c.hue(),200,220).name(), hsv(c.hue(),150,140).name(), hsv(c.hue(),180,180).name(), hsv(c.hue(),80,40).name(), hsv(c.hue(),120,80).name()));
        } else if (auto* sb = qobject_cast<QSpinBox*>(w)) {
            sb->setStyleSheet(QString("QSpinBox{background:%1;color:%2;border:1px solid %3;} QSpinBox:focus{border-color:%4;}")
                .arg(bg.name(), fg.name(), bd.name(), hsv(c.hue(),200,150).name()));
        } else if (auto* ds = qobject_cast<QDoubleSpinBox*>(w)) {
            ds->setStyleSheet(QString("QDoubleSpinBox{background:%1;color:%2;border:1px solid %3;} QDoubleSpinBox:focus{border-color:%4;}")
                .arg(bg.name(), fg.name(), bd.name(), hsv(c.hue(),200,150).name()));
        } else if (auto* cb = qobject_cast<QComboBox*>(w)) {
            cb->setStyleSheet(QString(
                "QComboBox{background:%1;color:%2;border:1px solid %3;} QComboBox:hover{border-color:%4;}"
                "QComboBox::drop-down{background:%5;border-left:1px solid %3;}"
                "QComboBox QAbstractItemView{background:%1;color:%2;border:1px solid %3;selection-background-color:%6;}"
            ).arg(bg.name(), fg.name(), bd.name(), hsv(c.hue(),200,150).name(), hsv(c.hue(),60,50).name(), hsv(c.hue(),100,60).name()));
        } else if (auto* pb = qobject_cast<QPushButton*>(w)) {
            pb->setStyleSheet(QString("QPushButton{background:%1;color:%2;border:1px solid %3;} QPushButton:hover{background:%4;}")
                .arg(bg.name(), fg.name(), bd.name(), hov.name()));
        }
    }
}

// ── Tab builders: 每个 row 调用 applyRowColor ──
static int g_rowIdx = 0;
static void addRow(QFormLayout* f, QWidget* row) {
    f->addRow(row);
    applyRowColor(row, f, g_rowIdx++);
}

void SettingsPanel::buildBorderTab(QTabWidget* tabs){
    auto*sc=new QScrollArea;sc->setWidgetResizable(true);sc->setFrameShape(QFrame::NoFrame);
    auto*pg=new QWidget;auto*f=new QFormLayout(pg);f->setSpacing(6);g_rowIdx=0;
    QSize scr=getScreenSize();
    // 边框最大值：上下 = 屏幕高/2，左右 = 屏幕宽/2（各方向独立）
    int maxBW_TB = std::max(20, scr.height() / 2);
    int maxBW_LR = std::max(20, scr.width()  / 2);
    addRow(f,createIntRow({"borderWidth.bottom","下边框",ParamDef::Type::Double,1,(double)maxBW_TB,1,80}));
    addRow(f,makeLabel("屏幕下方边框宽度。",hsv(0,0,100)));
    addRow(f,createIntRow({"borderWidth.top","上边框",ParamDef::Type::Double,1,(double)maxBW_TB,1,80}));
    addRow(f,makeLabel(QString("上下之和 <= %1px").arg(scr.height()),hsv(0,0,100)));
    addRow(f,createIntRow({"borderWidth.left","左边框",ParamDef::Type::Double,1,(double)maxBW_LR,1,80}));
    addRow(f,makeLabel(QString("左右之和 <= %1px").arg(scr.width()),hsv(0,0,100)));
    addRow(f,createIntRow({"borderWidth.right","右边框",ParamDef::Type::Double,1,(double)maxBW_LR,1,80}));
    addRow(f,makeLabel("推荐 60~120px",hsv(140,100,140)));
    addRow(f,createIntRow({"cornerTransition","转角平滑",ParamDef::Type::Double,0,100,1,40}));
    addRow(f,makeLabel("推荐 30~60px",hsv(140,100,140)));
    addRow(f,createIntRow({"border.innerGlowWidth","内发光",ParamDef::Type::Int,0,8,1,2}));
    addRow(f,makeLabel("0=关闭，推荐 1~3px",hsv(140,100,140)));
    addRow(f,createPercentRow({"border.pulseAmount","脉动幅度",ParamDef::Type::Double,0.0,0.1,0.005,0.03}));
    addRow(f,makeLabel("推荐 2%~5%",hsv(140,100,140)));
    sc->setWidget(pg);tabs->addTab(sc,"边框");
}
void SettingsPanel::buildVisualPerfTab(QTabWidget*tabs){
    auto*sc=new QScrollArea;sc->setWidgetResizable(true);sc->setFrameShape(QFrame::NoFrame);
    auto*pg=new QWidget;auto*f=new QFormLayout(pg);f->setSpacing(6);g_rowIdx=0;
    {
        // 使用 ParamStore 中实际注册的 GPU 选项（动态检测的显卡列表）
        auto& ps = ParamStore::Instance();
        const ParamDef* gpuDef = ps.GetParamDef("gpu.selectedIndex");
        if (gpuDef) {
            addRow(f, createEnumRow(*gpuDef));
        } else {
            addRow(f, createEnumRow({"gpu.selectedIndex","显卡选择",ParamDef::Type::Enum,0,10,1,0,{"自动"}}));
        }
    }
    addRow(f,makeLabel("按显存自动选最佳，修改后需重启生效",hsv(140,100,140)));
    addRow(f,createIntRow({"targetFps","目标帧率",ParamDef::Type::Int,30,360,30,60}));
    addRow(f,makeLabel("推荐 60",hsv(140,100,140)));
    addRow(f,createEnumRow({"vsync","垂直同步",ParamDef::Type::Enum,0,1,1,0,{"关闭","开启"}}));
    addRow(f,makeLabel("推荐开启",hsv(140,100,140)));
    sc->setWidget(pg);tabs->addTab(sc,"视觉");
}
void SettingsPanel::buildPhysicsDormantTab(QTabWidget*tabs){
    auto*sc=new QScrollArea;sc->setWidgetResizable(true);sc->setFrameShape(QFrame::NoFrame);
    auto*pg=new QWidget;auto*f=new QFormLayout(pg);f->setSpacing(6);g_rowIdx=0;
    addRow(f,createDoubleRow({"physics.mass","质量",ParamDef::Type::Double,0.1,2.0,0.1,0.6}));
    addRow(f,makeLabel("推荐 0.4~0.8",hsv(140,100,140)));
    addRow(f,createDoubleRow({"physics.stiffness","刚度",ParamDef::Type::Double,0.1,2.0,0.1,0.4}));
    addRow(f,makeLabel("推荐 0.3~0.5",hsv(140,100,140)));
    addRow(f,createPercentRow({"physics.damping","阻尼",ParamDef::Type::Double,0.1,0.99,0.01,0.82}));
    addRow(f,makeLabel("推荐 70%~90%",hsv(140,100,140)));
    addRow(f,createPercentRow({"physics.nonlinearity","非线性",ParamDef::Type::Double,0.0,1.0,0.05,0.3}));
    addRow(f,makeLabel("推荐 20%~50%",hsv(140,100,140)));
    addRow(f,createDoubleRow({"transition.duration","过渡时长",ParamDef::Type::Double,0.1,3.0,0.1,0.5}));
    addRow(f,makeLabel("推荐 0.3~0.8s",hsv(140,100,140)));
    auto*sep=new QLabel("-- 休眠 --");sep->setStyleSheet("color:#8090b0;font-weight:bold;padding-top:8px;");addRow(f,sep);
    addRow(f,createEnumRow({"dormantBehavior","无音频时",ParamDef::Type::Enum,0,1,1,0,{"保持呼吸","渐隐"}}));
    addRow(f,makeLabel("推荐: 保持呼吸",hsv(140,100,140)));
    addRow(f,createPercentRow({"dormantThreshold","休眠阈值",ParamDef::Type::Double,0.0,1.0,0.01,0.01}));
    addRow(f,makeLabel("推荐 1%~5%",hsv(140,100,140)));
    addRow(f,createDoubleRow({"dormantDelay","休眠延迟",ParamDef::Type::Double,1.0,10.0,1.0,3.0}));
    addRow(f,makeLabel("推荐 2~5s",hsv(140,100,140)));
    sc->setWidget(pg);tabs->addTab(sc,"物理");
}
void SettingsPanel::buildModeTab(QTabWidget*tabs){
    auto*sc=new QScrollArea;sc->setWidgetResizable(true);sc->setFrameShape(QFrame::NoFrame);
    auto*pg=new QWidget;auto*f=new QFormLayout(pg);f->setSpacing(6);g_rowIdx=0;
    addRow(f,createEnumRow({"mode","可视化模式",ParamDef::Type::Enum,0,2,1,0,{"循环粒子","弹球","协奏"}}));
    addRow(f,makeLabel("推荐: 协奏",hsv(140,100,140)));
    auto&ps=ParamStore::Instance();
    m_subIds.push_back(ps.Subscribe("mode",[](const std::string&,double v){
        ControlCommand c;
        c.type = ControlCommand::Type::SetMode;
        c.value = v;
        if (!g_controlQueue.tryPush(c))
            AURORA_WARN("SettingsPanel", "SetMode command lost (control queue full)");
    }));
    sc->setWidget(pg);tabs->addTab(sc,"模式");
}
void SettingsPanel::buildCycleTab(QTabWidget*tabs){
    auto*sc=new QScrollArea;sc->setWidgetResizable(true);sc->setFrameShape(QFrame::NoFrame);
    auto*pg=new QWidget;auto*f=new QFormLayout(pg);f->setSpacing(6);g_rowIdx=0;
    // ── 调色板（粒子模式专属） ──
    addRow(f,createEnumRow({"colorScheme","调色板",ParamDef::Type::Enum,0,4,1,0,{"极光青","熔岩橙","星云紫","音频驱动","手动"}}));
    addRow(f,makeLabel("手动模式用下方取色器。",hsv(0,0,100)));
    addRow(f,makeLabel("推荐: 极光青 或 音频驱动",hsv(140,100,140)));
    // ── 自选颜色（粒子模式专属） ──
    {
        auto*cr=new QWidget;auto*cl=new QHBoxLayout(cr);cl->setContentsMargins(0,0,0,0);
        auto*clbl=new QLabel("自选颜色");clbl->setMinimumWidth(100);
        auto*cbtn=new QPushButton("取色");cbtn->setStyleSheet("QPushButton{background:#00b0e0;color:#fff;border-radius:4px;padding:4px 12px;}");
        cl->addWidget(clbl);cl->addWidget(cbtn,1); addRow(f,cr); m_customColorBtn=cbtn;
        connect(cbtn,&QPushButton::clicked,[this,cbtn](){
            auto&ps=ParamStore::Instance();
            double r = ps.GetDouble("customColor.r");
            double g = ps.GetDouble("customColor.g");
            double b = ps.GetDouble("customColor.b");
            // 安全：防止 NaN 或非法值导致黑色
            if (!std::isfinite(r) || r < 0.0 || r > 1.0) r = 0.0;
            if (!std::isfinite(g) || g < 0.0 || g > 1.0) g = 1.0;
            if (!std::isfinite(b) || b < 0.0 || b > 1.0) b = 0.8;
            QColor ic(static_cast<int>(r*255), static_cast<int>(g*255), static_cast<int>(b*255));
            // 安全：parent=nullptr 隔离 SettingsPanel 样式表，防止级联导致对话框背景纯黑
            QColor ch = QColorDialog::getColor(ic, nullptr, "选择RGB颜色");
            if(ch.isValid()){
                ps.SetDouble("customColor.r", ch.redF());
                ps.SetDouble("customColor.g", ch.greenF());
                ps.SetDouble("customColor.b", ch.blueF());
                cbtn->setStyleSheet(QString("QPushButton{background:%1;color:#fff;border-radius:4px;padding:4px 12px;}").arg(ch.name()));
            }
        });
        auto&ps2=ParamStore::Instance();
        double r2 = ps2.GetDouble("customColor.r");
        double g2 = ps2.GetDouble("customColor.g");
        double b2 = ps2.GetDouble("customColor.b");
        if (!std::isfinite(r2) || r2 < 0.0 || r2 > 1.0) r2 = 0.0;
        if (!std::isfinite(g2) || g2 < 0.0 || g2 > 1.0) g2 = 1.0;
        if (!std::isfinite(b2) || b2 < 0.0 || b2 > 1.0) b2 = 0.8;
        QColor ic2(static_cast<int>(r2*255), static_cast<int>(g2*255), static_cast<int>(b2*255));
        cbtn->setStyleSheet(QString("QPushButton{background:%1;color:#fff;border-radius:4px;padding:4px 12px;}").arg(ic2.name()));
    }
    addRow(f,createDoubleRow({"cycle.particleLife","粒子寿命",ParamDef::Type::Double,0.5,5.0,0.1,2.0}));
    addRow(f,makeLabel("推荐 1.5~3.0s",hsv(140,100,140)));
    addRow(f,createIntRow({"cycle.maxDistance","行进距离",ParamDef::Type::Int,100,2000,50,500}));
    addRow(f,makeLabel("推荐 300~800px",hsv(140,100,140)));
    addRow(f,createIntRow({"cycle.sizeMin","最小大小",ParamDef::Type::Int,1,50,1,3}));
    addRow(f,makeLabel("推荐 2~5px",hsv(140,100,140)));
    addRow(f,createIntRow({"cycle.sizeMax","最大大小",ParamDef::Type::Int,3,100,1,8}));
    addRow(f,makeLabel("推荐 5~15px",hsv(140,100,140)));
    addRow(f,createIntRow({"cycle.emitMultiplier","生成率",ParamDef::Type::Int,100,10000,50,500}));
    addRow(f,makeLabel("推荐 500~5000",hsv(140,100,140)));
    // 安全：上限与代码硬限制一致（ParticleEffect::kMaxCapacity=10000），防止UI设置超出运行时限制
    addRow(f,createIntRow({"particleCount","粒子池",ParamDef::Type::Int,500,10000,100,2000}));
    addRow(f,makeLabel("推荐 3000~10000",hsv(140,100,140)));
    sc->setWidget(pg);tabs->addTab(sc,"粒子");
}
void SettingsPanel::buildBounceBallTab(QTabWidget*tabs){
    auto*sc=new QScrollArea;sc->setWidgetResizable(true);sc->setFrameShape(QFrame::NoFrame);
    auto*pg=new QWidget;auto*f=new QFormLayout(pg);f->setSpacing(6);g_rowIdx=0;
    addRow(f,createEnumRow({"bb.dualMode","球数",ParamDef::Type::Enum,0,1,1,0,{"单球","双球"}}));
    addRow(f,makeLabel("双球在对面。",hsv(0,0,100)));
    addRow(f,createDoubleRow({"bb.kTangentSpeed","切向速度",ParamDef::Type::Double,0.01,5.0,0.01,0.15}));
    addRow(f,makeLabel("推荐 0.1~1.0",hsv(140,100,140)));
    addRow(f,createDoubleRow({"bb.kFollowSpeed","法向跟随",ParamDef::Type::Double,1.0,30.0,1.0,25.0}));
    addRow(f,makeLabel("推荐 15~30",hsv(140,100,140)));
    addRow(f,createDoubleRow({"bb.rmsSensitivity","RMS灵敏度",ParamDef::Type::Double,1.0,30.0,1.0,8.0}));
    addRow(f,makeLabel("越小越平滑，推荐 5~15",hsv(140,100,140)));
    addRow(f,createIntRow({"bb.trailLength","拖尾点数",ParamDef::Type::Int,50,5000,50,300}));
    addRow(f,makeLabel("推荐 200~2000",hsv(140,100,140)));
    addRow(f,createDoubleRow({"bb.trailMaxAge","拖尾寿命",ParamDef::Type::Double,1.0,15.0,0.5,5.0}));
    addRow(f,makeLabel("推荐 3~8s",hsv(140,100,140)));
    sc->setWidget(pg);tabs->addTab(sc,"弹球");
}
void SettingsPanel::buildConcertoTab(QTabWidget*tabs){
    auto*sc=new QScrollArea;sc->setWidgetResizable(true);sc->setFrameShape(QFrame::NoFrame);
    auto*pg=new QWidget;auto*f=new QFormLayout(pg);f->setSpacing(6);g_rowIdx=0;
    // ── 每边分段（协奏模式专属） ──
    addRow(f,createIntRow({"segmentsPerEdge","每边分段",ParamDef::Type::Int,3,8,1,4}));
    addRow(f,makeLabel("推荐 4~8，细分越多律动越连贯",hsv(140,100,140)));
    addRow(f,createEnumRow({"subMode","视觉效果",ParamDef::Type::Enum,0,7,1,0,{"晶柱升级","频谱柱","流体波","粒子流","网格光带","激光线扫","极光丝带","脉冲环"}}));
    addRow(f,makeLabel("0=晶柱 1=频谱柱 2=流体波 3=粒子流 4=网格光带 5=激光线扫 6=极光丝带 7=脉冲环",hsv(140,100,140)));
    // subMode 变更 → LogicThread → GlobalParam → ConcertoRenderer::switchSubMode
    auto& ps4 = ParamStore::Instance();
    m_subIds.push_back(ps4.Subscribe("subMode",[](const std::string&,double v){
        ControlCommand c;
        c.type = ControlCommand::Type::SetSubMode;
        c.value = v;
        if (!g_controlQueue.tryPush(c))
            AURORA_WARN("SettingsPanel", "SetSubMode command lost (control queue full)");
    }));
    addRow(f,createIntRow({"concerto.columnsPerSeg","每段柱数",ParamDef::Type::Int,1,30,1,10}));
    addRow(f,makeLabel("推荐 5~20",hsv(140,100,140)));
    addRow(f,createIntRow({"concerto.maxTotalColumns","总数上限",ParamDef::Type::Int,300,8000,100,5000}));
    addRow(f,makeLabel("推荐 2000~8000",hsv(140,100,140)));
    addRow(f,makeLabel("修改上限或边框时自动钳制。",hsv(0,0,100)));
    addRow(f,createDoubleRow({"concerto.followSpeed","跟随速度",ParamDef::Type::Double,3.0,30.0,1.0,12.0}));
    addRow(f,makeLabel("推荐 8~20",hsv(140,100,140)));
    addRow(f,createDoubleRow({"concerto.flowSpeed","流动速度",ParamDef::Type::Double,0.05,2.0,0.05,0.4}));
    addRow(f,makeLabel("晶柱模式亮带流速，推荐 0.2~0.8",hsv(140,100,140)));
    addRow(f,createPercentRow({"concerto.alphaBase","基础透明度",ParamDef::Type::Double,0.1,0.8,0.05,0.4}));
    addRow(f,makeLabel("推荐 30%~50%",hsv(140,100,140)));
    addRow(f,createPercentRow({"concerto.threshold","显示阈值",ParamDef::Type::Double,0.001,0.05,0.001,0.002}));
    addRow(f,makeLabel("推荐 0.1%~0.5%",hsv(140,100,140)));
    auto*sn=new QLabel("四边开关");sn->setStyleSheet("color:#8090b0;font-weight:bold;padding-top:6px;");addRow(f,sn);
    addRow(f,createEnumRow({"concerto.showBottom","下边",ParamDef::Type::Enum,0,1,1,1,{"关闭","开启"}}));
    addRow(f,createEnumRow({"concerto.showRight","右边",ParamDef::Type::Enum,0,1,1,1,{"关闭","开启"}}));
    addRow(f,createEnumRow({"concerto.showTop","上边",ParamDef::Type::Enum,0,1,1,1,{"关闭","开启"}}));
    addRow(f,createEnumRow({"concerto.showLeft","左边",ParamDef::Type::Enum,0,1,1,1,{"关闭","开启"}}));
    auto*mc=new QLabel("音乐驱动颜色");mc->setStyleSheet("color:#8090b0;font-weight:bold;padding-top:6px;");addRow(f,mc);
    addRow(f,createEnumRow({"concerto.musicColor","开关",ParamDef::Type::Enum,0,1,1,0,{"关闭","开启"}}));
    addRow(f,makeLabel("启用后颜色随 FFT 主频变化（低频红/中频青/高频紫）",hsv(140,100,140)));
    sc->setWidget(pg);tabs->addTab(sc,"协奏");
}

// ── Row creators ──
QWidget* SettingsPanel::createIntRow(const ParamDef& def){
    auto*row=new QWidget;auto*lay=new QHBoxLayout(row);lay->setContentsMargins(0,0,0,0);
    auto*label=new QLabel(QString::fromStdString(def.displayName));label->setMinimumWidth(100);
    int mn=def.minVal,mx=def.maxVal,dv=def.defaultVal;
    auto*slider=new QSlider(Qt::Horizontal);slider->setRange(mn,mx);slider->setValue(dv);
    auto*spin=new QSpinBox;spin->setRange(mn,mx);spin->setValue(dv);
    connect(slider,&QSlider::valueChanged,spin,&QSpinBox::setValue);
    connect(spin,QOverload<int>::of(&QSpinBox::valueChanged),slider,&QSlider::setValue);
    connect(spin,QOverload<int>::of(&QSpinBox::valueChanged),[key=def.key,isDbl=(def.type==ParamDef::Type::Double)](int v){if(isDbl)ParamStore::Instance().SetDouble(key,(double)v);else ParamStore::Instance().SetInt(key,v);});
    lay->addWidget(label);lay->addWidget(slider,1);lay->addWidget(spin);
    m_rows[def.key]=row;return row;
}
QWidget* SettingsPanel::createDoubleRow(const ParamDef& def){
    auto*row=new QWidget;auto*lay=new QHBoxLayout(row);lay->setContentsMargins(0,0,0,0);
    auto*label=new QLabel(QString::fromStdString(def.displayName));label->setMinimumWidth(100);
    // 安全：防止浮点值放大100倍后溢出int范围，同时限制滑块步数在合理范围
    double clampedMin = std::max(def.minVal, -10000.0);
    double clampedMax = std::min(def.maxVal, 10000.0);
    int mn = static_cast<int>(std::clamp(clampedMin * 100.0, -1000000.0, 1000000.0));
    int mx = static_cast<int>(std::clamp(clampedMax * 100.0, -1000000.0, 1000000.0));
    int dv = static_cast<int>(std::clamp(def.defaultVal * 100.0, -1000000.0, 1000000.0));
    auto*slider=new QSlider(Qt::Horizontal);slider->setRange(mn,mx);slider->setValue(dv);
    auto*spin=new QDoubleSpinBox;spin->setRange(def.minVal,def.maxVal);spin->setSingleStep(def.step);spin->setDecimals(2);spin->setValue(def.defaultVal);
    connect(slider,&QSlider::valueChanged,[spin](int v){spin->setValue(v/100.0);});
    connect(spin,QOverload<double>::of(&QDoubleSpinBox::valueChanged),[slider](double v){slider->setValue(v*100);});
    connect(spin,QOverload<double>::of(&QDoubleSpinBox::valueChanged),[key=def.key](double v){ParamStore::Instance().SetDouble(key,v);});
    lay->addWidget(label);lay->addWidget(slider,1);lay->addWidget(spin);
    m_rows[def.key]=row;return row;
}
QWidget* SettingsPanel::createPercentRow(const ParamDef& def){
    auto*row=new QWidget;auto*lay=new QHBoxLayout(row);lay->setContentsMargins(0,0,0,0);
    auto*label=new QLabel(QString::fromStdString(def.displayName));label->setMinimumWidth(100);
    // 安全：防止浮点值放大100倍后溢出int范围
    int mn = static_cast<int>(std::clamp(def.minVal * 100.0, -1000000.0, 1000000.0));
    int mx = static_cast<int>(std::clamp(def.maxVal * 100.0, -1000000.0, 1000000.0));
    int dv = static_cast<int>(std::clamp(def.defaultVal * 100.0, -1000000.0, 1000000.0));
    auto*slider=new QSlider(Qt::Horizontal);slider->setRange(mn,mx);slider->setValue(dv);
    auto*spin=new QDoubleSpinBox;spin->setRange(def.minVal*100.0,def.maxVal*100.0);spin->setSingleStep(def.step*100.0);spin->setDecimals(1);spin->setSuffix("%");spin->setValue(def.defaultVal*100.0);
    connect(slider,&QSlider::valueChanged,[spin](int v){spin->setValue((double)v);});
    connect(spin,QOverload<double>::of(&QDoubleSpinBox::valueChanged),[slider](double v){slider->setValue((int)v);});
    connect(spin,QOverload<double>::of(&QDoubleSpinBox::valueChanged),[key=def.key](double v){ParamStore::Instance().SetDouble(key,v/100.0);});
    lay->addWidget(label);lay->addWidget(slider,1);lay->addWidget(spin);
    m_rows[def.key]=row;return row;
}
QWidget* SettingsPanel::createEnumRow(const ParamDef& def){
    auto*row=new QWidget;auto*lay=new QHBoxLayout(row);lay->setContentsMargins(0,0,0,0);
    auto*label=new QLabel(QString::fromStdString(def.displayName));label->setMinimumWidth(100);
    auto*combo=new QComboBox;for(auto&opt:def.enumOptions)combo->addItem(QString::fromStdString(opt));
    combo->setCurrentIndex(ParamStore::Instance().GetInt(def.key));
    connect(combo,QOverload<int>::of(&QComboBox::currentIndexChanged),[key=def.key](int idx){ParamStore::Instance().SetDouble(key,(double)idx);});
    lay->addWidget(label);lay->addWidget(combo,1);
    m_rows[def.key]=row;return row;
}
void SettingsPanel::syncWidgetFromStore(const std::string& key,QWidget* row){
    auto&ps=ParamStore::Instance();auto*lo=row->layout();if(!lo)return;
    for(int i=0;i<lo->count();++i){auto*it=lo->itemAt(i);if(!it||!it->widget())continue;
        if(auto*s=qobject_cast<QSpinBox*>(it->widget())){s->setValue(ps.GetInt(key));return;}
        if(auto*d=qobject_cast<QDoubleSpinBox*>(it->widget())){d->setValue(d->suffix().contains('%')?ps.GetDouble(key)*100.0:ps.GetDouble(key));return;}
        if(auto*c=qobject_cast<QComboBox*>(it->widget())){
            // 安全：同步时阻断信号，防止 setCurrentIndex 触发 currentIndexChanged
            // 导致不必要的 SetDouble 调用（值未变时不通知订阅者，但避免副作用）
            c->blockSignals(true);
            c->setCurrentIndex(ps.GetInt(key));
            c->blockSignals(false);
            return;
        }
    }
}