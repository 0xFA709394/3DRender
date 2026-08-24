// renderer 骨架 smoke 测试:init → 空帧 → 提交 BoxTextured 一帧 → 不崩溃且句柄有效。
// (渲染正确性由 BoxUnlit golden 兜底)
#include <gtest/gtest.h>
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 256, kH = 256;

std::unique_ptr<rd::Renderer> makeRenderer(rd::Device& dev, rd::Backend b) {
  auto load = [&](const char* name) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, name); };
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
  auto r = std::make_unique<rd::Renderer>();
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code, sdVs.code, sdFs.code,
                            exFs.code,   bbFs.code,   cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,
                            {},{}, {},{}, {},{}, {}, {}, unlitVs.entry,
                            rd::Format::RGBA8_UNORM};
  if (!r->init(dev, sd)) return nullptr;
  return r;
}

void runCase(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  auto renderer = makeRenderer(*dev, b);
  ASSERT_NE(renderer, nullptr);

  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = dev->createOffscreenTarget(td);
  ASSERT_TRUE(target.valid());

  rd::scene::Camera cam;
  cam.lookAt({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);

  // 空帧(无提交)
  dev->beginFrame();
  renderer->beginScene(cam, {0.1f, 0.1f, 0.1f, 1.0f});
  auto* cmd = dev->acquireCommandBuffer();
  renderer->endScene(cmd, target);
  dev->submit(cmd);
  dev->waitIdle();
  dev->endFrame();

  // 提交 BoxTextured 一帧
  auto model = rd::loadGltf(RD_TEST_DATA_DIR "/assets/BoxTextured.glb");
  ASSERT_TRUE(model.valid());
  auto res = rd::MeshRenderResource::upload(*dev, model);
  ASSERT_NE(res, nullptr);
  rd::scene::Scene scene;
  auto meshNode = std::make_unique<rd::scene::MeshNode>();
  meshNode->mesh = res;
  scene.root().addChild(std::move(meshNode));

  dev->beginFrame();
  renderer->beginScene(cam, {0.1f, 0.1f, 0.1f, 1.0f});
  scene.collect(*renderer);
  cmd = dev->acquireCommandBuffer();
  renderer->endScene(cmd, target);
  dev->submit(cmd);
  dev->waitIdle();
  dev->endFrame();
  res->destroy(*dev);
}
} // namespace

TEST(Renderer, MetalSmokeFrame) {
#if defined(__APPLE__)
  runCase(rd::Backend::Metal);
#endif
}

TEST(Renderer, VulkanSmokeFrame) {
#if defined(RD_WITH_VULKAN)
  runCase(rd::Backend::Vulkan);
#endif
}
