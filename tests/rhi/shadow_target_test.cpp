// depth-only 渲染目标契约:渲三角形到深度纹理 → 比较采样器采样 → 前后景灰度正确。
#include <gtest/gtest.h>
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <glm/glm.hpp>
#include <vector>

namespace {
constexpr uint32_t kW = 64, kH = 64;

void runShadowTarget(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  if (!dev->caps().supports(rd::Capability::depth_texture)) GTEST_SKIP() << "无深度纹理";

  // ---- 资源(全部在 acquireCommandBuffer 之前)----
  rd::TextureDesc tdd;
  tdd.format = rd::Format::D32_FLOAT;
  tdd.width = kW;
  tdd.height = kH;
  tdd.usage = rd::TextureUsage::Sampled | rd::TextureUsage::RenderTargetAttachment;
  auto depthTex = dev->createTexture(tdd);
  ASSERT_TRUE(depthTex.valid());
  rd::OffscreenTargetDesc od;
  od.width = kW;
  od.height = kH;
  od.depthFromTexture = depthTex;
  auto shadowTarget = dev->createOffscreenTarget(od);
  ASSERT_TRUE(shadowTarget.valid()) << "depth-only 目标创建失败";
  uint32_t w = 0, h = 0;
  dev->targetSize(shadowTarget, w, h);
  EXPECT_EQ(w, kW);
  EXPECT_EQ(h, kH);
  EXPECT_TRUE(dev->targetColorTexture(shadowTarget).valid());  // 返回深度纹理
  // readback 深度目标 → false
  std::vector<uint8_t> junk(16);
  EXPECT_FALSE(dev->readbackTarget(shadowTarget, junk.data(), junk.size()));

  auto load = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
  auto smpVs = load("shadow_sample.vert"), smpFs = load("shadow_sample.frag");
  auto sdVsM = dev->createShaderModule({rd::ShaderStage::Vertex, sdVs.code, sdVs.entry});
  auto sdFsM = dev->createShaderModule({rd::ShaderStage::Fragment, sdFs.code, sdFs.entry});
  auto smpVsM = dev->createShaderModule({rd::ShaderStage::Vertex, smpVs.code, smpVs.entry});
  auto smpFsM = dev->createShaderModule({rd::ShaderStage::Fragment, smpFs.code, smpFs.entry});
  ASSERT_TRUE(sdVsM.valid() && sdFsM.valid() && smpVsM.valid() && smpFsM.valid());

  // 深度写入管线(depthOnly)
  rd::PipelineDesc dpd;
  dpd.vertexShader = sdVsM;
  dpd.fragmentShader = sdFsM;
  dpd.vertexBindings = {{0, 12}};
  dpd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0}};
  dpd.depthOnly = true;
  dpd.depthTest = true;
  dpd.depthWrite = true;
  auto depthPipe = dev->createPipeline(dpd);
  ASSERT_TRUE(depthPipe.valid()) << "depthOnly 管线创建失败";

  // 采样管线(普通离屏目标)
  rd::PipelineDesc spd;
  spd.vertexShader = smpVsM;
  spd.fragmentShader = smpFsM;
  spd.vertexBindings = {{0, 20}};
  spd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0},
                    {1, rd::Format::R32G32_FLOAT, 12, 0}};
  auto samplePipe = dev->createPipeline(spd);
  ASSERT_TRUE(samplePipe.valid());

  // ShadowUBO(lightViewProj=单位)+ ItemUBO(mvp/world/normalMatrix=单位)双 UBO
  glm::mat4 mvp(1.0f);
  auto ubo = dev->createBuffer({64, rd::BufferUsage::Uniform, true, false, nullptr});
  ASSERT_TRUE(ubo.valid());
  dev->updateBuffer(ubo, &mvp, 64, 0);
  float itemData[64] = {};
  for (int m = 0; m < 3; ++m)
    for (int k = 0; k < 16; ++k) itemData[m * 16 + k] = (k % 5 == 0) ? 1.0f : 0.0f;
  itemData[48] = itemData[49] = itemData[50] = itemData[51] = 1.0f;  // baseColorFactor
  auto itemUbo = dev->createBuffer({256, rd::BufferUsage::Uniform, true, false, nullptr});
  ASSERT_TRUE(itemUbo.valid());
  dev->updateBuffer(itemUbo, itemData, 256, 0);
  const float tri[3 * 3] = {-1, -1, 0.4f, 0, -1, 0.4f, -1, 1, 0.4f};  // 左下三角
  auto vbo = dev->createBuffer({sizeof(tri), rd::BufferUsage::Vertex, false, false, tri});
  const float quad[6 * 5] = {-1, -1, 0, 0, 1, 1, -1, 0, 1, 1, -1, 1, 0, 0, 0,
                             1,  -1, 0, 1, 1, 1, 1,  0, 1, 0, -1, 1, 0, 0, 0};
  auto quadVbo = dev->createBuffer({sizeof(quad), rd::BufferUsage::Vertex, false, false, quad});
  ASSERT_TRUE(vbo.valid() && quadVbo.valid());
  rd::SamplerDesc csd;
  csd.compareEnable = true;
  auto cmpSampler = dev->createSampler(csd);
  ASSERT_TRUE(cmpSampler.valid());
  rd::OffscreenTargetDesc cod;
  cod.width = kW;
  cod.height = kH;
  auto colorTarget = dev->createOffscreenTarget(cod);
  ASSERT_TRUE(colorTarget.valid());

  // ---- pass1:三角形深度写入 shadowTarget ----
  auto* cmd = dev->acquireCommandBuffer();
  cmd->beginRenderPass(shadowTarget, {0, 0, 0, 1, 1.0f});  // clear.depth=1
  cmd->bindPipeline(depthPipe);
  cmd->bindVertexBuffer(0, vbo, 0);
  cmd->bindUniformBuffer(0, ubo, 0, 64);
  cmd->bindUniformBuffer(1, itemUbo, 0, 256);
  cmd->draw(3, 0);
  cmd->endRenderPass();
  // ---- pass2:比较采样到 colorTarget ----
  cmd->beginRenderPass(colorTarget, {0, 0, 0, 1});
  cmd->bindPipeline(samplePipe);
  cmd->bindVertexBuffer(0, quadVbo, 0);
  cmd->bindTexture(0, depthTex, cmpSampler);
  cmd->draw(6, 0);
  cmd->endRenderPass();
  dev->submit(cmd);
  dev->waitIdle();

  std::vector<uint8_t> px(size_t(kW) * kH * 4);
  ASSERT_TRUE(dev->readbackTarget(colorTarget, px.data(), px.size()));
  // 三角形覆盖区(左下):深度 0.4 < 0.5 → 受光(≈255);右上(未覆盖,远平面 1.0)→ 阴影(0)
  const auto at = [&](uint32_t x, uint32_t y) { return px[(size_t(y) * kW + x) * 4]; };
  EXPECT_LT(at(kW / 8, kH * 7 / 8), 60u) << "三角形覆盖区应阴影(0.4<0.5 遮挡)";
  EXPECT_GT(at(kW * 7 / 8, kH / 8), 200u) << "未覆盖区应受光";

  dev->destroySampler(cmpSampler);
  dev->destroyBuffer(ubo);
  dev->destroyBuffer(itemUbo);
  dev->destroyBuffer(vbo);
  dev->destroyBuffer(quadVbo);
  dev->destroyTarget(colorTarget);
  dev->destroyTarget(shadowTarget);
  dev->destroyTexture(depthTex);
  dev->destroyPipeline(depthPipe);
  dev->destroyPipeline(samplePipe);
  dev->destroyShaderModule(sdVsM);
  dev->destroyShaderModule(sdFsM);
  dev->destroyShaderModule(smpVsM);
  dev->destroyShaderModule(smpFsM);
}
} // namespace

TEST(ShadowTarget, Metal) {
#if defined(__APPLE__)
  runShadowTarget(rd::Backend::Metal);
#endif
}
TEST(ShadowTarget, Vulkan) {
#if defined(RD_WITH_VULKAN)
  runShadowTarget(rd::Backend::Vulkan);
#endif
}
