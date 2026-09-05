// LoadOp 契约测试:pass1 清屏画绿四边形(z=0.2) → pass2 loadContent 重开,
// 画红四边形(同位置,z=0.5 更远)+ 蓝四边形(右上角,不重叠)。
// load 语义:颜色保留(蓝可见)+ 深度保留(红被绿遮挡 → 中心仍绿)。
// 对照(load=false):pass2 清屏 → 中心红、蓝可见。Metal/Vulkan 双后端。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 128, kH = 128;

// pos xyz + color rgb,stride 24;绿/红覆盖中心 [-0.5,0.5],蓝在右上角
const float kGreen[] = {  // z=0.2(近)
    -0.5f, -0.5f, 0.2f, 0, 1, 0,   0.5f, -0.5f, 0.2f, 0, 1, 0,   0.5f, 0.5f, 0.2f, 0, 1, 0,
    -0.5f, -0.5f, 0.2f, 0, 1, 0,   0.5f, 0.5f, 0.2f, 0, 1, 0,   -0.5f, 0.5f, 0.2f, 0, 1, 0,
};
const float kRed[] = {    // z=0.5(远,pass2 画)
    -0.5f, -0.5f, 0.5f, 1, 0, 0,   0.5f, -0.5f, 0.5f, 1, 0, 0,   0.5f, 0.5f, 0.5f, 1, 0, 0,
    -0.5f, -0.5f, 0.5f, 1, 0, 0,   0.5f, 0.5f, 0.5f, 1, 0, 0,   -0.5f, 0.5f, 0.5f, 1, 0, 0,
};
const float kBlue[] = {   // 右侧全高竖条 [0.55,0.95](与绿不重叠;y 全高避开视口翻转歧义)
    0.55f, -0.95f, 0.9f, 0, 0, 1,   0.95f, -0.95f, 0.9f, 0, 0, 1,   0.95f, 0.95f, 0.9f, 0, 0, 1,
    0.55f, -0.95f, 0.9f, 0, 0, 1,   0.95f, 0.95f, 0.9f, 0, 0, 1,   0.55f, 0.95f, 0.9f, 0, 0, 1,
};

rd::test::Image render(rd::Backend b, bool loadContent) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "depth.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "depth.frag");
  auto vsm = device->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsm = device->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  auto mkVbo = [&](const float* data) {
    return device->createBuffer({sizeof(float) * 36, rd::BufferUsage::Vertex, false, false, data});
  };
  auto vboG = mkVbo(kGreen);
  auto vboR = mkVbo(kRed);
  auto vboB = mkVbo(kBlue);

  rd::PipelineDesc pd;
  pd.vertexShader = vsm;
  pd.fragmentShader = fsm;
  pd.vertexBindings = {{0, 24}};
  pd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0},
                   {1, rd::Format::R32G32B32_FLOAT, 12, 0}};
  pd.depthTest = true;
  pd.depthWrite = true;
  auto pipeline = device->createPipeline(pd);

  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  td.preserveContent = true;  // pass 间持久(MSAA store/深度 store)
  auto target = device->createOffscreenTarget(td);
  if (!pipeline.valid() || !target.valid()) return {};

  auto* cmd = device->acquireCommandBuffer();
  rd::ClearColor clear{0.0f, 0.0f, 0.0f, 1.0f};
  cmd->beginRenderPass(target, clear);          // pass1:清屏画绿
  cmd->bindPipeline(pipeline);
  cmd->bindVertexBuffer(0, vboG, 0);
  cmd->draw(6, 0);
  cmd->endRenderPass();
  cmd->beginRenderPass(target, clear, loadContent);  // pass2:load 重开画红+蓝
  cmd->bindPipeline(pipeline);
  cmd->bindVertexBuffer(0, vboR, 0);
  cmd->draw(6, 0);
  cmd->bindVertexBuffer(0, vboB, 0);
  cmd->draw(6, 0);
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
  return img.pixels[i + 1] > 150 && img.pixels[i] < 60 && img.pixels[i + 2] < 60;
}
bool isRed(const rd::test::Image& img) {
  size_t i = ((kH / 2) * kW + kW / 2) * 4;
  return img.pixels[i] > 150 && img.pixels[i + 1] < 60 && img.pixels[i + 2] < 60;
}
bool isBlue(const rd::test::Image& img) {
  size_t i = (kH / 2 * kW + uint32_t(kW * 0.875f)) * 4;  // NDC x≈0.75,条内
  return img.pixels[i + 2] > 150 && img.pixels[i] < 60 && img.pixels[i + 1] < 60;
}

void runCase(rd::Backend b) {
  auto loadImg = render(b, true);
  ASSERT_EQ(loadImg.pixels.size(), kW * kH * 4);
  EXPECT_TRUE(isGreen(loadImg)) << "load 语义:深度保留,远红被近绿遮挡";
  EXPECT_TRUE(isBlue(loadImg)) << "load 语义:颜色保留 + 新画蓝可见";
  auto clearImg = render(b, false);
  ASSERT_EQ(clearImg.pixels.size(), kW * kH * 4);
  EXPECT_TRUE(isRed(clearImg)) << "对照:非 load 清屏,中心为 pass2 的红";
  EXPECT_TRUE(isBlue(clearImg));
}
} // namespace

TEST(LoadOp, PreserveAcrossPassesMetal) {
#if defined(__APPLE__)
  runCase(rd::Backend::Metal);
#endif
}

TEST(LoadOp, PreserveAcrossPassesVulkan) {
#if defined(RD_WITH_VULKAN)
  runCase(rd::Backend::Vulkan);
#endif
}
