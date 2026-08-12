// 纹理/采样器创建的单元测试：正常路径（2D/Cube/mip）与非法参数拒绝。
// Metal 用例在 __APPLE__ 下启用；Vulkan 用例在 RD_WITH_VULKAN 下启用。
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"

namespace {
std::unique_ptr<rd::Device> makeMetal() {
  rd::DeviceDesc d;
  d.backend = rd::Backend::Metal;
  return rd::createDevice(d);
}
std::unique_ptr<rd::Device> makeVulkan() {
  rd::DeviceDesc d;
  d.backend = rd::Backend::Vulkan;
  return rd::createDevice(d);
}
} // namespace

// 2D 纹理：带初始数据 + 多级 mip，创建与销毁正常
TEST(Texture, CreateDestroy2D) {
#if defined(__APPLE__)
  auto dev = makeMetal();
  ASSERT_NE(dev, nullptr);
  // 4x4 RGBA8，3 mip（4x4 + 2x2 + 1x1 共 84 字节）
  uint8_t pixels[84] = {};
  for (auto& p : pixels) p = 128;
  rd::TextureDesc td;
  td.width = 4;
  td.height = 4;
  td.mipLevels = 3;
  td.data = pixels;
  td.dataSize = sizeof(pixels);
  auto tex = dev->createTexture(td);
  EXPECT_TRUE(tex.valid());
  dev->destroyTexture(tex);
#endif
}

// Cube 纹理：6 面数据布局（面序 +X,-X,+Y,-Y,+Z,-Z）
TEST(Texture, CreateCube) {
#if defined(__APPLE__)
  auto dev = makeMetal();
  ASSERT_NE(dev, nullptr);
  rd::TextureDesc td;
  td.type = rd::TextureType::Cube;
  td.width = 4;
  td.height = 4;
  td.mipLevels = 1;
  uint8_t pixels[4 * 4 * 4 * 6] = {}; // 6 面
  td.data = pixels;
  td.dataSize = sizeof(pixels);
  EXPECT_TRUE(dev->createTexture(td).valid());
#endif
}

// 全零 Desc（width/height/mipLevels 为 0）必须被拒绝
TEST(Texture, InvalidDescReturnsEmpty) {
#if defined(__APPLE__)
  auto dev = makeMetal();
  ASSERT_NE(dev, nullptr);
  rd::TextureDesc td; // 全零
  EXPECT_FALSE(dev->createTexture(td).valid());
#endif
}

// 采样器：默认 Desc（Linear/Repeat）创建销毁正常
TEST(Texture, SamplerCreateDestroy) {
#if defined(__APPLE__)
  auto dev = makeMetal();
  ASSERT_NE(dev, nullptr);
  rd::SamplerDesc sd;
  auto s = dev->createSampler(sd);
  EXPECT_TRUE(s.valid());
  dev->destroySampler(s);
#endif
}

// 约束：Cube 必须方形，非方形拒绝
TEST(Texture, CubeNonSquareRejected) {
#if defined(__APPLE__)
  auto dev = makeMetal();
  ASSERT_NE(dev, nullptr);
  rd::TextureDesc td;
  td.type = rd::TextureType::Cube;
  td.width = 4;
  td.height = 8;
  td.mipLevels = 1;
  EXPECT_FALSE(dev->createTexture(td).valid());
#endif
}

// 约束：mipLevels 不得超过 floor(log2(max(w,h)))+1
TEST(Texture, ExcessiveMipLevelsRejected) {
#if defined(__APPLE__)
  auto dev = makeMetal();
  ASSERT_NE(dev, nullptr);
  rd::TextureDesc td;
  td.width = 4;
  td.height = 4;
  td.mipLevels = 10; // 4x4 最多 3 级
  EXPECT_FALSE(dev->createTexture(td).valid());
#endif
}

// Vulkan 后端的 Cube 方形约束（与 Metal 一致的失败语义）
TEST(Texture, VulkanCubeNonSquareRejected) {
#if defined(RD_WITH_VULKAN)
  auto dev = makeVulkan();
  ASSERT_NE(dev, nullptr);
  rd::TextureDesc td;
  td.type = rd::TextureType::Cube;
  td.width = 4;
  td.height = 8;
  td.mipLevels = 1;
  EXPECT_FALSE(dev->createTexture(td).valid());
#endif
}

// Vulkan 后端的 mip 上限约束
TEST(Texture, VulkanExcessiveMipLevelsRejected) {
#if defined(RD_WITH_VULKAN)
  auto dev = makeVulkan();
  ASSERT_NE(dev, nullptr);
  rd::TextureDesc td;
  td.width = 4;
  td.height = 4;
  td.mipLevels = 10; // 4x4 最多 3 级
  EXPECT_FALSE(dev->createTexture(td).valid());
#endif
}
