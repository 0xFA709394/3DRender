// demo 场景 golden(material_balls/cornell_box/light_playground 双后端)
// + 全程序场景冒烟(渲染不崩 + 非背景覆盖率 >3%)。
#include <gtest/gtest.h>
#include "common/golden_test.h"
#include "common/image.h"
#include "common/shader_code.h"
#include "tools/render_test/scenes.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <cstdlib>

namespace {
constexpr uint32_t kW = 512, kH = 512;

// 渲染指定场景 → RGBA8 图像(失败返回空)。
rd::test::Image renderScene(rd::Backend b, const char* name, rd::ModelAsset& storage) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto load = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
  auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
  auto skVs = load("pbr_forward_skinned.vert");
  auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
  auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
  auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
  auto sdsVs = load("shadow_depth_skinned.vert");
  auto exFs = load("bloom_extract.frag");
  auto bbFs = load("bloom_blur.frag");
  auto cpFs = load("composite.frag");
  auto fxFs = load("fxaa.frag");
  auto instVs = load("pbr_forward_instanced.vert");
  auto instFs = load("pbr_forward_instanced.frag");
  rd::Renderer renderer;
  // instanced_field 场景走实例化路径(instanced shader 加载 → 分组生效;golden 应不变)
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code, instVs.code, instFs.code, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, unlitVs.entry, rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  if (!target.valid() || !renderer.init(*device, sd)) return {};
  rd::tool::DemoScene scene;
  if (!rd::tool::buildDemoScene(name, *device, renderer, scene, storage)) return {};
  device->beginFrame();
  renderer.beginScene(scene.camera, {0.05f, 0.05f, 0.06f, 1.0f});
  rd::tool::submitDemoScene(scene, renderer, 1.0f / 60.0f);
  auto* cmd = device->acquireCommandBuffer();
  renderer.endScene(cmd, target);
  device->submit(cmd);
  device->waitIdle();
  device->endFrame();
  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(size_t(kW) * kH * 4);
  device->readbackTarget(target, img.pixels.data(), img.pixels.size());
  rd::tool::destroyDemoScene(scene, *device);
  renderer.shutdown();
  return img;
}

// 冒烟:非背景像素占比(16 步进采样)
double coverage(const rd::test::Image& img) {
  const uint8_t bg[3] = {13, 13, 15};
  uint32_t hit = 0, total = 0;
  for (uint32_t y = 0; y < img.height; y += 16)
    for (uint32_t x = 0; x < img.width; x += 16) {
      const uint8_t* p = img.pixels.data() + (size_t(y) * img.width + x) * 4;
      if (abs(p[0] - bg[0]) + abs(p[1] - bg[1]) + abs(p[2] - bg[2]) > 12) ++hit;
      ++total;
    }
  return double(hit) / double(total);
}

// 每场景渲染包装(宏的 RenderFn 签名)
rd::test::Image renderMaterialBalls(rd::Backend b) {
  rd::ModelAsset storage;
  return renderScene(b, "material_balls", storage);
}
rd::test::Image renderCornell(rd::Backend b) {
  rd::ModelAsset storage;
  return renderScene(b, "cornell_box", storage);
}
rd::test::Image renderLights(rd::Backend b) {
  rd::ModelAsset storage;
  return renderScene(b, "light_playground", storage);
}
rd::test::Image renderBloom(rd::Backend b) {
  rd::ModelAsset storage;
  return renderScene(b, "emissive_bloom", storage);
}
rd::test::Image renderNormalWall(rd::Backend b) {
  rd::ModelAsset storage;
  return renderScene(b, "normal_map_wall", storage);
}
rd::test::Image renderShadowGallery(rd::Backend b) {
  rd::ModelAsset storage;
  return renderScene(b, "shadow_gallery", storage);
}
rd::test::Image renderKtx2Gallery(rd::Backend b) {
  rd::ModelAsset storage;
  return renderScene(b, "ktx2_gallery", storage);
}
rd::test::Image renderAlphaBlend(rd::Backend b) {
  rd::ModelAsset storage;
  return renderScene(b, "alpha_blend", storage);
}
} // namespace



// 全程序场景冒烟(含 skinned_demo/instanced_field;知名 glb 缺失自动 skip)
TEST(DemoScenes, SmokeAll) {
#if defined(__APPLE__)
  if (!getenv("RD_ASSETS_DIR"))
    setenv("RD_ASSETS_DIR", (std::string(RD_TEST_DATA_DIR) + "/../assets").c_str(), 1);
  uint32_t count = 0;
  const char* const* names = rd::tool::demoSceneNames(count);
  for (uint32_t i = 0; i < count; ++i) {
    rd::ModelAsset storage;
    auto img = renderScene(rd::Backend::Metal, names[i], storage);
    if (img.pixels.empty()) continue;  // 资产缺失 skip
    EXPECT_GT(coverage(img), 0.03) << names[i] << " 覆盖率不足";
  }
#endif
}

RD_GOLDEN_TEST(DemoScenes, MaterialBalls, "demo_material", 0.05, renderMaterialBalls)
RD_GOLDEN_TEST(DemoScenes, Cornell, "demo_cornell", 0.05, renderCornell)
RD_GOLDEN_TEST(DemoScenes, Lights, "demo_lights", 0.05, renderLights)
RD_GOLDEN_TEST(DemoScenes, Bloom, "demo_bloom", 0.05, renderBloom)
RD_GOLDEN_TEST(DemoScenes, NormalWall, "demo_normal", 0.05, renderNormalWall)
RD_GOLDEN_TEST(DemoScenes, ShadowGallery, "demo_shadow", 0.05, renderShadowGallery)
RD_GOLDEN_TEST(DemoScenes, Ktx2Gallery, "demo_ktx2", 0.05, renderKtx2Gallery)
RD_GOLDEN_TEST(DemoScenes, AlphaBlend, "demo_blend", 0.05, renderAlphaBlend)
