// GPU 预滤波契约测试:mip0 采样 ≈ CPU 直接采样原环境(偏心方向);
// 高 mip 方向无关(模糊)。Metal/Vulkan 双后端。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "renderer/environment.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <glm/glm.hpp>

namespace {
constexpr uint32_t kW = 64, kH = 64;

// 用 texcube 管线以给定 dir/lod 采样 cube 到 2D 目标并读回中心像素
std::array<float, 3> sampleCube(rd::Device& dev, rd::Backend b,
                                rd::TextureHandle cube, const glm::vec3& dir, float lod) {
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "texcube.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "texcube.frag");
  auto vsm = dev.createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsm = dev.createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  auto ubo = dev.createBuffer({16, rd::BufferUsage::Uniform, true, false, nullptr});
  glm::vec3 nd = glm::normalize(dir);
  float udata[4] = {nd.x, nd.y, nd.z, lod};
  dev.updateBuffer(ubo, udata, sizeof(udata), 0);
  auto sampler = dev.createSampler({});
  rd::PipelineDesc pd;
  pd.vertexShader = vsm;
  pd.fragmentShader = fsm;
  auto pipeline = dev.createPipeline(pd);
  auto target = dev.createOffscreenTarget({kW, kH});
  if (!pipeline.valid() || !target.valid()) return {0, 0, 0};
  auto* cmd = dev.acquireCommandBuffer();
  cmd->beginRenderPass(target, {0, 0, 0, 1});
  cmd->bindPipeline(pipeline);
  cmd->bindUniformBuffer(0, ubo, 0, 16);
  cmd->bindTexture(0, cube, sampler);
  cmd->draw(3, 0);
  cmd->endRenderPass();
  dev.submit(cmd);
  dev.waitIdle();
  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(kW * kH * 4);
  if (!dev.readbackTarget(target, img.pixels.data(), img.pixels.size())) return {0, 0, 0};
  size_t i = ((kH / 2) * kW + kW / 2) * 4;
  return {img.pixels[i] / 255.0f, img.pixels[i + 1] / 255.0f, img.pixels[i + 2] / 255.0f};
}

void runCase(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  auto pfVs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "prefilter.vert");
  auto pfFs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "prefilter.frag");
  rd::renderer::Environment env;
  ASSERT_TRUE(env.build(*dev, pfVs.code, pfFs.code, pfVs.entry, rd::Format::RGBA8_UNORM));

  // mip0(roughness≈0):GPU 预滤波 ≈ CPU 直采原环境(偏心方向,非面心)
  glm::vec3 dirA = glm::normalize(glm::vec3(1.0f, 0.3f, -0.2f));
  auto gpuLow = sampleCube(*dev, b, env.prefilterCube(), dirA, 0.0f);
  auto cpuRef = rd::renderer::sampleEnv(env.cubemap(), dirA.x, dirA.y, dirA.z);
  for (int c = 0; c < 3; ++c) EXPECT_NEAR(gpuLow[c], cpuRef[c], 0.10f);

  // 高 mip(roughness=1):两个相反方向采样结果接近(方向无关/模糊)
  auto hi1 = sampleCube(*dev, b, env.prefilterCube(), dirA, 4.0f);
  auto hi2 = sampleCube(*dev, b, env.prefilterCube(), -dirA, 4.0f);
  for (int c = 0; c < 3; ++c) EXPECT_NEAR(hi1[c], hi2[c], 0.10f);
  env.destroy(*dev);
}
} // namespace

TEST(Prefilter, MetalGpuMatchesCpu) {
#if defined(__APPLE__)
  runCase(rd::Backend::Metal);
#endif
}

TEST(Prefilter, VulkanGpuMatchesCpu) {
#if defined(RD_WITH_VULKAN)
  runCase(rd::Backend::Vulkan);
#endif
}
