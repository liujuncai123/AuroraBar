/**
 * @file BorderGeometry.cpp
 * @brief 边框几何计算实现 — 角区平滑法线 + 像素精确周长
 * @date 2026-07-06
 */

#include "BorderGeometry.h"
#include "../logging/LoggerManager.h"
#include <algorithm>
#include <cmath>

BorderGeometry::BorderGeometry() {
    AURORA_TRACE("BorderGeometry", "Constructor");
}

void BorderGeometry::compute(const BorderConfig& cfg, int screenW, int screenH,
                             int sampleCount)
{
    m_screenW = screenW;
    m_screenH = screenH;
    m_cfg     = cfg;
    m_points.clear();

    const double W = static_cast<double>(screenW);
    const double H = static_cast<double>(screenH);
    const double trans = static_cast<double>(
        std::min({cfg.cornerTransition, screenW/4, screenH/4}));

    // ── 边定义：顺时针 下→右→上→左 ──
    struct Edge { double x0, y0, x1, y1; int widthIdx; int index; };
    Edge edges[] = {
        {0, H,  W, H,  0, 0},  // 下边 →   (cfg.bottom)
        {W, H,  W, 0,  1, 1},  // 右边 ↑   (cfg.right)
        {W, 0,  0, 0,  2, 2},  // 上边 ←   (cfg.top)
        {0, 0,  0, H,  3, 3},  // 左边 ↓   (cfg.left)
    };
    double cfgW[4] = {cfg.bottom, cfg.right, cfg.top, cfg.left};

    // ── 每条边的内向法线（屏幕坐标，指向屏幕内部） ──
    // 下边：向内=上   → (0, -1)
    // 右边：向内=左   → (-1, 0)
    // 上边：向内=下   → (0,  1)
    // 左边：向内=右   → (1,  0)
    float edgeNX[4] = { 0.0f, -1.0f,  0.0f, 1.0f };
    float edgeNY[4] = {-1.0f,  0.0f,  1.0f, 0.0f };

    // ── 计算每条边像素长度和总周长 ──
    double edgeLen[4];
    double totalPerimeter = 0.0;
    for (int e = 0; e < 4; ++e) {
        double dx = edges[e].x1 - edges[e].x0;
        double dy = edges[e].y1 - edges[e].y0;
        edgeLen[e] = std::sqrt(dx*dx + dy*dy);
        totalPerimeter += edgeLen[e];
    }
    if (totalPerimeter <= 0.0) totalPerimeter = 1.0;

    // ── 按比例分配每条边的采样点数（像素精确周长） ──
    int perEdgeCount[4];
    int allocated = 0;
    for (int e = 0; e < 3; ++e) {
        perEdgeCount[e] = std::max(3, static_cast<int>(sampleCount * edgeLen[e] / totalPerimeter));
        allocated += perEdgeCount[e];
    }
    perEdgeCount[3] = std::max(3, sampleCount - allocated);

    m_points.reserve(sampleCount);

    for (int e = 0; e < 4; ++e) {
        int prevE = (e + 3) % 4;
        int nextE = (e + 1) % 4;
        double dx = edges[e].x1 - edges[e].x0;
        double dy = edges[e].y1 - edges[e].y0;
        double len = edgeLen[e];
        int nPts = perEdgeCount[e];
        double currentW = cfgW[edges[e].widthIdx];

        // 前后边宽度
        double prevW = cfgW[edges[prevE].widthIdx];
        double nextW = cfgW[edges[nextE].widthIdx];

        for (int i = 0; i < nPts; ++i) {
            double t = static_cast<double>(i) / (nPts - 1);  // 0→1 沿边
            double x = edges[e].x0 + dx * t;
            double y = edges[e].y0 + dy * t;

            double distFromStart = len * t;
            double distFromEnd   = len * (1.0 - t);

            // ── 宽度：单向 blend，起点区从 prevW→currentW，终点区从 currentW→nextW ──
            double blendWidth = currentW;
            if (distFromStart < trans) {
                double a = distFromStart / trans;  // 0→1
                blendWidth = prevW + (currentW - prevW) * a;
            }
            if (distFromEnd < trans) {
                double a = distFromEnd / trans;  // 0→1
                blendWidth = nextW + (currentW - nextW) * a;
            }

            // ── 法线：角区平滑插值 ──
            float nx = edgeNX[e];
            float ny = edgeNY[e];
            if (distFromStart < trans) {
                double a = distFromStart / trans;  // 0=prevEdge 法线, 1=currentEdge 法线
                float snx = static_cast<float>(edgeNX[prevE] + (edgeNX[e] - edgeNX[prevE]) * a);
                float sny = static_cast<float>(edgeNY[prevE] + (edgeNY[e] - edgeNY[prevE]) * a);
                float slen = std::sqrt(snx*snx + sny*sny);
                if (slen > 0.001f) { snx /= slen; sny /= slen; }
                nx = snx; ny = sny;
            }
            if (distFromEnd < trans) {
                double a = distFromEnd / trans;  // 0=nextEdge 法线, 1=currentEdge 法线
                float enx = static_cast<float>(edgeNX[nextE] + (edgeNX[e] - edgeNX[nextE]) * a);
                float eny = static_cast<float>(edgeNY[nextE] + (edgeNY[e] - edgeNY[nextE]) * a);
                float elen = std::sqrt(enx*enx + eny*eny);
                if (elen > 0.001f) { enx /= elen; eny /= elen; }
                nx = enx; ny = eny;
            }

            BorderPoint bp;
            bp.x = x;
            bp.y = y;
            bp.width = blendWidth;
            bp.edgeIndex = e;
            bp.nx = nx;
            bp.ny = ny;
            m_points.push_back(bp);
        }
    }

    // ── 重新计算精准周长（按像素距离） ──
    totalPerimeter = 0.0;
    for (size_t i = 1; i < m_points.size(); ++i) {
        double dx = m_points[i].x - m_points[i-1].x;
        double dy = m_points[i].y - m_points[i-1].y;
        totalPerimeter += std::sqrt(dx*dx + dy*dy);
    }
    // 闭合最后一个点到第一个点
    if (!m_points.empty()) {
        double dx = m_points[0].x - m_points.back().x;
        double dy = m_points[0].y - m_points.back().y;
        totalPerimeter += std::sqrt(dx*dx + dy*dy);
    }

    // 重新分配 perimeter 值
    if (!m_points.empty()) {
        double accum = 0.0;
        m_points[0].perimeter = 0.0;
        for (size_t i = 1; i < m_points.size(); ++i) {
            double dx = m_points[i].x - m_points[i-1].x;
            double dy = m_points[i].y - m_points[i-1].y;
            accum += std::sqrt(dx*dx + dy*dy);
            // 安全：totalPerimeter 已在上面检查过 >0，此处防除零
            m_points[i].perimeter = (totalPerimeter > 0.0) ? (accum / totalPerimeter) : 0.0;
        }
    }

    AURORA_INFO("BorderGeometry", "compute {}x{} totalPts={} perim={:.0f}px",
                screenW, screenH, m_points.size(), totalPerimeter);
}

size_t BorderGeometry::indexAt(double t) const {
    if (m_points.empty()) return 0;
    t = std::max(0.0, std::min(1.0, t));
    size_t idx = static_cast<size_t>(t * (m_points.size() - 1));
    if (idx >= m_points.size()) idx = m_points.size() - 1;
    return idx;
}