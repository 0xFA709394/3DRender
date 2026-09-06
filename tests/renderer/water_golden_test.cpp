// water_pool golden:90 帧确定性仿真 + 两次脚本注入(host 场景无雨=确定性)。
#include <gtest/gtest.h>
#include "common/golden_test.h"
#include "common/shader_code.h"
#include "tools/render_test/scenes.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 512, kH = 512;

rd::test::Image renderWaterPool(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto load = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
  auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
  auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
  auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
  auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
  auto skVs = load("pbr_forward_skinned.vert");
  auto sdsVs = load("shadow_depth_skinned.vert");
  auto exFs = load("bloom_extract.frag"), bbFs = load("bloom_blur.frag");
  auto cpFs = load("composite.frag"), fxFs = load("fxaa.frag");
  auto wStepFs = load("water_step.frag"), wCauFs = load("water_caustics.frag");
  auto wSv = load("water_surface.vert"), wSf = load("water_surface.frag");
  auto wRv = load("water_receiver.vert"), wRf = load("water_receiver.frag");
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,
                            {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
                            unlitVs.entry, rd::Format::RGBA8_UNORM,
                            wStepFs.code, wCauFs.code, wSv.code, wSf.code, wRv.code,
                            wRf.code};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  rd::Renderer renderer;
  if (!target.valid() || !renderer.init(*device, sd)) return {};
  rd::ModelAsset storage;
  rd::tool::DemoScene scene;
  if (!rd::tool::buildDemoScene("water_pool", *device, renderer, scene, storage)) return {};
  // 48 帧 @1/60;帧 10/34 注入两滴(确定性;末期阻尼衰减前截取)
  for (int f = 0; f < 48; ++f) {
    if (f == 10) renderer.disturbWater(0.35f, 0.42f, 0.09f, 8.0f);
    if (f == 34) renderer.disturbWater(0.62f, 0.55f, 0.07f, 7.0f);
    device->beginFrame();
    renderer.beginScene(scene.camera, {0.05f, 0.05f, 0.06f, 1.0f});
    rd::tool::submitDemoScene(scene, renderer, 1.0f / 60.0f);
    auto* cmd = device->acquireCommandBuffer();
    renderer.endScene(cmd, target);
    device->submit(cmd);
    device->waitIdle();
    device->endFrame();
  }
  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(size_t(kW) * kH * 4);
  device->readbackTarget(target, img.pixels.data(), img.pixels.size());
  rd::tool::destroyDemoScene(scene, *device);
  renderer.shutdown();
  return img;
}
} // namespace

RD_GOLDEN_TEST(Water, Pool, "water_pool", 0.05, renderWaterPool)
