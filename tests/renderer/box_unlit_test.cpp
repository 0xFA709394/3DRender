// BoxTextured 固定机位 unlit 渲染 golden:相机 (0,0,3) 看向原点,fov 45°,
// 512x512 带深度,与 tests/golden/ 基准 PNG 容差比较(Metal/Vulkan 各一份)。
// 更新 golden:RD_UPDATE_GOLDENS=1 ctest --test-dir build -R BoxUnlit(更新后须目视核对)
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

namespace {
constexpr uint32_t kW = 512, kH = 512;

std::string goldenPath(rd::Backend b) {
  std::string name = (b == rd::Backend::Metal) ? "box_unlit_metal.png" : "box_unlit_vulkan.png";
  return std::string(RD_TEST_DATA_DIR) + "/golden/" + name;
}

rd::test::Image renderBox(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "unlit.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "unlit.frag");
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{vs.code, fs.code, vs.entry, rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  auto model = rd::loadGltf(RD_TEST_DATA_DIR "/assets/BoxTextured.glb");
  if (!target.valid() || !model.valid() || !renderer.init(*device, sd)) return {};
  auto res = rd::MeshRenderResource::upload(*device, model);
  if (!res) return {};

  rd::scene::Scene scene;
  auto meshNode = std::make_unique<rd::scene::MeshNode>();
  meshNode->mesh = res;
  scene.root().addChild(std::move(meshNode));
  rd::scene::Camera cam;
  cam.lookAt({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);

  device->beginFrame();
  renderer.beginScene(cam, {0.2f, 0.2f, 0.25f, 1.0f});
  scene.collect(renderer);
  auto* cmd = device->acquireCommandBuffer();
  renderer.endScene(cmd, target);
  device->submit(cmd);
  device->waitIdle();
  device->endFrame();

  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(kW * kH * 4);
  device->readbackTarget(target, img.pixels.data(), img.pixels.size());
  res->destroy(*device);
  renderer.shutdown();
  return img;
}

void runGolden(rd::Backend b) {
  auto img = renderBox(b);
  ASSERT_EQ(img.pixels.size(), kW * kH * 4);
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

TEST(BoxUnlit, MetalGolden) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal);
#endif
}

TEST(BoxUnlit, VulkanGolden) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan);
#endif
}
