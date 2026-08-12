// 立方体渲染的 golden image 测试（RHI 三后端渲染正确性的核心回归测试）。
//
// 流程：离屏目标渲染旋转 45° 的立方体 → readback → 与 tests/golden/ 下的
// 基准 PNG 做容差比较（channelTol=3，ratioTol=1%~2%——允许不同 GPU/驱动的
// 微小栅格化差异，但能抓住渲染错误）。
//
// 更新 golden：RD_UPDATE_GOLDENS=1 ctest --test-dir build -R Cube
// （重新生成基准 PNG 并跳过比较），更新后须目视核对 tests/golden/*.png 再提交。
#include <gtest/gtest.h>
#include <cstdlib>
#include <cmath>
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "scene/cube_scene.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 512, kH = 512;
constexpr float kAngle = 0.78539816f; // 45°

// golden 文件路径：按后端区分（Metal/Vulkan 各自一份基准）
std::string goldenPath(rd::Backend b) {
  std::string name = (b == rd::Backend::Metal) ? "cube_metal.png" : "cube_vulkan.png";
  return std::string(RD_TEST_DATA_DIR) + "/golden/" + name;
}

// 用指定后端渲染立方体并 readback；任一步失败返回空 Image
rd::test::Image renderCube(rd::Backend b) {
  rd::DeviceDesc desc;
  desc.backend = b;
  auto device = rd::createDevice(desc);
  if (!device) return {};
  auto target = device->createOffscreenTarget({kW, kH});
  rd::demo::CubeScene cube;
  auto code = rd::test::loadCubeShaderCode(b, RD_SHADER_DIR);
  bool ready = target.valid() &&
               cube.init(*device, code.vs.data(), code.vs.size(), code.fs.data(),
                         code.fs.size(), code.entry.c_str());
  if (!ready) return {};
  cube.render(*device, target, kW, kH, kAngle);
  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(static_cast<size_t>(kW) * kH * 4);
  bool ok = device->readbackTarget(target, img.pixels.data(), img.pixels.size());
  cube.shutdown(*device);
  device->destroyTarget(target);
  if (!ok) return {};
  return img;
}

// 与 golden 比较；RD_UPDATE_GOLDENS 环境变量存在时改为更新基准并跳过
void expectMatchesGolden(rd::Backend b, const rd::test::Image& img) {
  ASSERT_FALSE(img.pixels.empty());
  std::string path = goldenPath(b);
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(rd::test::savePNG(path, img.width, img.height, img.pixels.data()));
    GTEST_SKIP() << "golden updated: " << path;
  }
  auto golden = rd::test::loadPNG(path);
  ASSERT_EQ(golden.width, img.width) << "golden 缺失？用 RD_UPDATE_GOLDENS=1 生成";
  ASSERT_EQ(golden.height, img.height);
  auto r = rd::test::compareRGBA8(img.pixels.data(), golden.pixels.data(), img.width,
                                  img.height, /*channelTol=*/3, /*ratioTol=*/0.01);
  EXPECT_TRUE(r.pass) << "diffRatio=" << r.diffRatio << " maxChannelDiff=" << r.maxChannelDiff;
}
} // namespace

// Metal 后端 vs 自身 golden
TEST(Cube, MetalGoldenMatches) {
#if defined(__APPLE__)
  auto img = renderCube(rd::Backend::Metal);
  ASSERT_FALSE(img.pixels.empty()) << "metal 渲染失败";
  expectMatchesGolden(rd::Backend::Metal, img);
#else
  GTEST_SKIP() << "metal 仅 Apple 平台";
#endif
}

// Vulkan 后端 vs 自身 golden
TEST(Cube, VulkanGoldenMatches) {
#if defined(RD_WITH_VULKAN)
  auto img = renderCube(rd::Backend::Vulkan);
  ASSERT_FALSE(img.pixels.empty()) << "vulkan 渲染失败";
  expectMatchesGolden(rd::Backend::Vulkan, img);
#else
  GTEST_SKIP() << "vulkan 未编译";
#endif
}

// 跨后端一致性：Metal 与 Vulkan 渲染同一画面，容差略宽（2%）——
// 保证两后端输出在感知上完全一致（坐标系/绑定约定没有搞错）。
TEST(Cube, CrossBackendConsistent) {
#if defined(__APPLE__) && defined(RD_WITH_VULKAN)
  auto a = renderCube(rd::Backend::Metal);
  auto b = renderCube(rd::Backend::Vulkan);
  ASSERT_FALSE(a.pixels.empty());
  ASSERT_FALSE(b.pixels.empty());
  auto r = rd::test::compareRGBA8(a.pixels.data(), b.pixels.data(), kW, kH, 3, 0.02);
  EXPECT_TRUE(r.pass) << "diffRatio=" << r.diffRatio << " maxChannelDiff=" << r.maxChannelDiff;
#else
  GTEST_SKIP() << "需要双后端";
#endif
}

// 覆盖率检查：非背景像素占比 > 5%（26,26,31 = 清屏色的 8bit 值）——
// 不依赖 golden 的最小 sanity check，防止"渲染成功但画面空白"。
TEST(Cube, CoversScreenArea) {
#if defined(__APPLE__)
  auto img = renderCube(rd::Backend::Metal);
  ASSERT_FALSE(img.pixels.empty());
  uint64_t covered = 0;
  for (size_t i = 0; i < img.pixels.size(); i += 4) {
    if (std::abs(int(img.pixels[i]) - 26) > 8 || std::abs(int(img.pixels[i + 1]) - 26) > 8 ||
        std::abs(int(img.pixels[i + 2]) - 31) > 8)
      ++covered;
  }
  EXPECT_GT(double(covered) / (kW * kH), 0.05);
#else
  GTEST_SKIP();
#endif
}
