// instancing 契约测试:per-instance 偏移把同一小三角形画到四个象限,
// readback 验证各象限中心为红、画面中心为背景色。Metal/Vulkan 双后端。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 128, kH = 128;

rd::test::Image render(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "inst.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "inst.frag");
  auto vsm = device->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsm = device->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});

  const float tri[] = {-0.15f, -0.15f, 0.15f, -0.15f, 0.0f, 0.15f};  // 小三角形
  const float offs[] = {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f};  // 4 实例偏移
  auto vbo = device->createBuffer({sizeof(tri), rd::BufferUsage::Vertex, false, false, tri});
  auto ibo = device->createBuffer({sizeof(offs), rd::BufferUsage::Vertex, false, false, offs});

  rd::PipelineDesc pd;
  pd.vertexShader = vsm;
  pd.fragmentShader = fsm;
  pd.vertexBindings = {{0, 8, rd::VertexStepRate::Vertex}, {1, 8, rd::VertexStepRate::Instance}};
  pd.attributes = {{0, rd::Format::R32G32_FLOAT, 0, 0}, {1, rd::Format::R32G32_FLOAT, 0, 1}};
  auto pipeline = device->createPipeline(pd);
  auto target = device->createOffscreenTarget({kW, kH});
  if (!pipeline.valid() || !target.valid() || !vbo.valid() || !ibo.valid()) return {};

  auto* cmd = device->acquireCommandBuffer();
  cmd->beginRenderPass(target, {0.0f, 0.0f, 0.0f, 1.0f});
  cmd->bindPipeline(pipeline);
  cmd->bindVertexBuffer(0, vbo, 0);
  cmd->bindVertexBuffer(1, ibo, 0);
  cmd->drawInstanced(3, 0, 4, 0);
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

bool isRed(const rd::test::Image& img, uint32_t x, uint32_t y) {
  size_t i = (size_t(y) * img.width + x) * 4;
  return img.pixels[i] > 150 && img.pixels[i + 1] < 60;
}
} // namespace

TEST(Instancing, MetalFourInstances) {
#if defined(__APPLE__)
  auto img = render(rd::Backend::Metal);
  ASSERT_EQ(img.pixels.size(), kW * kH * 4);
  // NDC(±0.5,±0.5) → 像素(32,96),(96,96),(32,32),(96,32)(顶向下行优先)
  EXPECT_TRUE(isRed(img, 96, 32));   // (+0.5,+0.5) → 右下
  EXPECT_TRUE(isRed(img, 32, 32));   // (-0.5,+0.5) → 左下
  EXPECT_TRUE(isRed(img, 96, 96));   // (+0.5,-0.5) → 右上
  EXPECT_TRUE(isRed(img, 32, 96));   // (-0.5,-0.5) → 左上
  EXPECT_FALSE(isRed(img, 64, 64));  // 中心为背景
#endif
}

TEST(Instancing, VulkanFourInstances) {
#if defined(RD_WITH_VULKAN)
  auto img = render(rd::Backend::Vulkan);
  ASSERT_EQ(img.pixels.size(), kW * kH * 4);
  EXPECT_TRUE(isRed(img, 96, 32));
  EXPECT_TRUE(isRed(img, 32, 96));
  EXPECT_FALSE(isRed(img, 64, 64));
#endif
}
