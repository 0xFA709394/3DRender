// MSAA 契约:sampleCount=4 目标渲染斜切 quad,边缘应出现中间灰阶(resolve 后);
// 同时验证 targetColorTexture 返回 resolve 纹理、超 caps 拒绝。
#include <gtest/gtest.h>
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <algorithm>
#include <set>
#include <vector>

namespace {
constexpr uint32_t kW = 128, kH = 128;

void runMsaa(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  if (dev->caps().get(rd::Capability::msaa) < 4) {
    GTEST_SKIP() << "后端 MSAA<4";
  }

  // 全部资源在 acquireCommandBuffer 之前创建(Vulkan 上传路径重置共享命令缓冲)
  auto loadShader = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto vs = loadShader("texquad.vert");
  auto fs = loadShader("texquad.frag");
  auto vsMod = dev->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsMod = dev->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  ASSERT_TRUE(vsMod.valid() && fsMod.valid());

  rd::PipelineDesc pd;
  pd.vertexShader = vsMod;
  pd.fragmentShader = fsMod;
  pd.vertexBindings = {{0, 20}};
  pd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0},
                   {1, rd::Format::R32G32_FLOAT, 12, 0}};
  pd.sampleCount = 4;
  auto pipe4 = dev->createPipeline(pd);
  ASSERT_TRUE(pipe4.valid()) << "sampleCount=4 管线创建失败";

  rd::OffscreenTargetDesc od;
  od.width = kW;
  od.height = kH;
  od.sampleCount = 4;
  auto target = dev->createOffscreenTarget(od);
  ASSERT_TRUE(target.valid()) << "sampleCount=4 目标创建失败";

  // resolve 纹理可查询
  EXPECT_TRUE(dev->targetColorTexture(target).valid());

  // 斜切 quad(覆盖左下半屏,斜边即 MSAA 采样区);绑 1x1 白纹理保证确定性
  uint8_t white[4] = {255, 255, 255, 255};
  auto white1 = dev->createTexture({rd::TextureType::Texture2D, rd::TextureUsage::Sampled, 1, 1,
                                    rd::Format::RGBA8_UNORM, 1, white, 4});
  ASSERT_TRUE(white1.valid());
  auto sampler = dev->createSampler({});
  ASSERT_TRUE(sampler.valid());
  const float quad[6 * 5] = {-1, -1, 0, 0, 0, 1, 1, 0, 1, 0, -1, 1, 0, 0, 1,
                             -1, -1, 0, 0, 0, 1, 1, 0, 1, 1, -1, 1, 0, 0, 1};
  auto vbo = dev->createBuffer({sizeof(quad), rd::BufferUsage::Vertex, false, false, quad});
  ASSERT_TRUE(vbo.valid());

  auto* cmd = dev->acquireCommandBuffer();
  cmd->beginRenderPass(target, {0, 0, 0, 1});
  cmd->bindPipeline(pipe4);
  cmd->bindVertexBuffer(0, vbo, 0);
  cmd->bindTexture(0, white1, sampler);
  cmd->draw(6, 0);
  cmd->endRenderPass();
  dev->submit(cmd);
  dev->waitIdle();

  std::vector<uint8_t> px(size_t(kW) * kH * 4);
  ASSERT_TRUE(dev->readbackTarget(target, px.data(), px.size()));
  // MSAA 特征:斜边附近存在中间灰阶(非 0/255)
  std::set<int> levels;
  for (size_t i = 0; i < px.size(); i += 4) levels.insert(px[i]);
  const bool hasMid =
      std::any_of(levels.begin(), levels.end(), [](int v) { return v > 16 && v < 239; });
  EXPECT_TRUE(hasMid) << "无中间灰阶,MSAA 未生效(levels=" << levels.size() << ")";

  dev->destroyBuffer(vbo);
  dev->destroySampler(sampler);
  dev->destroyTexture(white1);
  dev->destroyTarget(target);
  dev->destroyPipeline(pipe4);
  dev->destroyShaderModule(vsMod);
  dev->destroyShaderModule(fsMod);
}

// 非法:sampleCount 超 caps 必须拒绝
void runReject(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  const uint32_t msaa = dev->caps().get(rd::Capability::msaa);
  if (msaa == 0) GTEST_SKIP() << "后端无 MSAA caps";
  rd::OffscreenTargetDesc od;
  od.width = 16;
  od.height = 16;
  od.sampleCount = msaa * 2;
  EXPECT_FALSE(dev->createOffscreenTarget(od).valid());
}
} // namespace

#if 0  // Task 5(Metal MSAA)完成后启用
TEST(Msaa, Metal) {
#if defined(__APPLE__)
  runMsaa(rd::Backend::Metal);
  runReject(rd::Backend::Metal);
#endif
}
#endif

TEST(Msaa, Vulkan) {
#if defined(RD_WITH_VULKAN)
  runMsaa(rd::Backend::Vulkan);
  runReject(rd::Backend::Vulkan);
#endif
}
