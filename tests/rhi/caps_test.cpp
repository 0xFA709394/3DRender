// 能力表测试：三后端上报值健全性 + 名称表完整。
// Metal 用例在 __APPLE__ 下启用；Vulkan 用例在 RD_WITH_VULKAN 下启用（同 texture_test 模式）。
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"
#include "rhi/rhi_capability.h"

namespace {
std::unique_ptr<rd::Device> make(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  return rd::createDevice(d);
}
} // namespace

// 名称表：每个枚举都有非空名字（X-macro 三处展开一致性）
TEST(Caps, NameTableComplete) {
  for (uint32_t i = 0; i < static_cast<uint32_t>(rd::Capability::kCount); ++i) {
    auto c = static_cast<rd::Capability>(i);
    EXPECT_STRNE(rd::DeviceCaps::name(c), "");
  }
}

TEST(Caps, MetalReports) {
#if defined(__APPLE__)
  auto dev = make(rd::Backend::Metal);
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->caps().get(rd::Capability::instancing), 1u);
  EXPECT_GE(dev->caps().get(rd::Capability::max_texture_size), 4096u);
  EXPECT_TRUE(dev->caps().supports(rd::Capability::cube_render_target));
  EXPECT_TRUE(dev->caps().supports(rd::Capability::generate_mipmap));
#endif
}

TEST(Caps, VulkanReports) {
#if defined(RD_WITH_VULKAN)
  auto dev = make(rd::Backend::Vulkan);
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->caps().get(rd::Capability::instancing), 1u);
  EXPECT_GE(dev->caps().get(rd::Capability::max_texture_size), 2048u);
  EXPECT_TRUE(dev->caps().supports(rd::Capability::cube_render_target));
#endif
}
