// 纹理采样渲染测试：全屏四边形采样棋盘格纹理 → readback → golden 比较。
//
// 覆盖：纹理上传、binding（texture slot 0 ↔ 各后端映射）、采样器过滤、mip 选择。
// golden 更新方式同 cube_test：RD_UPDATE_GOLDENS=1 重新生成基准 PNG。
#include <gtest/gtest.h>
#include <cstdlib>
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 256, kH = 256;

// 64x64 棋盘格：红/绿 8x8 块
void makeChecker(uint8_t* out, uint32_t size, uint32_t block) {
  for (uint32_t y = 0; y < size; ++y)
    for (uint32_t x = 0; x < size; ++x) {
      bool green = ((x / block) + (y / block)) % 2 == 0;
      uint8_t* p = out + (y * size + x) * 4;
      p[0] = green ? 20 : 200;
      p[1] = green ? 160 : 30;
      p[2] = 40;
      p[3] = 255;
    }
}

// 公共尾部：texquad pipeline + 全屏三角带 + readback（顶点数据/纹理/采样器由调用方给）
rd::test::Image drawAndReadback(rd::Device& device, rd::BufferHandle vbo,
                                rd::TextureHandle tex, rd::SamplerHandle sampler) {
  auto codeVs = rd::test::loadShaderCode(device.backend(), RD_SHADER_DIR, "texquad.vert");
  auto codeFs = rd::test::loadShaderCode(device.backend(), RD_SHADER_DIR, "texquad.frag");
  rd::ShaderModuleDesc vsd{rd::ShaderStage::Vertex, codeVs.code, codeVs.entry};
  rd::ShaderModuleDesc fsd{rd::ShaderStage::Fragment, codeFs.code, codeFs.entry};
  auto vs = device.createShaderModule(vsd);
  auto fs = device.createShaderModule(fsd);
  rd::PipelineDesc pd;
  pd.vertexShader = vs;
  pd.fragmentShader = fs;
  pd.topology = rd::PrimitiveTopology::TriangleStrip; // 4 顶点全屏三角带
  // 顶点布局：stride 20 = pos(3f)@0 + uv(2f)@12
  pd.vertexBindings = {{0, 20}};
  pd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0},
                   {1, rd::Format::R32G32_FLOAT, 12, 0}};
  auto pipeline = device.createPipeline(pd);
  auto target = device.createOffscreenTarget({kW, kH});
  if (!pipeline.valid() || !target.valid()) return {};

  auto* cmd = device.acquireCommandBuffer();
  cmd->beginRenderPass(target, {0, 0, 0, 1});
  cmd->bindPipeline(pipeline);
  cmd->bindTexture(0, tex, sampler);  // texture slot 0 ↔ 各后端映射（见 rhi_types.h 约定）
  cmd->bindVertexBuffer(0, vbo, 0);
  cmd->draw(4, 0);
  cmd->endRenderPass();
  device.submit(cmd);
  device.waitIdle();

  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(size_t(kW) * kH * 4);
  if (!device.readbackTarget(target, img.pixels.data(), img.pixels.size())) return {};
  return img;
}

// 基本路径：64x64 棋盘格纹理 + 默认采样器（Linear/Repeat），UV∈[0,1] 全屏四边形
rd::test::Image renderTexQuad(rd::Backend b) {
  rd::DeviceDesc desc;
  desc.backend = b;
  auto device = rd::createDevice(desc);
  if (!device) return {};

  // 全屏三角带：pos(3f) + uv(2f)
  const float verts[] = {-1, -1, 0, 0, 1,  -1, 1, 0, 0, 0,  1, -1, 0, 1, 1,  1, 1, 0, 1, 0};
  auto vbo = device->createBuffer({sizeof(verts), rd::BufferUsage::Vertex, verts});

  uint8_t checker[64 * 64 * 4];
  makeChecker(checker, 64, 8);
  rd::TextureDesc td;
  td.width = 64;
  td.height = 64;
  td.data = checker;
  td.dataSize = sizeof(checker);
  auto tex = device->createTexture(td);
  auto sampler = device->createSampler({});
  if (!vbo.valid() || !tex.valid() || !sampler.valid()) return {};
  return drawAndReadback(*device, vbo, tex, sampler);
}

// 3 级 mip 纯色纹理 + 全 nearest 采样器；UV 跨度 32（Repeat）→ 64px 纹理缩 32 倍，
// LOD=3 钳到 level 2（蓝）。若 sampler maxLod 被错误钳 0 会采到 mip0（红）。
rd::test::Image renderMipQuad(rd::Backend b) {
  rd::DeviceDesc desc;
  desc.backend = b;
  auto device = rd::createDevice(desc);
  if (!device) return {};

  // UV 跨度 32：强制大幅缩小，触发高 LOD mip 选择
  const float verts[] = {-1, -1, 0, 0, 32,  -1, 1, 0, 0, 0,
                         1, -1, 0, 32, 32,  1, 1, 0, 32, 0};
  auto vbo = device->createBuffer({sizeof(verts), rd::BufferUsage::Vertex, verts});

  // 三级 mip 各填纯色（红/绿/蓝），数据按 mip 逐级紧凑排列
  uint8_t data[64 * 64 * 4 + 32 * 32 * 4 + 16 * 16 * 4];
  auto fill = [](uint8_t* p, uint32_t px, uint8_t r, uint8_t g, uint8_t b) {
    for (uint32_t i = 0; i < px; ++i) {
      p[i * 4 + 0] = r;
      p[i * 4 + 1] = g;
      p[i * 4 + 2] = b;
      p[i * 4 + 3] = 255;
    }
  };
  fill(data, 64 * 64, 200, 30, 40);                             // mip0 红
  fill(data + 64 * 64 * 4, 32 * 32, 20, 160, 40);               // mip1 绿
  fill(data + 64 * 64 * 4 + 32 * 32 * 4, 16 * 16, 30, 40, 200); // mip2 蓝
  rd::TextureDesc td;
  td.width = 64;
  td.height = 64;
  td.mipLevels = 3;
  td.data = data;
  td.dataSize = sizeof(data);
  auto tex = device->createTexture(td);

  rd::SamplerDesc sd;
  sd.minFilter = rd::Filter::Nearest;
  sd.magFilter = rd::Filter::Nearest;
  sd.mipFilter = rd::Filter::Nearest;
  auto sampler = device->createSampler(sd);
  if (!vbo.valid() || !tex.valid() || !sampler.valid()) return {};
  return drawAndReadback(*device, vbo, tex, sampler);
}

// 断言画面中心像素为 mip2 的蓝（容差 ±3/通道）
void expectMip2Blue(const rd::test::Image& img) {
  ASSERT_FALSE(img.pixels.empty());
  const uint8_t* c = img.pixels.data() + (size_t(kH / 2) * kW + kW / 2) * 4;
  EXPECT_NEAR(int(c[0]), 30, 3) << "R 应来自 mip2（蓝）";
  EXPECT_NEAR(int(c[1]), 40, 3) << "G 应来自 mip2（蓝）";
  EXPECT_NEAR(int(c[2]), 200, 3) << "B 应来自 mip2（蓝）";
}
} // namespace

// Metal 棋盘格 golden
TEST(TextureQuad, MetalGolden) {
#if defined(__APPLE__)
  auto img = renderTexQuad(rd::Backend::Metal);
  ASSERT_FALSE(img.pixels.empty());
  std::string path = std::string(RD_TEST_DATA_DIR) + "/golden/texquad_metal.png";
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(rd::test::savePNG(path, img.width, img.height, img.pixels.data()));
    GTEST_SKIP() << "golden updated";
  }
  auto golden = rd::test::loadPNG(path);
  ASSERT_EQ(golden.width, img.width) << "RD_UPDATE_GOLDENS=1 先生成";
  auto r = rd::test::compareRGBA8(img.pixels.data(), golden.pixels.data(), img.width,
                                  img.height, 3, 0.01);
  EXPECT_TRUE(r.pass) << "diffRatio=" << r.diffRatio;
#endif
}

// Vulkan 棋盘格 golden
TEST(TextureQuad, VulkanGolden) {
#if defined(RD_WITH_VULKAN)
  auto img = renderTexQuad(rd::Backend::Vulkan);
  ASSERT_FALSE(img.pixels.empty());
  std::string path = std::string(RD_TEST_DATA_DIR) + "/golden/texquad_vulkan.png";
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(rd::test::savePNG(path, img.width, img.height, img.pixels.data()));
    GTEST_SKIP() << "golden updated";
  }
  auto golden = rd::test::loadPNG(path);
  ASSERT_EQ(golden.width, img.width) << "RD_UPDATE_GOLDENS=1 先生成";
  auto r = rd::test::compareRGBA8(img.pixels.data(), golden.pixels.data(), img.width,
                                  img.height, 3, 0.01);
  EXPECT_TRUE(r.pass) << "diffRatio=" << r.diffRatio;
#endif
}

// Metal mip 选择（期望采到 mip2 蓝）
TEST(TextureQuad, MetalMipSelection) {
#if defined(__APPLE__)
  expectMip2Blue(renderMipQuad(rd::Backend::Metal));
#endif
}

// Vulkan mip 选择
TEST(TextureQuad, VulkanMipSelection) {
#if defined(RD_WITH_VULKAN)
  expectMip2Blue(renderMipQuad(rd::Backend::Vulkan));
#endif
}

// 跨后端一致性（容差 2%，同 cube 跨后端用例）
TEST(TextureQuad, CrossBackend) {
#if defined(__APPLE__) && defined(RD_WITH_VULKAN)
  auto a = renderTexQuad(rd::Backend::Metal);
  auto b = renderTexQuad(rd::Backend::Vulkan);
  ASSERT_FALSE(a.pixels.empty());
  ASSERT_FALSE(b.pixels.empty());
  auto r = rd::test::compareRGBA8(a.pixels.data(), b.pixels.data(), kW, kH, 3, 0.02);
  EXPECT_TRUE(r.pass) << "diffRatio=" << r.diffRatio;
#endif
}
