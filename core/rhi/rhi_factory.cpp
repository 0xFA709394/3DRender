// Device 工厂的实现：按 DeviceDesc::backend 分发到各后端的创建函数。
//
// 后端启用方式（编译期）：
//   - Metal/Vulkan：由构建系统定义 RD_WITH_METAL / RD_WITH_VULKAN（找到对应 SDK 时启用）
//   - GLES：随 __ANDROID__ 平台宏启用（P0 阶段 GLES 后端仅支持 Android）
// 未启用的后端走下方的 static 空实现（返回 nullptr），使工厂在任何平台都可链接。
#include "foundation/log.h"
#include "rhi/rhi_device.h"

namespace rd {

// 后端创建函数由各后端编译单元提供；未启用时走下方弱实现。
#if defined(RD_WITH_METAL)
std::unique_ptr<Device> createMetalDevice(const DeviceDesc& desc);
#else
static std::unique_ptr<Device> createMetalDevice(const DeviceDesc&) { return nullptr; }
#endif

#if defined(RD_WITH_VULKAN)
std::unique_ptr<Device> createVulkanDevice(const DeviceDesc& desc);
#else
static std::unique_ptr<Device> createVulkanDevice(const DeviceDesc&) { return nullptr; }
#endif

#if defined(__ANDROID__)
std::unique_ptr<Device> createGLESDevice(const DeviceDesc& desc);
#else
static std::unique_ptr<Device> createGLESDevice(const DeviceDesc&) { return nullptr; }
#endif

std::unique_ptr<Device> createDevice(const DeviceDesc& desc) {
  std::unique_ptr<Device> device;
  switch (desc.backend) {
    case Backend::Metal:
      device = createMetalDevice(desc);
      break;
    case Backend::Vulkan:
      device = createVulkanDevice(desc);
      break;
    case Backend::GLES:
      device = createGLESDevice(desc);
      break;
  }
  // 后端未启用或运行期初始化失败：记警告并返回 nullptr（内核不用异常）。
  if (!device) {
    RD_LOGW("rhi", "backend %d unavailable", static_cast<int>(desc.backend));
  }
  return device;
}

} // namespace rd
