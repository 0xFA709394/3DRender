// R16F 渲染目标契约:写入 HDR 颜色(>1.0)→ 采样读出验证精度保留。
#include <gtest/gtest.h>
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <vector>

namespace {
constexpr uint32_t kW = 32, kH = 32;

void runHdr(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  if (!dev->caps().supports(rd::Capability::hdr_render_target))
    GTEST_SKIP() << "后端无 HDR 渲染目标 caps";

  // R16F 纹理 + texture-backed 目标
  rd::TextureDesc td;
  td.format = rd::Format::R16G16B16A16_FLOAT;
  td.width = kW;
  td.height = kH;
  td.usage = rd::TextureUsage::Sampled | rd::TextureUsage::RenderTargetAttachment;
  auto tex = dev->createTexture(td);
  ASSERT_TRUE(tex.valid());
  rd::OffscreenTargetDesc od;
  od.width = kW;
  od.height = kH;
  od.colorFromTexture = tex;
  auto target = dev->createOffscreenTarget(od);
  ASSERT_TRUE(target.valid()) << "R16F 目标创建失败";
  EXPECT_EQ(rd::formatSize(rd::Format::R16G16B16A16_FLOAT), 8u);

  // 采样管线(shadow_sample.vert + hdr_view.frag;slot0 采样)
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "shadow_sample.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "hdr_view.frag");
  auto vsM = dev->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsM = dev->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  ASSERT_TRUE(vsM.valid() && fsM.valid());
  rd::PipelineDesc pd;
  pd.vertexShader = vsM;
  pd.fragmentShader = fsM;
  pd.vertexBindings = {{0, 20}};
  pd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0},
                   {1, rd::Format::R32G32_FLOAT, 12, 0}};
  auto pipe = dev->createPipeline(pd);
  ASSERT_TRUE(pipe.valid());
  auto sampler = dev->createSampler({});
  ASSERT_TRUE(sampler.valid());
  const float quad[6 * 5] = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1,
                             1,  -1, 0, 1, 0, 1, 1,  0, 1, 1, -1, 1, 0, 0, 1};
  auto vbo = dev->createBuffer({sizeof(quad), rd::BufferUsage::Vertex, false, false, quad});
  ASSERT_TRUE(vbo.valid());
  rd::OffscreenTargetDesc cod;
  cod.width = kW;
  cod.height = kH;
  auto out = dev->createOffscreenTarget(cod);
  ASSERT_TRUE(out.valid());

  // pass1:清屏 HDR 颜色 (1.5, 0.5, 0.25) 到 R16F 目标
  auto* cmd = dev->acquireCommandBuffer();
  cmd->beginRenderPass(target, {1.5f, 0.5f, 0.25f, 1.0f});
  cmd->endRenderPass();
  // pass2:采样 ×0.5 → 期望 (0.75, 0.25, 0.125)
  cmd->beginRenderPass(out, {0, 0, 0, 1});
  cmd->bindPipeline(pipe);
  cmd->bindVertexBuffer(0, vbo, 0);
  cmd->bindTexture(0, tex, sampler);
  cmd->draw(6, 0);
  cmd->endRenderPass();
  dev->submit(cmd);
  dev->waitIdle();

  std::vector<uint8_t> px(size_t(kW) * kH * 4);
  ASSERT_TRUE(dev->readbackTarget(out, px.data(), px.size()));
  const size_t c = (size_t(kH / 2) * kW + kW / 2) * 4;
  EXPECT_NEAR(int(px[c]), 191, 8) << "R≈0.75";
  EXPECT_NEAR(int(px[c + 1]), 64, 8) << "G≈0.25";
  EXPECT_NEAR(int(px[c + 2]), 32, 8) << "B≈0.125";

  dev->destroyBuffer(vbo);
  dev->destroySampler(sampler);
  dev->destroyTarget(out);
  dev->destroyTarget(target);
  dev->destroyTexture(tex);
  dev->destroyPipeline(pipe);
  dev->destroyShaderModule(vsM);
  dev->destroyShaderModule(fsM);
}
} // namespace

TEST(HdrTarget, Metal) {
#if defined(__APPLE__)
  runHdr(rd::Backend::Metal);
#endif
}
TEST(HdrTarget, Vulkan) {
#if defined(RD_WITH_VULKAN)
  runHdr(rd::Backend::Vulkan);
#endif
}
