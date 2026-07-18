/**
 * @file BuiltInDefaults.cpp
 * @brief 注册所有默认参数
 * @date 2026-07-06
 */

#include "BuiltInDefaults.h"
#include "ParamStore.h"
#include "../ui/GpuDetector.h"

void RegisterAllParams() {
    auto& ps = ParamStore::Instance();

    // ---- GPU（不导出到方案，每人硬件不同） ----
    {
        auto gpus = GpuDetector::enumerate();
        std::vector<std::string> opts = {"自动"};
        for (auto& g : gpus) {
            opts.push_back(g.name + " (" + std::to_string(g.vramMB) + "MB)");
        }
        if (opts.size() == 1) opts.push_back("默认显卡");
        ParamDef def{"gpu.selectedIndex", "显卡选择",
            ParamDef::Type::Enum, 0, static_cast<double>(opts.size()-1), 1, 0.0, opts};
        def.noExport = true; // 不导出到方案
        ps.RegisterParam(def);
    }

    // ---- 模式 ----
    ps.RegisterParam({"mode", "模式",
        ParamDef::Type::Enum, 0, 2, 1, 0.0, {"循环粒子", "弹球", "协奏"}});

    // 协奏 8 个子模式（0~7）—— 与 ConcertoEffectFactory::create() case 一一对应
    //   0=CrystalEffect     晶柱升级
    //   1=SpectrumBarEffect 极简频谱柱
    //   2=FluidWaveEffect   流体波
    //   3=ParticleFlowEffect 粒子流
    //   4=GridBeamEffect    网格光带
    //   5=LaserSweepEffect  激光线扫
    //   6=AuroraRibbonEffect 极光丝带
    //   7=PulseRingEffect   脉冲环
    ps.RegisterParam({"subMode", "子模式",
        ParamDef::Type::Enum, 0, 7, 1, 0.0,
        {"晶柱升级", "频谱柱", "流体波", "粒子流",
         "网格光带", "激光线扫", "极光丝带", "脉冲环"}});

    // ---- 边框 ----
    ps.RegisterParam({"borderWidth.top", "上边框宽度",
        ParamDef::Type::Double, 1, 500, 1, 100});
    ps.RegisterParam({"borderWidth.bottom", "下边框宽度",
        ParamDef::Type::Double, 1, 500, 1, 100});
    ps.RegisterParam({"borderWidth.left", "左边框宽度",
        ParamDef::Type::Double, 1, 500, 1, 100});
    ps.RegisterParam({"borderWidth.right", "右边框宽度",
        ParamDef::Type::Double, 1, 500, 1, 100});

    ps.RegisterParam({"cornerTransition", "转角过渡区",
        ParamDef::Type::Double, 0, 100, 1, 40});

    // ---- 分段 ----
    ps.RegisterParam({"segmentsPerEdge", "每边分段",
        ParamDef::Type::Int, 3, 8, 1, 4});

    // ---- 视觉与性能 ----
    ps.RegisterParam({"colorScheme", "颜色方案",
        ParamDef::Type::Enum, 0, 4, 1, 0.0,
        {"极光青", "熔岩橙", "星云紫", "音频驱动", "手动选择"}});

    ps.RegisterParam({"customColor.r", "手动颜色R",
        ParamDef::Type::Double, 0.0, 1.0, 0.01, 0.0});
    ps.RegisterParam({"customColor.g", "手动颜色G",
        ParamDef::Type::Double, 0.0, 1.0, 0.01, 1.0});
    ps.RegisterParam({"customColor.b", "手动颜色B",
        ParamDef::Type::Double, 0.0, 1.0, 0.01, 0.8});

    ps.RegisterParam({"particleCount", "粒子池容量",
        ParamDef::Type::Int, 500, 10000, 100, 1500});

    ps.RegisterParam({"targetFps", "目标帧率",
        ParamDef::Type::Int, 30, 360, 30, 60});

    ps.RegisterParam({"vsync", "垂直同步",
        ParamDef::Type::Enum, 0, 1, 1, 0, {"关闭", "开启"}});

    // ---- 物理 ----
    ps.RegisterParam({"physics.mass", "质量",
        ParamDef::Type::Double, 0.1, 2.0, 0.1, 0.6});
    ps.RegisterParam({"physics.stiffness", "刚度",
        ParamDef::Type::Double, 0.1, 2.0, 0.1, 0.4});
    ps.RegisterParam({"physics.damping", "阻尼",
        ParamDef::Type::Double, 0.1, 0.99, 0.01, 0.82});
    ps.RegisterParam({"physics.nonlinearity", "非线性刚度",
        ParamDef::Type::Double, 0.0, 1.0, 0.05, 0.3});

    // ---- 边框增强 ----
    ps.RegisterParam({"border.pulseAmount", "边框脉动幅度",
        ParamDef::Type::Double, 0.0, 0.1, 0.005, 0.03});
    ps.RegisterParam({"border.innerGlowWidth", "内发光宽度(px)",
        ParamDef::Type::Int, 0, 8, 1, 2});

    // ---- 休眠 ----
    ps.RegisterParam({"dormantBehavior", "无音频行为",
        ParamDef::Type::Enum, 0, 1, 1, 0.0, {"保持呼吸", "渐隐"}});
    ps.RegisterParam({"dormantThreshold", "休眠音量阈值",
        ParamDef::Type::Double, 0.0, 1.0, 0.01, 0.01});
    ps.RegisterParam({"dormantDelay", "休眠延迟(秒)",
        ParamDef::Type::Double, 1.0, 10.0, 1.0, 3.0});

    // ---- BounceBall 弹球参数 ----
    ps.RegisterParam({"bb.kTangentSpeed", "切向速度",
        ParamDef::Type::Double, 0.01, 5.0, 0.01, 0.15});
    ps.RegisterParam({"bb.kFollowSpeed", "法向跟随速度",
        ParamDef::Type::Double, 1.0, 30.0, 1.0, 25.0});
    ps.RegisterParam({"bb.rmsSensitivity", "RMS灵敏度",
        ParamDef::Type::Double, 1.0, 30.0, 1.0, 8.0});
    ps.RegisterParam({"bb.trailLength", "拖尾点数上限",
        ParamDef::Type::Int, 50, 5000, 50, 300});
    ps.RegisterParam({"bb.trailMaxAge", "拖尾寿命(秒)",
        ParamDef::Type::Double, 1.0, 15.0, 0.5, 5.0});
    ps.RegisterParam({"bb.dualMode", "球数模式",
        ParamDef::Type::Enum, 0, 1, 1, 0, {"单球", "双球"}});

    // ---- Cycle 粒子参数 ----
    ps.RegisterParam({"cycle.particleLife", "粒子寿命(秒)",
        ParamDef::Type::Double, 0.5, 5.0, 0.1, 2.0});
    ps.RegisterParam({"cycle.maxDistance", "最大行进距离(px)",
        ParamDef::Type::Int, 100, 2000, 50, 500});
    ps.RegisterParam({"cycle.sizeMin", "最小粒子大小(px)",
        ParamDef::Type::Int, 1, 50, 1, 3});
    ps.RegisterParam({"cycle.sizeMax", "最大粒子大小(px)",
        ParamDef::Type::Int, 3, 100, 1, 8});
    ps.RegisterParam({"cycle.emitMultiplier", "生成率(粒/秒)",
        ParamDef::Type::Int, 100, 10000, 50, 500});

    // ---- Concerto 光柱参数 ----
    ps.RegisterParam({"concerto.maxHeight", "光柱最大高度(px)",
        ParamDef::Type::Int, 10, 200, 10, 60});
    ps.RegisterParam({"concerto.columnsPerSeg", "每段光柱数",
        ParamDef::Type::Int, 1, 30, 1, 10});
    ps.RegisterParam({"concerto.maxTotalColumns", "光柱总数上限",
        ParamDef::Type::Int, 300, 8000, 100, 5000});
    ps.RegisterParam({"concerto.followSpeed", "跟随速度",
        ParamDef::Type::Double, 3.0, 30.0, 1.0, 12.0});
    ps.RegisterParam({"concerto.flowSpeed", "流动速度",
        ParamDef::Type::Double, 0.05, 2.0, 0.05, 0.4});
    ps.RegisterParam({"concerto.alphaBase", "基础透明度",
        ParamDef::Type::Double, 0.1, 0.8, 0.05, 0.4});
    ps.RegisterParam({"concerto.threshold", "显示阈值",
        ParamDef::Type::Double, 0.001, 0.05, 0.001, 0.002});
    ps.RegisterParam({"concerto.showBottom", "显示下边",
        ParamDef::Type::Enum, 0, 1, 1, 1, {"关闭", "开启"}});
    ps.RegisterParam({"concerto.showRight", "显示右边",
        ParamDef::Type::Enum, 0, 1, 1, 1, {"关闭", "开启"}});
    ps.RegisterParam({"concerto.showTop", "显示上边",
        ParamDef::Type::Enum, 0, 1, 1, 1, {"关闭", "开启"}});
    ps.RegisterParam({"concerto.showLeft", "显示左边",
        ParamDef::Type::Enum, 0, 1, 1, 1, {"关闭", "开启"}});
    // 音乐驱动颜色开关：LogicThread 通过 FFT 主频→色相→RGB 计算并推送
    ps.RegisterParam({"concerto.musicColor", "音乐驱动颜色",
        ParamDef::Type::Enum, 0, 1, 1, 0, {"关闭", "开启"}});

    // ---- 过渡/平滑参数 ----
    ps.RegisterParam({"transition.duration", "过渡时长(秒)",
        ParamDef::Type::Double, 0.1, 3.0, 0.1, 0.5});
}