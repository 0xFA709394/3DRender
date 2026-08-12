/**
 * @file gles_device.h
 * @brief GLES 后端入口声明（实现在 gles_device.cpp，仅在 __ANDROID__ 下编译生效）。
 */
#pragma once
#include "rhi/rhi_device.h"

namespace rd {
/**
 * @brief 创建 GLES(ES3) 后端设备。
 * @return 失败（EGL 初始化失败等）返回 nullptr 并记日志。
 * @note P0 阶段 GLES 后端仅支持 Android；其他平台由 rhi_factory.cpp
 *       提供返回 nullptr 的空实现。
 */
std::unique_ptr<Device> createGLESDevice(const DeviceDesc& desc);
}
