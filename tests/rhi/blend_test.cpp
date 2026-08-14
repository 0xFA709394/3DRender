// blend 契约测试:清屏底色 (0.2,0,0,1) 上画半透明绿 (0,1,0,0.5)。
// blend 开 → (0.1, 0.5, 0);blend 关 → (0, 1, 0) 直接覆盖。Metal/Vulkan 双后端。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 64, kH = 64;

rd::test::Image renderQuad(rd::Backend b, bool blendEnable) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "blend.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "blend.frag");
  auto vsm = device->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsm = device->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  rd::PipelineDesc pd;
  pd.vertexShader = vsm;
  pd.fragmentShader = fsm;
  pd.blend.enable = blendEnable;
  auto pipeline = device->createPipeline(pd);
  auto target = device->createOffscreenTarget({kW, kH});
  if (!pipeline.valid() || !target.valid()) return {};

  auto* cmd = device->acquireCommandBuffer();
  cmd->beginRenderPass(target, {0.2f, 0.0f, 0.0f, 1.0f});
  cmd->bindPipeline(pipeline);
  cmd->draw(3, 0);
  cmd->endRenderPass();
  device->submit(cmd);
  device->waitIdle();

  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(kW * kH * 4);
  if (!device->readbackTarget(target, img.pixels.data(), img.pixels.size())) return {};
  return img;
}

// 中心像素(图像为顶向下行优先)
void expectCenter(const rd::test::Image& img, uint8_t r, uint8_t g, uint8_t b) {
  size_t idx = ((kH / 2) * kW + (kW / 2)) * 4;
  EXPECT_NEAR(img.pixels[idx + 0], r, 3);
  EXPECT_NEAR(img.pixels[idx + 1], g, 3);
  EXPECT_NEAR(img.pixels[idx + 2], b, 3);
}
} // namespace

TEST(Blend, MetalEnable) {
#if defined(__APPLE__)
  auto img = renderQuad(rd::Backend::Metal, true);
  ASSERT_EQ(img.pixels.size(), kW * kH * 4);
  expectCenter(img, 26, 128, 0);  // 0.2*0.5=0.1→26;1.0*0.5→128
#endif
}

TEST(Blend, MetalDisable) {
#if defined(__APPLE__)
  auto img = renderQuad(rd::Backend::Metal, false);
  ASSERT_EQ(img.pixels.size(), kW * kH * 4);
  expectCenter(img, 0, 255, 0);  // 直接覆盖
#endif
}

TEST(Blend, VulkanEnable) {
#if defined(RD_WITH_VULKAN)
  auto img = renderQuad(rd::Backend::Vulkan, true);
  ASSERT_EQ(img.pixels.size(), kW * kH * 4);
  expectCenter(img, 26, 128, 0);
#endif
}
