// 资源退休测试:帧循环内高频 create/destroy(模拟每帧重建场景),
// 数千帧无崩溃;waitIdle 清空退休队列。
// Metal 用例在 __APPLE__ 下启用;Vulkan 用例在 RD_WITH_VULKAN 下启用。
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"

namespace {
std::unique_ptr<rd::Device> make(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  return rd::createDevice(d);
}

void churn(rd::Device& dev, int frames) {
  auto target = dev.createOffscreenTarget({64, 64});
  ASSERT_TRUE(target.valid());
  for (int i = 0; i < frames; ++i) {
    dev.beginFrame();
    float data[16] = {};
    auto buf = dev.createBuffer({sizeof(data), rd::BufferUsage::Vertex, true, false, data});
    EXPECT_TRUE(buf.valid());
    dev.destroyBuffer(buf);  // 本帧 destroy:句柄立即失效,底层资源退休延迟释放
    dev.endFrame();
  }
  dev.destroyTarget(target);
  dev.waitIdle();  // waitIdle 清空退休队列
}
} // namespace

TEST(Retire, MetalChurn) {
#if defined(__APPLE__)
  auto dev = make(rd::Backend::Metal);
  ASSERT_NE(dev, nullptr);
  churn(*dev, 2000);
#endif
}

TEST(Retire, VulkanChurn) {
#if defined(RD_WITH_VULKAN)
  auto dev = make(rd::Backend::Vulkan);
  ASSERT_NE(dev, nullptr);
  churn(*dev, 2000);
#endif
}
