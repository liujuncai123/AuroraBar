/**
 * @file BounceBallRenderer.cpp
 * @brief 弹球模式渲染器实现 —— 支持单/双球
 * @date 2026-07-07
 */
#include "BounceBallRenderer.h"
#include "../BorderGeometry.h"
#include "../Camera.h"
#include "../../core/CommandTypes.h"
#include "../../params/ParamStore.h"
#include <string_view>
#include "../../logging/LoggerManager.h"
#include <algorithm>
#include <cmath>
#include <cstring>

BounceBallRenderer::BounceBallRenderer() { AURORA_TRACE("BounceBall", "Constructor"); }
BounceBallRenderer::~BounceBallRenderer() { m_effect.cleanup(); }

Result<void> BounceBallRenderer::initialize(const BorderGeometry* geometry,
                                          const Camera* camera,
                                          int maxParticles,
                                          std::function<void()> glInitFn) {
    AURORA_INFO("BounceBall", "initialize() maxParticles={}", maxParticles);
    m_geometry = geometry;
    m_camera = camera;
    m_trailCapacity = std::max(50, maxParticles);
    m_ball1.trail.resize(m_trailCapacity);
    m_ball2.trail.resize(m_trailCapacity);
    m_ball1.perimeterPos = 0.0;
    m_ball2.perimeterPos = 0.5;  // 对面
    return m_effect.initialize(glInitFn, m_trailCapacity * 2);
}

Result<void> BounceBallRenderer::compileShaders() { return m_effect.compileShaders(); }

void BounceBallRenderer::updateBorder(const BorderGeometry& geo) {
    m_geometry = &geo;
    if (m_geometry->pointCount() > 0)
        m_effect.setScreenSize(m_geometry->screenW(), m_geometry->screenH());
}

double BounceBallRenderer::currentBorderWidth() const {
    if (!m_geometry || m_geometry->pointCount() == 0) return 100.0;
    size_t idx = m_geometry->indexAt(m_ball1.perimeterPos);
    return m_geometry->points()[idx].width;
}

void BounceBallRenderer::pushCommand(const RenderCommand& cmd) {
    switch (cmd.type) {
    case RenderCommand::Type::GlobalParam: {
        std::string_view name(cmd.paramName.data());
        if (name == "rawRms")
            m_rawRms = std::clamp(cmd.paramValue, 0.0, 1.0);
        else if (name == "bb.kTangentSpeed")
            m_kTangentSpeed = std::clamp(cmd.paramValue, 0.01, 5.0);
        else if (name == "bb.kFollowSpeed")
            m_kFollowSpeed = std::clamp(cmd.paramValue, 1.0, 30.0);
        else if (name == "bb.rmsSensitivity")
            m_rmsSensitivity = std::clamp(cmd.paramValue, 1.0, 30.0);
        else if (name == "bb.trailLength") {
            int newCap = std::max(50, static_cast<int>(cmd.paramValue));
            if (newCap != m_trailCapacity) {
                m_trailCapacity = newCap;
                // 球1拖尾缩容
                {
                    int keep = std::min(m_ball1.trailCount, newCap);
                    std::vector<TrailPoint> nt(newCap);
                    for (int i = 0; i < keep; ++i) {
                        int si = (m_ball1.trailWrite + m_ball1.trailCount - keep + i) % static_cast<int>(m_ball1.trail.size());
                        nt[i] = m_ball1.trail[si];
                    }
                    m_ball1.trail = std::move(nt);
                    m_ball1.trailWrite = 0;
                    m_ball1.trailCount = keep;
                }
                // 球2拖尾缩容
                {
                    int keep2 = std::min(m_ball2.trailCount, newCap);
                    std::vector<TrailPoint> nt(newCap);
                    for (int i = 0; i < keep2; ++i) {
                        int si = (m_ball2.trailWrite + m_ball2.trailCount - keep2 + i) % static_cast<int>(m_ball2.trail.size());
                        nt[i] = m_ball2.trail[si];
                    }
                    m_ball2.trail = std::move(nt);
                    m_ball2.trailWrite = 0;
                    m_ball2.trailCount = keep2;
                }
            }
        } else if (name == "bb.trailMaxAge")
            m_trailMaxAge = std::clamp(cmd.paramValue, 1.0, 15.0);
        else if (name == "bb.dualMode")
            m_dualMode = (static_cast<int>(cmd.paramValue) == 1);
        else if (name == "border.pulseAmount")
            m_pulseAmount = std::clamp(cmd.paramValue, 0.0, 0.1);
        break;
    }
    case RenderCommand::Type::DormantState:
        m_dormantCoeff = std::clamp(cmd.paramValue, 0.0, 1.0);
        break;
    case RenderCommand::Type::Onset:
        m_onsetStrength = std::max(m_onsetStrength, cmd.paramValue);
        break;
    default: break;
    }
}

void BounceBallRenderer::renderFrame(double dt) {
    if (dt <= 0.0) dt = 1.0 / 60.0;
    if (!m_geometry || m_geometry->pointCount() < 3) return;
    m_time += static_cast<float>(dt);

    if (m_onsetStrength > 0.001) m_onsetStrength *= exp(-dt * 8.0);
    else m_onsetStrength = 0.0;

    auto& ps = ParamStore::Instance();
    m_dualMode = (ps.GetInt("bb.dualMode") == 1);

    // === 物理（共享） ===
    double borderWidth = currentBorderWidth();
    double pulseFactor = 1.0 + m_pulseAmount * (0.5 + 0.5 * std::sin(m_time * 6.0));
    borderWidth *= pulseFactor;
    if (m_ball1.normalDist > borderWidth || m_ball1.normalDist < 0.0)
        m_ball1.normalDist = borderWidth * 0.5;

    double rmsNorm = std::min(1.0, m_rawRms * m_rmsSensitivity) * m_dormantCoeff;

    double rawTargetVel = rmsNorm * m_kTangentSpeed;
    double targetSmoothF = std::min(1.0, dt * 3.0);
    m_smoothedTargetVel += (rawTargetVel - m_smoothedTargetVel) * targetSmoothF;
    double tangentialSmooth = std::min(1.0, dt * 4.0);
    m_tangentialVel += (m_smoothedTargetVel - m_tangentialVel) * tangentialSmooth;
    m_tangentialVel = std::clamp(m_tangentialVel, -2000.0, 2000.0);

    double targetNormal = borderWidth * (0.02 + rmsNorm * 0.96);
    double followF = std::min(1.0, dt * m_kFollowSpeed);
    m_ball1.normalDist += (targetNormal - m_ball1.normalDist) * followF;
    if (m_ball1.normalDist < 0.0) m_ball1.normalDist = 0.0;
    if (m_ball1.normalDist > borderWidth) m_ball1.normalDist = borderWidth;
    m_ball2.normalDist = m_ball1.normalDist;

    m_ball1.perimeterPos += m_tangentialVel * dt;
    while (m_ball1.perimeterPos >= 1.0) m_ball1.perimeterPos -= 1.0;
    while (m_ball1.perimeterPos < 0.0) m_ball1.perimeterPos += 1.0;
    m_ball2.perimeterPos = std::fmod(m_ball1.perimeterPos + 0.5, 1.0);

    // 拖尾采样
    sampleTrail(m_ball1);
    if (m_dualMode) sampleTrail(m_ball2);

    if (m_dormantCoeff < 0.005) return;

    // 渲染球1
    renderBall(m_ball1, rmsNorm);
    // 渲染球2
    if (m_dualMode) renderBall(m_ball2, rmsNorm);
}

void BounceBallRenderer::renderBall(BallState& ball, double rmsNorm) {
    m_effect.computeBallScreenPos(*m_geometry, ball.perimeterPos, ball.normalDist);
    float pulseR = std::min(1.0f, static_cast<float>(rmsNorm * 2.0f + m_onsetStrength * 0.8f));
    m_effect.setBallState(m_effect.ballScreenX(), m_effect.ballScreenY(), 12.0f, pulseR);

    buildTrailVertices(ball);
    m_effect.setTrailData(ball.trailVertices.data(), static_cast<int>(ball.trailVertices.size()));

    if (m_camera) m_effect.render(*m_camera);
}

void BounceBallRenderer::sampleTrail(BallState& ball) {
    if (m_trailCapacity <= 0) return;
    bool empty = (ball.trailCount == 0);
    if (std::abs(m_tangentialVel) < 0.001 && !empty) return;

    if (static_cast<int>(ball.trail.size()) < m_trailCapacity)
        ball.trail.resize(m_trailCapacity);

    ball.trail[ball.trailWrite] = {ball.perimeterPos, ball.normalDist, m_time};
    ball.trailWrite = (ball.trailWrite + 1) % m_trailCapacity;
    if (ball.trailCount < m_trailCapacity) ball.trailCount++;
}

void BounceBallRenderer::buildTrailVertices(BallState& ball) {
    ball.trailVertices.clear();
    if (ball.trailCount == 0 || !m_geometry) return;
    ball.trailVertices.reserve(ball.trailCount);  // 避免 push_back 触发 reallocation

    int startIdx = (ball.trailCount >= m_trailCapacity) ? ball.trailWrite : 0;
    int cap = static_cast<int>(ball.trail.size());
    for (int i = 0; i < ball.trailCount; ++i) {
        int idx = (startIdx + i) % cap;
        auto& tp = ball.trail[idx];

        double age = m_time - tp.timestamp;
        if (age > m_trailMaxAge) continue;

        double ageRatio = std::clamp(1.0 - age / m_trailMaxAge, 0.0, 1.0);
        float sx, sy;
        m_effect.computeTrailScreenPos(*m_geometry, tp.perimeterPos, tp.normalDist, sx, sy);

        float alpha = 0.05f + 0.90f * static_cast<float>(ageRatio);
        if (alpha < 0.02f) continue;

        float hue = 1.0f - static_cast<float>(ageRatio);
        float r, g, b;
        BallTrailEffect::hslToRgb(hue, 0.9f, 0.55f, r, g, b);

        float size = 2.0f + alpha * 6.0f;
        ball.trailVertices.push_back({sx, sy, r, g, b, alpha, size});
    }
}