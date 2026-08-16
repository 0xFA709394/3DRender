// 目标可采样化契约:渲染到目标 A(清屏红色)→ 把 A 的颜色当纹理采样画到目标 B
// → readback B 验红色。覆盖 targetColorTexture/targetSize 接口。
#include <gtest/gtest.h>
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <vector>

namespace {
constexpr uint32_t kW = 64, kH = 64;

void runRoundtrip(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);

  rd::OffscreenTargetDesc od;
  od.width = kW;
  od.height = kH;
  auto targetA = dev->createOffscreenTarget(od);
  auto targetB = dev->createOffscreenTarget(od);
  ASSERT_TRUE(targetA.valid() && targetB.valid());

  // targetSize 接口
  uint32_t w = 0, h = 0;
  dev->targetSize(targetA, w, h);
  EXPECT_EQ(w, kW);
  EXPECT_EQ(h, kH);

  // 全部资源须在 acquireCommandBuffer 之前创建(Vulkan 后端上传路径会重置
  // 共享命令缓冲,acquire 后创建带数据资源会冲掉已录制命令)
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "texquad.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "texquad.frag");
  auto vsMod = dev->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsMod = dev->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  ASSERT_TRUE(vsMod.valid() && fsMod.valid());
  rd::PipelineDesc pd;
  pd.vertexShader = vsMod;
  pd.fragmentShader = fsMod;
  // texquad 顶点布局(与 shaders/texquad.vert 一致):pos3@0|uv2@12,binding0 stride20
  pd.vertexBindings = {{0, 20}};
  pd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0},
                   {1, rd::Format::R32G32_FLOAT, 12, 0}};
  auto pipe = dev->createPipeline(pd);
  ASSERT_TRUE(pipe.valid());
  auto sampler = dev->createSampler({});
  ASSERT_TRUE(sampler.valid());
  rd::TextureHandle sceneTex = dev->targetColorTexture(targetA);
  ASSERT_TRUE(sceneTex.valid()) << "targetColorTexture 返回无效句柄";

  const float quad[6 * 5] = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1,
                             1,  -1, 0, 1, 0, 1, 1,  0, 1, 1, -1, 1, 0, 0, 1};
  auto vbo = dev->createBuffer({sizeof(quad), rd::BufferUsage::Vertex, false, false, quad});
  ASSERT_TRUE(vbo.valid());

  // pass1:清屏红色到 A;pass2:采样 A 画全屏 quad 到 B(同一命令缓冲顺序执行)
  auto* cmd = dev->acquireCommandBuffer();
  cmd->beginRenderPass(targetA, {1, 0, 0, 1});
  cmd->endRenderPass();
  cmd->beginRenderPass(targetB, {0, 0, 0, 1});
  cmd->bindPipeline(pipe);
  cmd->bindVertexBuffer(0, vbo, 0);
  cmd->bindTexture(0, sceneTex, sampler);
  cmd->draw(6, 0);
  cmd->endRenderPass();
  dev->submit(cmd);
  dev->waitIdle();

  std::vector<uint8_t> px(size_t(kW) * kH * 4);
  ASSERT_TRUE(dev->readbackTarget(targetB, px.data(), px.size()));
  // 中心像素应为红(允许采样/格式微差)
  const size_t c = (size_t(kH / 2) * kW + kW / 2) * 4;
  EXPECT_GT(px[c], 200u) << "R";
  EXPECT_LT(px[c + 1], 60u) << "G";
  EXPECT_LT(px[c + 2], 60u) << "B";

  dev->destroyBuffer(vbo);
  dev->destroySampler(sampler);
  dev->destroyPipeline(pipe);
  dev->destroyShaderModule(vsMod);
  dev->destroyShaderModule(fsMod);
  dev->destroyTarget(targetA);
  dev->destroyTarget(targetB);
}
} // namespace

TEST(TargetTexture, Metal) {
#if defined(__APPLE__)
  runRoundtrip(rd::Backend::Metal);
#endif
}
TEST(TargetTexture, Vulkan) {
#if defined(RD_WITH_VULKAN)
  runRoundtrip(rd::Backend::Vulkan);
#endif
}
