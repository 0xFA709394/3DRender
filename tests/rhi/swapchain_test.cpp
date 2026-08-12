// swapchain 的单元测试：非法参数的防御行为（host 无窗口系统，无法测正常路径，
// 正常路径由双端模拟器截图验证覆盖，见 AGENTS.md）。
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"

// host 无窗口系统：非法 native window 应返回无效句柄而非崩溃
TEST(SwapChain, NullNativeWindowReturnsInvalid) {
#if defined(__APPLE__)
  rd::DeviceDesc desc;
  desc.backend = rd::Backend::Metal;
  auto device = rd::createDevice(desc);
  ASSERT_NE(device, nullptr);
  auto sc = device->createSwapChain(nullptr, 512, 512);
  EXPECT_FALSE(sc.valid());
#endif
}
