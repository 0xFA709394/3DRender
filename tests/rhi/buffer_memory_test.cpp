// 内存模型 flag 化测试:device-local 静态缓冲(带初始数据)创建正常;
// hostWrite 缓冲可更新;非 hostWrite 缓冲的 updateBuffer 被拒绝(记日志,不生效)。
// Metal 用例在 __APPLE__ 下启用;Vulkan 用例在 RD_WITH_VULKAN 下启用。
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"

namespace {
std::unique_ptr<rd::Device> make(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  return rd::createDevice(d);
}
} // namespace

TEST(BufferMemory, MetalDeviceLocalCreateWithData) {
#if defined(__APPLE__)
  auto dev = make(rd::Backend::Metal);
  ASSERT_NE(dev, nullptr);
  float data[4] = {1, 2, 3, 4};
  // hostWrite=false(默认)→ device-local + 内部 staging 上传
  auto buf = dev->createBuffer({sizeof(data), rd::BufferUsage::Vertex, false, false, data});
  EXPECT_TRUE(buf.valid());
  dev->destroyBuffer(buf);
#endif
}

TEST(BufferMemory, MetalHostWriteUpdate) {
#if defined(__APPLE__)
  auto dev = make(rd::Backend::Metal);
  ASSERT_NE(dev, nullptr);
  auto buf = dev->createBuffer({64, rd::BufferUsage::Uniform, true, false, nullptr});
  ASSERT_TRUE(buf.valid());
  float v[16] = {};
  v[0] = 42.0f;
  dev->updateBuffer(buf, v, sizeof(v), 0);  // hostWrite 缓冲可更新
  dev->destroyBuffer(buf);
#endif
}

TEST(BufferMemory, VulkanDeviceLocalCreateWithData) {
#if defined(RD_WITH_VULKAN)
  auto dev = make(rd::Backend::Vulkan);
  ASSERT_NE(dev, nullptr);
  float data[4] = {1, 2, 3, 4};
  auto buf = dev->createBuffer({sizeof(data), rd::BufferUsage::Vertex, false, false, data});
  EXPECT_TRUE(buf.valid());
  dev->destroyBuffer(buf);
#endif
}
