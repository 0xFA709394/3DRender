// 画质预设表 + caps 启发式 + setQuality 渲染(Low 档 golden)。
#include <gtest/gtest.h>
#include "common/golden_test.h"
#include "common/image.h"
#include "common/shader_code.h"
#include "renderer/quality.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <cstdlib>
#include <glm/glm.hpp>

TEST(Quality, PresetTable) {
  const auto hi = rd::qualityPreset(rd::QualityTier::High);
  EXPECT_FLOAT_EQ(hi.renderScale, 1.0f);
  EXPECT_EQ(hi.msaa, 4u);
  EXPECT_EQ(hi.iblPrefilterSize, 256u);
  EXPECT_EQ(hi.iblPrefilterMips, 6u);
  EXPECT_EQ(hi.maxTextureDim, 4096u);
  EXPECT_EQ(hi.shadowMapSize, 2048u);
  EXPECT_EQ(hi.postEnabled, 1u);
  EXPECT_EQ(hi.fxaaEnabled, 0u);
  const auto mid = rd::qualityPreset(rd::QualityTier::Mid);
  EXPECT_FLOAT_EQ(mid.renderScale, 0.75f);
  EXPECT_EQ(mid.msaa, 2u);
  EXPECT_EQ(mid.iblPrefilterSize, 128u);
  EXPECT_EQ(mid.iblPrefilterMips, 5u);
  EXPECT_EQ(mid.maxTextureDim, 2048u);
  EXPECT_EQ(mid.shadowMapSize, 1024u);
  EXPECT_EQ(mid.postEnabled, 1u);
  EXPECT_EQ(mid.fxaaEnabled, 0u);
  const auto low = rd::qualityPreset(rd::QualityTier::Low);
  EXPECT_FLOAT_EQ(low.renderScale, 0.5f);
  EXPECT_EQ(low.msaa, 1u);
  EXPECT_EQ(low.iblPrefilterSize, 64u);
  EXPECT_EQ(low.iblPrefilterMips, 4u);
  EXPECT_EQ(low.maxTextureDim, 1024u);
  EXPECT_EQ(low.shadowMapSize, 0u);
  EXPECT_EQ(low.postEnabled, 0u);
  EXPECT_EQ(low.fxaaEnabled, 1u);
}

TEST(Quality, CapsHeuristic) {
  EXPECT_EQ(rd::qualityFromCaps(4, 8192), rd::QualityTier::High);
  EXPECT_EQ(rd::qualityFromCaps(8, 16384), rd::QualityTier::High);
  EXPECT_EQ(rd::qualityFromCaps(2, 8192), rd::QualityTier::Mid);
  EXPECT_EQ(rd::qualityFromCaps(4, 4096), rd::QualityTier::Mid);
  EXPECT_EQ(rd::qualityFromCaps(1, 4096), rd::QualityTier::Low);
  EXPECT_EQ(rd::qualityFromCaps(0, 0), rd::QualityTier::Low);
}

namespace {
constexpr uint32_t kW = 512, kH = 512;

// Low 档渲染 helmet:0.5x 内部分辨率 → upscale;golden 感知容差比对。
// golden 渲染函数(SSIM 判据由宏统一)
rd::test::Image renderLow(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!((device) != (nullptr))) return {};
  auto load = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
  auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
  auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
  auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
  auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
  auto exFs = load("bloom_extract.frag");
  auto bbFs = load("bloom_blur.frag");
  auto cpFs = load("composite.frag");
  auto fxFs = load("fxaa.frag");
  auto skVs = load("pbr_forward_skinned.vert");
  auto sdsVs = load("shadow_depth_skinned.vert");
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code, sdVs.code, sdFs.code,
                            exFs.code,   bbFs.code,   cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,
                            {},{}, {},{}, {}, unlitVs.entry, rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  auto model = rd::loadGltf(RD_TEST_DATA_DIR "/assets/DamagedHelmet.glb");
  if (!(target.valid() && model.valid())) return {};
  if (!(renderer.init(*device, sd))) return {};
  renderer.setQuality(rd::qualityPreset(rd::QualityTier::Low));
  auto res = rd::MeshRenderResource::upload(*device, model);
  if (!((res) != (nullptr))) return {};
  rd::scene::Scene scene;
  auto node = std::make_unique<rd::scene::MeshNode>();
  node->mesh = res;
  scene.root().addChild(std::move(node));
  rd::scene::Camera cam;
  const float dist = model.boundingRadius * 2.5f;
  glm::vec3 center(model.boundingCenter[0], model.boundingCenter[1], model.boundingCenter[2]);
  glm::vec3 eye = center + glm::vec3(dist * 0.65f, dist * 0.35f, dist * 0.65f);
  cam.lookAt({eye.x, eye.y, eye.z}, {center.x, center.y, center.z}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, dist * 0.1f, dist * 10.0f);

  device->beginFrame();
  renderer.beginScene(cam, {0.05f, 0.05f, 0.06f, 1.0f});
  scene.collect(renderer);
  auto* cmd = device->acquireCommandBuffer();
  renderer.endScene(cmd, target);
  device->submit(cmd);
  device->waitIdle();
  device->endFrame();

  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(size_t(kW) * kH * 4);
  auto& px = img.pixels;
  if (!(device->readbackTarget(target, px.data(), px.size()))) return {};
  res->destroy(*device);
  renderer.shutdown();
    return img;
}
} // namespace

RD_GOLDEN_TEST(Quality, LowGolden, "helmet_low", 0.05, renderLow)
