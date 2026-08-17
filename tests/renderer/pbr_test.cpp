// DamagedHelmet PBR+IBL golden:包围球取景(45° 方位角/20° 仰角),
// 512x512 带深度,与 tests/golden/ 基准 PNG 容差比较(Metal/Vulkan 各一份)。
// 更新:RD_UPDATE_GOLDENS=1 ctest --test-dir build -R PbrHelmet(须像素核对再提交)
#include <gtest/gtest.h>
#include <cstdlib>
#include "common/image.h"
#include "common/shader_code.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <glm/glm.hpp>

namespace {
constexpr uint32_t kW = 512, kH = 512;

std::string goldenPath(rd::Backend b) {
  std::string name = (b == rd::Backend::Metal) ? "helmet_metal.png" : "helmet_vulkan.png";
  return std::string(RD_TEST_DATA_DIR) + "/golden/" + name;
}

rd::test::Image renderHelmet(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
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
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code, sdVs.code, sdFs.code,
                            exFs.code,   bbFs.code,   cpFs.code,   fxFs.code,
                            unlitVs.entry,
                            rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  auto model = rd::loadGltf(RD_TEST_DATA_DIR "/assets/DamagedHelmet.glb");
  if (!target.valid() || !model.valid() || !renderer.init(*device, sd)) return {};
  auto res = rd::MeshRenderResource::upload(*device, model);
  if (!res) return {};

  rd::scene::Scene scene;
  auto meshNode = std::make_unique<rd::scene::MeshNode>();
  meshNode->mesh = res;
  scene.root().addChild(std::move(meshNode));

  // 包围球取景:45° 方位角、20° 仰角
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
  device->readbackTarget(target, img.pixels.data(), img.pixels.size());
  res->destroy(*device);
  renderer.shutdown();
  return img;
}

void runGolden(rd::Backend b) {
  auto img = renderHelmet(b);
  ASSERT_EQ(img.pixels.size(), size_t(kW) * kH * 4);
  const std::string path = goldenPath(b);
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(rd::test::savePNG(path, img.width, img.height, img.pixels.data()));
    return;
  }
  auto golden = rd::test::loadPNG(path);
  ASSERT_EQ(golden.pixels.size(), img.pixels.size()) << "golden 缺失: " << path;
  auto cmp = rd::test::compareRGBA8(img.pixels.data(), golden.pixels.data(), kW, kH, 3, 0.02);
  EXPECT_TRUE(cmp.pass) << "diffRatio=" << cmp.diffRatio;
}
} // namespace

TEST(PbrHelmet, MetalGolden) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal);
#endif
}

TEST(PbrHelmet, VulkanGolden) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan);
#endif
}
