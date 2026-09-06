// WaterSurface 创建/注入/步进冒烟 + Renderer 集成语义测试(平态零漂移/注入可见/
// 阻尼归零/静态场零操作)。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "renderer/renderer.h"
#include "renderer/water.h"
#include "resource/mesh_render_resource.h"
#include "resource/primitives.h"
#include "rhi/rhi_device.h"
#include "scene/camera.h"
#include "rd_shader_dir.h"
#include <glm/glm.hpp>

static rd::WaterSurface* makeWater(rd::Device& dev) {
  auto blitVs = rd::test::loadShaderCode(dev.backend(), RD_SHADER_DIR, "blit.vert");
  auto stepFs = rd::test::loadShaderCode(dev.backend(), RD_SHADER_DIR, "water_step.frag");
  auto cauFs = rd::test::loadShaderCode(dev.backend(), RD_SHADER_DIR, "water_caustics.frag");
  auto* w = new rd::WaterSurface();
  rd::WaterDesc d;
  d.simSize = 64;
  if (!w->create(dev, d, blitVs.code, stepFs.code, cauFs.code, blitVs.entry)) {
    delete w;
    return nullptr;
  }
  return w;
}

TEST(WaterSurface, CreateStepSmoke) {
#if defined(__APPLE__)
  rd::DeviceDesc dd;
  dd.backend = rd::Backend::Metal;
  auto dev = rd::createDevice(dd);
  ASSERT_TRUE(dev);
  auto* w = makeWater(*dev);
  ASSERT_NE(w, nullptr);
  EXPECT_TRUE(w->waveTex().valid());
  EXPECT_TRUE(w->causticsTex().valid());
  w->disturb(0.5f, 0.5f, 0.05f, 3.0f);
  w->tick(1.0f / 60.0f);
  dev->beginFrame();
  auto* cmd = dev->acquireCommandBuffer();
  w->step(cmd);
  dev->submit(cmd);
  dev->waitIdle();
  dev->endFrame();
  w->destroy(*dev);
  delete w;
#endif
}

namespace {
constexpr uint32_t kW = 256, kH = 256;

struct WaterRig {
  std::unique_ptr<rd::Device> dev;
  rd::Renderer renderer;
  rd::TargetHandle target;
  std::shared_ptr<rd::MeshRenderResource> floor, surf;
  bool ok = false;

  WaterRig(rd::Backend b) {
    rd::DeviceDesc d;
    d.backend = b;
    dev = rd::createDevice(d);
    if (!dev) return;
    auto load = [&](const char* n) {
      return rd::test::loadShaderCode(b, RD_SHADER_DIR, n);
    };
    auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
    auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
    auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
    auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
    auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
    auto skVs = load("pbr_forward_skinned.vert");
    auto sdsVs = load("shadow_depth_skinned.vert");
    auto exFs = load("bloom_extract.frag"), bbFs = load("bloom_blur.frag");
    auto cpFs = load("composite.frag"), fxFs = load("fxaa.frag");
    rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                              pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                              sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                              cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,
                              {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
                              unlitVs.entry, rd::Format::RGBA8_UNORM,
                              load("water_step.frag").code,
                              load("water_caustics.frag").code,
                              load("water_surface.vert").code,
                              load("water_surface.frag").code,
                              load("water_receiver.vert").code,
                              load("water_receiver.frag").code};
    rd::OffscreenTargetDesc td;
    td.width = kW;
    td.height = kH;
    td.depth = true;
    target = dev->createOffscreenTarget(td);
    if (!target.valid() || !renderer.init(*dev, sd)) return;
    rd::WaterDesc wd;
    wd.simSize = 128;
    if (!renderer.enableWater(wd)) return;
    // 池底 + 水面
    auto f = rd::primitives::makePlane(4.0f, 2.0f);
    f.material.roughnessFactor = 0.9f;
    rd::ModelAsset fm;
    fm.meshes.push_back(std::move(f));
    fm.boundingRadius = 3.0f;
    floor = rd::MeshRenderResource::upload(*dev, fm);
    auto s = rd::primitives::makeGrid(4.0f, 64);
    s.material.alphaBlend = true;
    s.material.baseColorFactor[0] = 0.15f;
    s.material.baseColorFactor[1] = 0.35f;
    s.material.baseColorFactor[2] = 0.4f;
    rd::ModelAsset sm;
    sm.meshes.push_back(std::move(s));
    sm.boundingRadius = 3.0f;
    surf = rd::MeshRenderResource::upload(*dev, sm);
    ok = floor && surf;
  }

  std::vector<uint8_t> frame() {
    rd::scene::Camera cam;
    cam.lookAt({2.2f, 2.0f, 2.4f}, {0, -0.2f, 0}, {0, 1, 0});
    cam.setPerspective(0.78539816f, 1.0f, 0.1f, 50.0f);
    dev->beginFrame();
    renderer.tick(1.0f / 60.0f);
    renderer.beginScene(cam, {0.05f, 0.05f, 0.06f, 1.0f});
    renderer.submitWaterReceiver(floor, glm::translate(rd::math::Mat4(1.0f),
                                                       rd::math::Vec3(0, -1.0f, 0)));
    renderer.submitWaterSurface(surf, rd::math::Mat4(1.0f));
    auto* cmd = dev->acquireCommandBuffer();
    renderer.endScene(cmd, target);
    dev->submit(cmd);
    dev->waitIdle();
    dev->endFrame();
    std::vector<uint8_t> px(size_t(kW) * kH * 4);
    dev->readbackTarget(target, px.data(), px.size());
    return px;
  }
};

size_t maxDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  size_t m = 0;
  for (size_t i = 0; i < a.size(); ++i)
    m = std::max(m, size_t(std::abs(int(a[i]) - int(b[i]))));
  return m;
}
} // namespace

TEST(Water, StillFramesIdentical) {
#if defined(__APPLE__)
  WaterRig rig(rd::Backend::Metal);
  ASSERT_TRUE(rig.ok);
  auto a = rig.frame();
  auto b = rig.frame();
  auto c = rig.frame();
  EXPECT_EQ(maxDiff(a, b), 0u);  // 平态启动:逐帧零漂移
  EXPECT_EQ(maxDiff(b, c), 0u);
#endif
}

TEST(Water, InjectChangesThenDecays) {
#if defined(__APPLE__)
  WaterRig rig(rd::Backend::Metal);
  ASSERT_TRUE(rig.ok);
  auto still = rig.frame();
  rig.renderer.disturbWater(0.5f, 0.5f, 0.05f, 3.0f);
  auto ripple = rig.frame();
  EXPECT_GT(maxDiff(still, ripple), 4u);  // 注入必可见
  for (int i = 0; i < 900; ++i) rig.frame();  // 15s @60fps 阻尼耗尽
  auto settled = rig.frame();
  EXPECT_LE(maxDiff(still, settled), 2u);  // 回到平态(±量化噪声)
#endif
}

TEST(Water, GateZeroOp) {
#if defined(__APPLE__)
  WaterRig rig(rd::Backend::Metal);
  ASSERT_TRUE(rig.ok);
  rig.renderer.setWaterParams({0.0f, 3.0f, 1.0f});  // waveScale=0 静态场
  rig.renderer.disturbWater(0.5f, 0.5f, 0.05f, 3.0f);
  auto a = rig.frame();
  rig.renderer.setWaterParams({0.0f, 0.0f, 1.0f});  // causticsIntensity 0 vs 3
  auto b = rig.frame();
  EXPECT_EQ(maxDiff(a, b), 0u);  // 静态场下焦散强度零操作
#endif
}
