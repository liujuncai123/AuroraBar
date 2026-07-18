/**
 * @file ParamDef.h
 * @brief 参数定义结构
 * @date 2026-07-06
 * @details 描述一个用户可调参数的元数据：名称、类型、范围、默认值。
 *          ParamStore 启动时注册所有参数定义，运行时校验类型和范围。
 */

#pragma once

#include <string>
#include <vector>

struct ParamDef {
    enum class Type { Int, Double, Bool, Enum };

    std::string key;              ///< 参数键名 (如 "borderWidth.top")
    std::string displayName;      ///< 显示名称 (如 "上边框宽度")
    Type type = Type::Double;     ///< 参数类型
    double minVal = 0.0;          ///< 最小值 (Int/Double 时) / 枚举索引下限 (Enum)
    double maxVal = 100.0;        ///< 最大值 / 枚举索引上限
    double step = 1.0;            ///< 步长
    double defaultVal = 50.0;     ///< 默认值 / 枚举默认索引
    std::vector<std::string> enumOptions;  ///< 枚举选项 (Type=Enum 时)
    bool   noExport = false;           ///< 不导出到方案（如 GPU 选择，每人硬件不同）
};
