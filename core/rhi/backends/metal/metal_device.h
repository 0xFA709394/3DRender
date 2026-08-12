/**
 * @file metal_device.h
 * @brief Metal 后端入口声明（实现在 metal_device.mm，Objective-C++）。
 */
#pragma once
#include "rhi/rhi_device.h"

namespace rd {
/**
 * @brief 创建 Metal 后端设备。
 * @return 失败（无 Metal 设备/创建队列失败）返回 nullptr 并记日志。
 * @note 仅 Apple 平台编译；其他平台由 rhi_factory.cpp 提供返回 nullptr 的空实现。
 */
std::unique_ptr<Device> createMetalDevice(const DeviceDesc& desc);
}
