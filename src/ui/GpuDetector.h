/**
 * @file GpuDetector.h
 * @brief 通过 DXGI 枚举所有 GPU，按显存大小排序，自动选择最佳
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct GpuInfo {
    std::string name;       // GPU 名称
    uint64_t    vramMB;     // 显存 (MB)
    int         index;      // DXGI 适配器索引
};

class GpuDetector {
public:
    /// 枚举所有 GPU，返回按显存降序排列的列表
    static std::vector<GpuInfo> enumerate();

    /// 自动选择显存最大的 GPU，返回其索引（失败返回 -1）
    static int selectBest();

    /// 检查是否支持 GPU 选择（Windows 8+）
    static bool isSupported();
};