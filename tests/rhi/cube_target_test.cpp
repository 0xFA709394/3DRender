// cube face 渲染目标契约测试:向 cube 每个 face 清屏不同颜色,
// 再用 texcube 采样 +X 面验证内容正确;并覆盖 generateMipmaps 返回值。
// Metal/Vulkan 双后端;GLES 路径由 Android 构建验证。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kCube = 64;
constexpr uint32_t kW = 64, kH = 64;

// 向六个面分别清屏(r 通道按面序递增:40*(face+1)),供后续采样验证
void renderFaces(rd::Device& dev, rd::TextureHandle tex) {
  for (uint32_t face = 0; face < 6; ++face) {
    rd::OffscreenTargetDesc td;
    td.width = kCube;
    td.height = kCube;
    td.colorFromTexture = tex;
    td.face = face;
    td.mipLevel = 0;
    auto target = dev.createOffscreenTarget(td);
    if (!target.valid()) continue;
    auto* cmd = dev.acquireCommandBuffer();
    cmd->beginRenderPass(target, {float(40 * (face + 1)) / 255.0f, 0.1f, 0.2f, 1.0f});
    cmd->endRenderPass();
    dev.submit(cmd);
    dev.waitIdle();
    dev.destroyTarget(target);
  }
}

// 用 texcube 采样 +X 面(face0)到 2D 目标并 readback
rd::test::Image sampleFace(rd::Device& dev, rd::Backend b, rd::TextureHandle tex,
                           const float* dir4) {
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "texcube.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "texcube.frag");
  auto vsm = dev.createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsm = dev.createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  auto ubo = dev.createBuffer({16, rd::BufferUsage::Uniform, true, false, nullptr});
  dev.updateBuffer(ubo, dir4, 16, 0);
  auto sampler = dev.createSampler({});
  rd::PipelineDesc pd;
  pd.vertexShader = vsm;
  pd.fragmentShader = fsm;
  auto pipeline = dev.createPipeline(pd);
  auto target = dev.createOffscreenTarget({kW, kH});
  if (!pipeline.valid() || !target.valid()) return {};

  auto* cmd = dev.acquireCommandBuffer();
  cmd->beginRenderPass(target, {0, 0, 0, 1});
  cmd->bindPipeline(pipeline);
  cmd->bindUniformBuffer(0, ubo, 0, 16);
  cmd->bindTexture(0, tex, sampler);
  cmd->draw(3, 0);
  cmd->endRenderPass();
  dev.submit(cmd);
  dev.waitIdle();

  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(kW * kH * 4);
  if (!dev.readbackTarget(target, img.pixels.data(), img.pixels.size())) return {};
  return img;
}

void runCase(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  ASSERT_TRUE(dev->caps().supports(rd::Capability::cube_render_target));

  rd::TextureDesc td;
  td.type = rd::TextureType::Cube;
  td.width = kCube;
  td.height = kCube;
  td.mipLevels = 3;  // 多 mip 以便 generateMipmaps 有实际工作
  td.usage = rd::TextureUsage::Sampled | rd::TextureUsage::RenderTargetAttachment;
  auto tex = dev->createTexture(td);
  ASSERT_TRUE(tex.valid());
  renderFaces(*dev, tex);

  const float dir[4] = {1.0f, 0.0f, 0.0f, 0.0f};  // +X 面
  auto img = sampleFace(*dev, b, tex, dir);
  ASSERT_EQ(img.pixels.size(), kW * kH * 4);
  size_t i = ((kH / 2) * kW + kW / 2) * 4;
  EXPECT_NEAR(img.pixels[i], 40, 4);     // face0 清屏 r = 40/255
  EXPECT_NEAR(img.pixels[i + 1], 26, 4); // 0.1*255 ≈ 26
  EXPECT_TRUE(dev->generateMipmaps(tex));
}
} // namespace

TEST(CubeTarget, MetalRenderToFaceAndSample) {
#if defined(__APPLE__)
  runCase(rd::Backend::Metal);
#endif
}

TEST(CubeTarget, VulkanRenderToFaceAndSample) {
#if defined(RD_WITH_VULKAN)
  runCase(rd::Backend::Vulkan);
#endif
}
