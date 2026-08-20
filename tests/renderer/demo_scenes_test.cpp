// demo 场景 golden(material_balls/cornell_box/light_playground 双后端)
// + 全程序场景冒烟(渲染不崩 + 非背景覆盖率 >3%)。
#include <gtest/gtest.h>
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
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,
                            unlitVs.entry, rd::Format::RGBA8_UNORM};
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

void runGolden(rd::Backend b, const char* scene, const char* goldenName) {
  rd::ModelAsset storage;
  auto img = renderScene(b, scene, storage);
  ASSERT_FALSE(img.pixels.empty());
  const std::string path = std::string(RD_TEST_DATA_DIR) + "/golden/" + goldenName;
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(rd::test::savePNG(path, img.width, img.height, img.pixels.data()));
    return;
  }
  auto golden = rd::test::loadPNG(path);
  ASSERT_EQ(golden.pixels.size(), img.pixels.size()) << "golden 缺失: " << path;
  auto cmp = rd::test::compareRGBA8(img.pixels.data(), golden.pixels.data(), kW, kH, 3, 0.02);
  EXPECT_TRUE(cmp.pass) << "diffRatio=" << cmp.diffRatio;
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
} // namespace

TEST(DemoScenes, MaterialBallsMetal) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal, "material_balls", "demo_material_metal.png");
#endif
}
TEST(DemoScenes, MaterialBallsVulkan) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan, "material_balls", "demo_material_vulkan.png");
#endif
}
TEST(DemoScenes, CornellMetal) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal, "cornell_box", "demo_cornell_metal.png");
#endif
}
TEST(DemoScenes, CornellVulkan) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan, "cornell_box", "demo_cornell_vulkan.png");
#endif
}
TEST(DemoScenes, LightsMetal) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal, "light_playground", "demo_lights_metal.png");
#endif
}
TEST(DemoScenes, LightsVulkan) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan, "light_playground", "demo_lights_vulkan.png");
#endif
}


TEST(DemoScenes, BloomMetal) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal, "emissive_bloom", "demo_bloom_metal.png");
#endif
}
TEST(DemoScenes, BloomVulkan) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan, "emissive_bloom", "demo_bloom_vulkan.png");
#endif
}
TEST(DemoScenes, NormalWallMetal) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal, "normal_map_wall", "demo_normal_metal.png");
#endif
}
TEST(DemoScenes, NormalWallVulkan) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan, "normal_map_wall", "demo_normal_vulkan.png");
#endif
}
TEST(DemoScenes, ShadowGalleryMetal) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal, "shadow_gallery", "demo_shadow_metal.png");
#endif
}
TEST(DemoScenes, ShadowGalleryVulkan) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan, "shadow_gallery", "demo_shadow_vulkan.png");
#endif
}
TEST(DemoScenes, Ktx2GalleryMetal) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal, "ktx2_gallery", "demo_ktx2_metal.png");
#endif
}
TEST(DemoScenes, Ktx2GalleryVulkan) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan, "ktx2_gallery", "demo_ktx2_vulkan.png");
#endif
}


TEST(DemoScenes, AlphaBlendMetal) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal, "alpha_blend", "demo_blend_metal.png");
#endif
}
TEST(DemoScenes, AlphaBlendVulkan) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan, "alpha_blend", "demo_blend_vulkan.png");
#endif
}

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
