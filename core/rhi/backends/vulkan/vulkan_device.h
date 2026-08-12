/**
 * @file vulkan_device.h
 * @brief Vulkan 后端入口声明（实现在 vulkan_device.cpp）。
 */
#pragma once
#include "rhi/rhi_device.h"

namespace rd {
/**
 * @brief 创建 Vulkan 后端设备。
 * @return 失败（无实例/无物理设备/无图形队列等）返回 nullptr 并记日志。
 * @note macOS 上经 MoltenVK 运行（直连 ICD）；Android 为原生 Vulkan。
 *       未编译启用（无 RD_WITH_VULKAN）的平台由 rhi_factory.cpp 提供空实现。
 */
std::unique_ptr<Device> createVulkanDevice(const DeviceDesc& desc);
}
