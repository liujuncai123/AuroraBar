/**
 * @file GpuDetector.cpp
 * @brief 通过 DXGI 枚举所有 GPU
 */

#include "GpuDetector.h"

#ifdef _WIN32
#include <d3d11.h>
#include <dxgi.h>
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#endif

#include <algorithm>

std::vector<GpuInfo> GpuDetector::enumerate() {
    std::vector<GpuInfo> result;

#ifdef _WIN32
    IDXGIFactory* factory = nullptr;
    HRESULT hr = CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) return result;

    UINT idx = 0;
    IDXGIAdapter* adapter = nullptr;
    while (factory->EnumAdapters(idx, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
            GpuInfo info;
            // 宽字符转 UTF-8
            int len = WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                info.name.resize(len - 1);
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, &info.name[0], len, nullptr, nullptr);
            }
            info.vramMB = desc.DedicatedVideoMemory / (1024 * 1024);
            info.index  = static_cast<int>(idx);
            // 跳过微软软件渲染器（WARP / Basic Render）
            if (info.vramMB > 0 && info.name.find("Microsoft") == std::string::npos) {
                result.push_back(info);
            }
        }
        adapter->Release();
        ++idx;
    }
    factory->Release();

    // 按显存降序
    std::sort(result.begin(), result.end(),
              [](const GpuInfo& a, const GpuInfo& b) { return a.vramMB > b.vramMB; });
#endif

    return result;
}

int GpuDetector::selectBest() {
    auto gpus = enumerate();
    if (gpus.empty()) return -1;
    return gpus[0].index; // 显存最大的
}

bool GpuDetector::isSupported() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}