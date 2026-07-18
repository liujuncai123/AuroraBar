cmake_minimum_required(VERSION 3.21)
project(AuroraBar LANGUAGES CXX)

# ============================================================
# C++ 标准
# ============================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# MSVC 特定设置
# ============================================================
if(MSVC)
    add_compile_options(/W4 /utf-8)
endif()

# ============================================================
# 第三方库
# ============================================================
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets OpenGL OpenGLWidgets)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(FFTW3 REQUIRED)
find_package(GLEW REQUIRED)

qt_standard_project_setup()

# ============================================================
# 主程序源文件
# ============================================================
set(SOURCES
    src/main.cpp
    src/ui/mainwindow.cpp
    src/ui/mainwindow.h
    src/ui/mainwindow.ui
    # 核心基础设施
    src/core/ThreadBase.h
    src/core/ThreadBase.cpp
    # 日志
    src/logging/LoggerManager.h
    src/logging/LoggerManager.cpp
    # 参数
    src/params/ParamStore.h
    src/params/ParamStore.cpp
    src/params/ParamDef.h
    src/params/BuiltInDefaults.h
    src/params/BuiltInDefaults.cpp
    # 配置
    src/config/ConfigManager.h
    src/config/ConfigManager.cpp
    # 阶段 3：线程骨架
    src/core/CommandTypes.h
    src/audio/AudioCaptureThread.h
    src/analysis/LogicThread.h
    # [DEPRECATED] src/render/RenderThread.h —— Win32 方案，已废弃，将被 Qt 方案替代
    # src/render/RenderThread.h
    # 阶段 4：WASAPI 采集
    src/audio/AudioCapture.h
    src/audio/WasapiCapture.h
    src/audio/WasapiCapture.cpp
    # 阶段 5：FFT + 特征提取
    src/audio/FftProcessor.h
    src/audio/FftProcessor.cpp
    src/analysis/FeatureSet.h
    src/analysis/IFeatureExtractor.h
    src/analysis/RmsExtractor.h
    src/analysis/BandEnergyExtractor.h
    src/analysis/HpssExtractor.h
    src/analysis/SpectralFluxExtractor.h
    src/analysis/LoudnessExtractor.h
    # 阶段 6：物理模拟 + 分段
    src/physics/PhysicsState.h
    src/physics/PhysicsState.cpp
    src/segmentation/SegmentationManager.h
    src/segmentation/SegmentationManager.cpp
    # 阶段 7：过渡系统
    src/transition/EasingFunctions.h
    src/transition/ParameterTransition.h
    src/transition/ParameterTransition.cpp
    src/transition/StateTransition.h
    src/transition/StateTransition.cpp
    src/transition/TransitionManager.h
    src/transition/TransitionManager.cpp
    # 阶段 8：OpenGL 渲染基础
    src/render/Camera.h
    src/render/Camera.cpp
    src/render/BorderGeometry.h
    src/render/BorderGeometry.cpp
    src/render/ColorScheme.h
    src/render/ColorScheme.cpp
    # 阶段 9：粒子效果
    src/render/effects/IEffect.h
    src/render/effects/ParticleEffect.h
    src/render/effects/ParticleEffect.cpp
    src/render/modes/IModeRenderer.h
    src/render/modes/CycleRenderer.h
    src/render/modes/CycleRenderer.cpp
    # 阶段 13：BounceBall 弹球模式
    src/render/effects/BallTrailEffect.h
    src/render/effects/BallTrailEffect.cpp
    src/render/modes/BounceBallRenderer.h
    src/render/modes/BounceBallRenderer.cpp
    # 阶段 14：Concerto 协奏曲模式
    src/render/effects/LightColumnEffect.h
    src/render/effects/LightColumnEffect.cpp
    src/render/modes/ConcertoRenderer.h
    src/render/modes/ConcertoRenderer.cpp
    # 阶段 15：协奏模式重写——独立 Effect 架构（替代 LightColumnEffect）
    src/render/effects/concerto/IConcertoEffect.h
    src/render/effects/concerto/ConcertoEffectFactory.h
    src/render/effects/concerto/ConcertoEffectFactory.cpp
    src/render/effects/concerto/CrystalEffect.h
    src/render/effects/concerto/CrystalEffect.cpp
    src/render/effects/concerto/SpectrumBarEffect.h
    src/render/effects/concerto/SpectrumBarEffect.cpp
    src/render/effects/concerto/FluidWaveEffect.h
    src/render/effects/concerto/FluidWaveEffect.cpp
    src/render/effects/concerto/ParticleFlowEffect.h
    src/render/effects/concerto/ParticleFlowEffect.cpp
    src/render/effects/concerto/GridBeamEffect.h
    src/render/effects/concerto/GridBeamEffect.cpp
    src/render/effects/concerto/LaserSweepEffect.h
    src/render/effects/concerto/LaserSweepEffect.cpp
    src/render/effects/concerto/AuroraRibbonEffect.h
    src/render/effects/concerto/AuroraRibbonEffect.cpp
    src/render/effects/concerto/PulseRingEffect.h
    src/render/effects/concerto/PulseRingEffect.cpp
    # 阶段 10：GlContext + 全流程串联
    src/render/GlContext.h
    src/render/GlContext.cpp
    # 阶段 11：透明叠加窗口
    # ⚠️ [DEPRECATED] 旧 Win32 方案，main.cpp 仍依赖 RenderThread 迁移期间保留编译
    #     完成迁移后请删除以下 3 行，并清理 main.cpp 中的 RenderThread 引用
    src/ui/OverlayWindow.h
    src/ui/OverlayWindow.cpp
    # SEH 保护渲染帧（独立编译单元避免 MSVC C2712）
    src/render/RenderFrameSEH.cpp
    # Logic + Audio 线程 SEH 保护
    src/core/ThreadSEH.cpp
    # 阶段 12：设置面板
    src/ui/SettingsPanel.h
    src/ui/SettingsPanel.cpp
    # GPU 检测
    src/ui/GpuDetector.h
    src/ui/GpuDetector.cpp
    # ⚠️ [DEPRECATED] 独立提交线程，迁移期间保留编译
    #     完成迁移后请删除以下 2 行，并清理 main.cpp 中的 RenderThread 引用
    src/ui/SubmitThread.h
    src/ui/SubmitThread.cpp
    # Qt6 版透明叠加窗口（DirectComposition 方案验证）
    src/ui/QtOverlayWindow.h
    src/ui/QtOverlayWindow.cpp
    # Qt6 版独立渲染线程（替代旧 RenderThread，离屏 GL + 信号槽跨线程）
    src/ui/QtRenderThread.h
    src/ui/QtRenderThread.cpp
)

set(CORE_HEADERS
    src/core/Result.h
    src/core/SPSCQueue.h
    src/core/ObjectPool.h
    src/core/CommandTypes.h
    src/core/ThreadBase.h
    src/logging/LoggerManager.h
)

# ============================================================
# Windows 应用图标资源（嵌入 EXE）
# ============================================================
set(APP_ICON_RC "${CMAKE_CURRENT_SOURCE_DIR}/res/app_icon.rc")

qt_add_executable(AuroraBar
    WIN32
    ${SOURCES}
    ${CORE_HEADERS}
    ${APP_ICON_RC}
)

target_include_directories(AuroraBar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(AuroraBar
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::OpenGL
        Qt::OpenGLWidgets
        spdlog::spdlog
        nlohmann_json::nlohmann_json
        ole32
        opengl32
        dwmapi
        FFTW3::fftw3
        GLEW::GLEW
)

# ============================================================
# 构建后自动部署：复制 icon + 补 Qt6OpenGL + windeployqt
# 注意：不能直接用 CMAKE_PREFIX_PATH 复制 DLL，因为 vcpkg 会将其拼接为多路径
# ============================================================

# Qt bin 目录（用于复制 windeployqt 漏检的 DLL）
# 安全：优先取 Qt6Core 的位置，fallback 到 CMAKE_PREFIX_PATH
get_target_property(QT6CORE_LOCATION Qt6::Core LOCATION)
get_filename_component(QT_BIN_DIR "${QT6CORE_LOCATION}" DIRECTORY)

add_custom_command(TARGET AuroraBar POST_BUILD
    # 复制 icon.png（托盘图标用）
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/res/icon.png"
        "$<TARGET_FILE_DIR:AuroraBar>/icon.png"
    # 补充 windeployqt 漏检测的 Qt6OpenGL*.dll
    # 注意：Debug 模式用 Qt6OpenGLd.dll，Release 用 Qt6OpenGL.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_BIN_DIR}/$<IF:$<CONFIG:Debug>,Qt6OpenGLd.dll,Qt6OpenGL.dll>"
        "$<TARGET_FILE_DIR:AuroraBar>/$<IF:$<CONFIG:Debug>,Qt6OpenGLd.dll,Qt6OpenGL.dll>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QT_BIN_DIR}/$<IF:$<CONFIG:Debug>,Qt6OpenGLWidgetsd.dll,Qt6OpenGLWidgets.dll>"
        "$<TARGET_FILE_DIR:AuroraBar>/$<IF:$<CONFIG:Debug>,Qt6OpenGLWidgetsd.dll,Qt6OpenGLWidgets.dll>"
    COMMENT "Copying icon.png + Qt6OpenGL DLLs"
)

# windeployqt 必须在 vcpkg applocal 之后运行
add_custom_command(TARGET AuroraBar POST_BUILD
    COMMAND "${QT_BIN_DIR}/windeployqt.exe" --no-translations --no-opengl-sw "$<TARGET_FILE:AuroraBar>"
    COMMENT "Running windeployqt for Qt runtime DLLs"
)

# ============================================================
# [AuroraBar 发布版] 测试目标已禁用
#   tests/ 目录未同步到此发布副本，用 if(FALSE) 整体跳过测试段定义
#   后续若需启用，移除外层 if(FALSE)/endif() 即可
# ============================================================
if(FALSE)
# ============================================================
# 阶段 8 冒烟测试：Camera + BorderGeometry + ColorScheme
# ============================================================
add_executable(phase8_smoke_test
    tests/phase8_smoke_test.cpp
    src/core/ThreadBase.cpp
    src/logging/LoggerManager.cpp
    src/render/Camera.cpp
    src/render/BorderGeometry.cpp
    src/render/ColorScheme.cpp
)

target_include_directories(phase8_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(phase8_smoke_test PRIVATE
    Threads::Threads
    spdlog::spdlog
)

# ============================================================
# 阶段 9 冒烟测试：ParticleEffect + CycleRenderer
# ============================================================
add_executable(phase9_smoke_test
    tests/phase9_smoke_test.cpp
    src/core/ThreadBase.cpp
    src/logging/LoggerManager.cpp
    src/physics/PhysicsState.cpp
    src/segmentation/SegmentationManager.cpp
    src/render/Camera.cpp
    src/render/BorderGeometry.cpp
    src/render/ColorScheme.cpp
    src/render/effects/ParticleEffect.cpp
    src/render/effects/BallTrailEffect.cpp
    src/render/modes/CycleRenderer.cpp
    src/render/modes/BounceBallRenderer.cpp
    src/render/effects/LightColumnEffect.cpp
    src/render/modes/ConcertoRenderer.cpp
    src/ui/OverlayWindow.cpp
)

target_include_directories(phase9_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(phase9_smoke_test PRIVATE
    Threads::Threads
    spdlog::spdlog
    opengl32
    GLEW::GLEW
)

# ============================================================
# 阶段 10 全流程冒烟测试
# ============================================================
add_executable(phase10_smoke_test
    tests/phase10_smoke_test.cpp
    src/core/ThreadBase.cpp
    src/logging/LoggerManager.cpp
    src/params/ParamStore.cpp
    src/audio/FftProcessor.cpp
    src/physics/PhysicsState.cpp
    src/segmentation/SegmentationManager.cpp
    src/transition/ParameterTransition.cpp
    src/transition/StateTransition.cpp
    src/transition/TransitionManager.cpp
    src/render/Camera.cpp
    src/render/BorderGeometry.cpp
    src/render/ColorScheme.cpp
    src/render/effects/ParticleEffect.cpp
    src/render/effects/BallTrailEffect.cpp
    src/render/modes/CycleRenderer.cpp
    src/render/modes/BounceBallRenderer.cpp
    src/render/effects/LightColumnEffect.cpp
    src/render/modes/ConcertoRenderer.cpp
    src/render/GlContext.cpp
    src/ui/OverlayWindow.cpp
)

target_include_directories(phase10_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(phase10_smoke_test PRIVATE
    Threads::Threads
    spdlog::spdlog
    opengl32
    dwmapi
    FFTW3::fftw3
    GLEW::GLEW
)

# ============================================================
# 稳定性回归测试：上下文丢失安全 + 粒子系统边界条件
# ============================================================
add_executable(stability_test
    tests/stability_test.cpp
    src/logging/LoggerManager.cpp
    src/render/effects/ParticleEffect.cpp
)

target_include_directories(stability_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(stability_test PRIVATE
    spdlog::spdlog
    opengl32
    GLEW::GLEW
)

# ============================================================
# 阶段 1 冒烟测试 (独立可执行文件)
# ============================================================
add_executable(phase1_smoke_test
    tests/phase1_smoke_test.cpp
    src/core/ThreadBase.cpp
    src/logging/LoggerManager.cpp
)

target_include_directories(phase1_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(phase1_smoke_test PRIVATE Threads::Threads spdlog::spdlog)

# ============================================================
# 阶段 2 冒烟测试
# ============================================================
add_executable(phase2_smoke_test
    tests/phase2_smoke_test.cpp
    src/core/ThreadBase.cpp
    src/logging/LoggerManager.cpp
    src/params/ParamStore.cpp
    src/params/BuiltInDefaults.cpp
    src/config/ConfigManager.cpp
)

target_include_directories(phase2_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(phase2_smoke_test PRIVATE
    Threads::Threads
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

# ============================================================
# 阶段 3 冒烟测试：4 线程启停 + 3 队列完整性
# ============================================================
add_executable(phase3_smoke_test
    tests/phase3_smoke_test.cpp
    src/core/ThreadBase.cpp
    src/logging/LoggerManager.cpp
    src/params/ParamStore.cpp
    src/audio/WasapiCapture.cpp
    src/audio/FftProcessor.cpp
    src/physics/PhysicsState.cpp
    src/segmentation/SegmentationManager.cpp
    src/transition/ParameterTransition.cpp
    src/transition/StateTransition.cpp
    src/transition/TransitionManager.cpp
    src/render/Camera.cpp
    src/render/BorderGeometry.cpp
    src/render/ColorScheme.cpp
    src/render/effects/ParticleEffect.cpp
    src/render/effects/BallTrailEffect.cpp
    src/render/modes/CycleRenderer.cpp
    src/render/modes/BounceBallRenderer.cpp
    src/render/effects/LightColumnEffect.cpp
    src/render/modes/ConcertoRenderer.cpp
    src/ui/OverlayWindow.cpp
)

target_include_directories(phase3_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(phase3_smoke_test PRIVATE
    Threads::Threads
    spdlog::spdlog
    ole32
    opengl32
    dwmapi
    FFTW3::fftw3
    GLEW::GLEW
)

# ============================================================
# 阶段 4 冒烟测试：WASAPI 采集 + AudioCaptureThread 集成
# ============================================================
add_executable(phase4_smoke_test
    tests/phase4_smoke_test.cpp
    src/core/ThreadBase.cpp
    src/logging/LoggerManager.cpp
    src/audio/WasapiCapture.cpp
)

target_include_directories(phase4_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(phase4_smoke_test PRIVATE
    Threads::Threads
    spdlog::spdlog
    ole32
)

# ============================================================
# 阶段 5 冒烟测试：FFT + 5 特征提取器
# ============================================================
add_executable(phase5_smoke_test
    tests/phase5_smoke_test.cpp
    src/core/ThreadBase.cpp
    src/logging/LoggerManager.cpp
    src/audio/WasapiCapture.cpp
    src/audio/FftProcessor.cpp
)

target_include_directories(phase5_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(phase5_smoke_test PRIVATE
    Threads::Threads
    spdlog::spdlog
    ole32
    FFTW3::fftw3
)

# ============================================================
# 阶段 6 冒烟测试：物理模拟 + 分段管理
# ============================================================
add_executable(phase6_smoke_test
    tests/phase6_smoke_test.cpp
    src/core/ThreadBase.cpp
    src/logging/LoggerManager.cpp
    src/audio/WasapiCapture.cpp
    src/audio/FftProcessor.cpp
    src/physics/PhysicsState.cpp
    src/segmentation/SegmentationManager.cpp
)

target_include_directories(phase6_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(phase6_smoke_test PRIVATE
    Threads::Threads
    spdlog::spdlog
    ole32
    FFTW3::fftw3
)

# ============================================================
# 阶段 7 冒烟测试：过渡系统
# ============================================================
add_executable(phase7_smoke_test
    tests/phase7_smoke_test.cpp
    src/core/ThreadBase.cpp
    src/logging/LoggerManager.cpp
    src/audio/WasapiCapture.cpp
    src/audio/FftProcessor.cpp
    src/physics/PhysicsState.cpp
    src/segmentation/SegmentationManager.cpp
    src/transition/ParameterTransition.cpp
    src/transition/StateTransition.cpp
    src/transition/TransitionManager.cpp
)

target_include_directories(phase7_smoke_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(phase7_smoke_test PRIVATE
    Threads::Threads
    spdlog::spdlog
    ole32
    FFTW3::fftw3
)

endif()  # [AuroraBar 发布版] 测试段禁用结束

# ============================================================
# 安装与部署
# ============================================================
include(GNUInstallDirs)

install(TARGETS AuroraBar
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET AuroraBar
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
