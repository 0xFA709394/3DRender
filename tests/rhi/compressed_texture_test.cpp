// 压缩纹理契约测试:caps 门控的创建/上传/渲染冒烟。
// ASTC 在 Apple Silicon(Metal/MoltenVK)可用;ETC2 host 不可用(预期拒绝)。
// 像素级正确性由 KTX2 golden(Task 19)兜底,本文件只验契约行为与链路不崩。
#include <gtest/gtest.h>
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t kW = 64, kH = 64;

// 伪随机填充(内容无所谓,只验链路)
void fillPixels(uint8_t* p, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) p[i] = uint8_t((i * 37) & 0xFF);
}

void runContract(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);

  // ---- ETC2:无 caps 时创建必须失败 ----
  if (!dev->caps().supports(rd::Capability::texture_compression_etc2)) {
    rd::TextureDesc td;
    td.width = 8;
    td.height = 8;
    td.format = rd::Format::ETC2_RGBA8_UNORM;
    uint8_t data[64] = {};
    td.data = data;
    td.dataSize = sizeof(data);
    EXPECT_FALSE(dev->createTexture(td).valid()) << "ETC2 无 caps 却创建成功";
  }

  // ---- ASTC:按 caps 分支 ----
  rd::TextureDesc td;
  td.width = 8;
  td.height = 8;
  td.mipLevels = 2;  // 8x8(64B) + 4x4(16B) = 80B
  td.format = rd::Format::ASTC_4x4_UNORM;
  uint8_t astcData[80] = {};
  fillPixels(astcData, sizeof(astcData));
  td.data = astcData;
  td.dataSize = sizeof(astcData);
  auto tex = dev->createTexture(td);
  if (!dev->caps().supports(rd::Capability::texture_compression_astc)) {
    EXPECT_FALSE(tex.valid());
    return;  // 无 caps:后续用例无意义
  }
  ASSERT_TRUE(tex.valid());

  // 数据量不足必须失败(2 mip 需 80B,只给 64B)
  {
    rd::TextureDesc bad = td;
    bad.dataSize = 64;
    EXPECT_FALSE(dev->createTexture(bad).valid());
  }

  // ---- 渲染冒烟:离屏目标 + texquad shader 采样 ASTC 纹理 ----
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

  rd::OffscreenTargetDesc od;
  od.width = kW;
  od.height = kH;
  auto target = dev->createOffscreenTarget(od);
  ASSERT_TRUE(target.valid());
  auto sampler = dev->createSampler({});
  ASSERT_TRUE(sampler.valid());

  // 全屏 quad(两个三角形,6 顶点;pos3+uv2 交错,stride 20)
  const float quad[6 * 5] = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1,
                             1,  -1, 0, 1, 0, 1, 1,  0, 1, 1, -1, 1, 0, 0, 1};
  auto vbo = dev->createBuffer({sizeof(quad), rd::BufferUsage::Vertex, false, false, quad});
  ASSERT_TRUE(vbo.valid());

  auto* cmd = dev->acquireCommandBuffer();
  cmd->beginRenderPass(target, {0, 0, 0, 1});
  cmd->bindPipeline(pipe);
  cmd->bindVertexBuffer(0, vbo, 0);
  cmd->bindTexture(0, tex, sampler);
  cmd->draw(6, 0);
  cmd->endRenderPass();
  dev->submit(cmd);
  dev->waitIdle();

  // readback 成功即链路通畅(内容不做像素断言——ASTC 解码由 GPU 保证)
  std::vector<uint8_t> px(size_t(kW) * kH * 4);
  EXPECT_TRUE(dev->readbackTarget(target, px.data(), px.size()));

  dev->destroyBuffer(vbo);
  dev->destroySampler(sampler);
  dev->destroyTarget(target);
  dev->destroyPipeline(pipe);
  dev->destroyShaderModule(vsMod);
  dev->destroyShaderModule(fsMod);
  dev->destroyTexture(tex);
}
} // namespace

TEST(CompressedTexture, Metal) {
#if defined(__APPLE__)
  runContract(rd::Backend::Metal);
#endif
}

TEST(CompressedTexture, Vulkan) {
#if defined(RD_WITH_VULKAN)
  runContract(rd::Backend::Vulkan);
#endif
}
