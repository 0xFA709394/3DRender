// 深度纹理(可渲染目标+可采样)与比较采样器创建契约。
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"

namespace {
void runCreate(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);

  // D32 渲染目标纹理(可采样)
  rd::TextureDesc td;
  td.format = rd::Format::D32_FLOAT;
  td.width = 256;
  td.height = 256;
  td.usage = rd::TextureUsage::Sampled | rd::TextureUsage::RenderTargetAttachment;
  auto tex = dev->createTexture(td);
  EXPECT_TRUE(tex.valid()) << "D32 RT 纹理创建失败";

  // 比较采样器
  rd::SamplerDesc sd;
  sd.compareEnable = true;
  sd.minFilter = rd::Filter::Linear;
  sd.magFilter = rd::Filter::Linear;
  auto cmp = dev->createSampler(sd);
  EXPECT_TRUE(cmp.valid()) << "比较采样器创建失败";

  // 默认(compareEnable=false)不受影响
  auto plain = dev->createSampler({});
  EXPECT_TRUE(plain.valid());

  dev->destroySampler(cmp);
  dev->destroySampler(plain);
  dev->destroyTexture(tex);
}
} // namespace

TEST(DepthTexture, Metal) {
#if defined(__APPLE__)
  runCreate(rd::Backend::Metal);
#endif
}
TEST(DepthTexture, Vulkan) {
#if defined(RD_WITH_VULKAN)
  runCreate(rd::Backend::Vulkan);
#endif
}
