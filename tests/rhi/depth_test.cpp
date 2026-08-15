// 深度附件契约测试:两个同位置交叠四边形,绿(z=0.2)先画、红(z=0.5)后画。
// depthTest 开(Less,清屏深度 1.0)→ 中心为绿(近者胜);
// depthTest 关 → 中心为红(后画覆盖)。Metal/Vulkan 双后端。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 128, kH = 128;

// 两个覆盖半屏的四边形(各 2 三角形):pos xyz + color rgb,stride 24
const float kGreenQuad[] = {  // z=0.2,绿色
    -0.5f, -0.5f, 0.2f, 0, 1, 0,   0.5f, -0.5f, 0.2f, 0, 1, 0,   0.5f, 0.5f, 0.2f, 0, 1, 0,
    -0.5f, -0.5f, 0.2f, 0, 1, 0,   0.5f, 0.5f, 0.2f, 0, 1, 0,   -0.5f, 0.5f, 0.2f, 0, 1, 0,
};
const float kRedQuad[] = {    // z=0.5,红色
    -0.5f, -0.5f, 0.5f, 1, 0, 0,   0.5f, -0.5f, 0.5f, 1, 0, 0,   0.5f, 0.5f, 0.5f, 1, 0, 0,
    -0.5f, -0.5f, 0.5f, 1, 0, 0,   0.5f, 0.5f, 0.5f, 1, 0, 0,   -0.5f, 0.5f, 0.5f, 1, 0, 0,
};

rd::test::Image render(rd::Backend b, bool depthTest) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "depth.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "depth.frag");
  auto vsm = device->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsm = device->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  auto vboG = device->createBuffer(
      {sizeof(kGreenQuad), rd::BufferUsage::Vertex, false, false, kGreenQuad});
  auto vboR = device->createBuffer(
      {sizeof(kRedQuad), rd::BufferUsage::Vertex, false, false, kRedQuad});

  rd::PipelineDesc pd;
  pd.vertexShader = vsm;
  pd.fragmentShader = fsm;
  pd.vertexBindings = {{0, 24}};
  pd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0},
                   {1, rd::Format::R32G32B32_FLOAT, 12, 0}};
  pd.depthTest = depthTest;
  pd.depthWrite = depthTest;
  auto pipeline = device->createPipeline(pd);

  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  if (!pipeline.valid() || !target.valid()) return {};

  auto* cmd = device->acquireCommandBuffer();
  rd::ClearColor clear{0.0f, 0.0f, 0.0f, 1.0f};  // depth 默认 1.0
  cmd->beginRenderPass(target, clear);
  cmd->bindPipeline(pipeline);
  cmd->bindVertexBuffer(0, vboG, 0);
  cmd->draw(6, 0);  // 绿先画(近,z=0.2)
  cmd->bindVertexBuffer(0, vboR, 0);
  cmd->draw(6, 0);  // 红后画(远,z=0.5)
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

bool isGreen(const rd::test::Image& img) {
  size_t i = ((kH / 2) * kW + kW / 2) * 4;
  return img.pixels[i + 1] > 150 && img.pixels[i] < 60;
}
bool isRed(const rd::test::Image& img) {
  size_t i = ((kH / 2) * kW + kW / 2) * 4;
  return img.pixels[i] > 150 && img.pixels[i + 1] < 60;
}

void runCase(rd::Backend b) {
  auto onImg = render(b, true);
  ASSERT_EQ(onImg.pixels.size(), kW * kH * 4);
  EXPECT_TRUE(isGreen(onImg));   // 深度开:近者(绿)胜
  auto offImg = render(b, false);
  ASSERT_EQ(offImg.pixels.size(), kW * kH * 4);
  EXPECT_TRUE(isRed(offImg));    // 深度关:后画(红)覆盖
}
} // namespace

TEST(Depth, MetalOcclusion) {
#if defined(__APPLE__)
  runCase(rd::Backend::Metal);
#endif
}

TEST(Depth, VulkanOcclusion) {
#if defined(RD_WITH_VULKAN)
  runCase(rd::Backend::Vulkan);
#endif
}
