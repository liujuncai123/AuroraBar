// ============================================================================
// ⚠️  DEPRECATED — 本文件已废弃，请勿在新代码中使用
// ----------------------------------------------------------------------------
//  原方案：SEH 包装的 OpenGL 渲染帧，配合 RenderThread（旧 Win32 方案）
//  原因  ：依赖 RenderThread + OverlayWindow，两者均已废弃。
//  替代  ：Qt6 方案中通过 QtOverlayWindow + QPainter blit 渲染，无需 SEH。
//  说明  ：文件保留供历史参考，不再参与主代码路径；新代码请勿 include。
// ============================================================================
#ifdef _MSC_VER
#pragma message("warning: " __FILE__ " is DEPRECATED — will be replaced by Qt-based renderer")
#endif

/**
 * @file RenderFrameSEH.cpp
 * @brief SEH 保护的 OpenGL 渲染帧 + 线程级崩溃捕获（独立编译单元避免 MSVC C2712）
 * @date 2026-07-15
 * @details MSVC /EHsc 模式下 __try/__except 与任何有析构函数的 C++ 对象
 *          在同一个函数中会触发 C2712 编译错误。本文件将 SEH 块隔离在
 *          最小的 C 风格辅助函数中，避免 C++ 临时对象。
 * @note 线程安全：本文件函数在各线程的 onRun() 中调用，不额外加锁。
 */
#include <atomic>
#include "../ui/OverlayWindow.h"
#include "Camera.h"
#include "modes/IModeRenderer.h"
#include "RenderThread.h"

/**
 * @brief SEH 保护的渲染帧
 * @param window  OverlayWindow 引用（beginFrame/endFrame）
 * @param camera  Camera 引用（update）
 * @param renderer 渲染器指针（renderFrame/renderParticles）
 * @param needParticles 兼容保留参数（已由 renderer->renderParticles 虚函数处理）
 * @param pContextLost 原子标志指针，崩溃时置 true 触发下帧恢复
 * @pre 调用前必须确保 makeCurrent() 成功
 * @post 正常：渲染一帧到 FBO；崩溃：pContextLost 置 true
 * @note 本函数故意保持为"纯 C 风格的 GL 调用"，无 C++ 临时对象，
 *       以绕过 MSVC C2712 限制。__except 块中不写日志宏。
 * @note 安全：原 static_cast<CycleRenderer*> 改为虚函数 renderParticles 调用，
 *              避免模式切换竞态导致类型不匹配的未定义行为
 */
void RenderFrameSEH_Impl(OverlayWindow& window, Camera& camera,
                         IModeRenderer* renderer, bool needParticles,
                         std::atomic<bool>* pContextLost) {
    __try {
        // 安全：beginFrame 返回 false 表示检测到僵尸上下文，跳过本帧
        //   endFrame 会因 m_frameValid=false 自动跳过 glReadPixels，避免无限挂起
        if (!window.beginFrame()) {
            if (pContextLost) *pContextLost = true;
            return;
        }
        camera.update();
        if (renderer) {
            renderer->renderFrame(1.0f / 60.0f);
            // 安全：虚函数分派，CycleRenderer 真正渲染粒子，其他模式默认空操作
            //   避免 static_cast 类型不匹配的 UB（原 needParticles 改为虚函数内部决定）
            renderer->renderParticles(camera);
        }
        window.endFrame();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // 安全：本块中不使用 C++ 对象，仅设置原子标志
        if (pContextLost) {
            *pContextLost = true;
        }
    }
}

/**
 * @brief SEH 保护的 RenderThread::onRun() 包装
 * @param self RenderThread 实例指针
 * @pre 调用前 RenderThread 已初始化
 * @post 正常：执行一帧渲染逻辑；崩溃：调用 markContextLost() 触发下帧恢复
 * @note 本函数不含 C++ 临时对象，绕过 MSVC C2712 限制
 */
void RenderThread_onRun_SEH(RenderThread* self) {
    __try {
        self->onRunBody();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        self->markContextLost();
    }
}
