// createDevice 工厂的单元测试：不可用后端的失败语义。
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"

// GLES 在 host 上永不可用（实现归 Android/P0-2），工厂应返回 nullptr 且不崩溃。
// Metal/Vulkan 的可用性由 cube 测试覆盖（见 Task 10/11）。
TEST(Factory, UnavailableBackendReturnsNull) {
  rd::DeviceDesc desc;
  desc.backend = rd::Backend::GLES;
  EXPECT_EQ(rd::createDevice(desc), nullptr);
}
