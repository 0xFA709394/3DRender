// 蒙皮 golden:运行时生成 2 骨 quad,弯折中点(t=0.5)渲染,双后端 golden。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "common/skinned_gen.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/animator.h"
#include "scene/camera.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <cstdlib>
#include <filesystem>

namespace {
constexpr uint32_t kW = 256, kH = 256;

void runSkinnedGolden(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  ASSERT_NE(device, nullptr);
  const std::string dir =
      (std::filesystem::temp_directory_path() / "rd_skin_golden").string();
  const std::string gltfPath = rd::test::writeSkinnedQuad(dir);
  auto model = rd::loadGltf(gltfPath.c_str());
  ASSERT_TRUE(model.valid());

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
  ASSERT_TRUE(renderer.init(*device, sd));

  auto res = rd::MeshRenderResource::upload(*device, model);
  ASSERT_NE(res, nullptr);
  rd::scene::Animator anim;
  ASSERT_TRUE(anim.bind(model));
  anim.play(0);
  anim.update(0.5f);  // 弯折中点(确定性)

  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  ASSERT_TRUE(target.valid());
  rd::scene::Camera cam;
  cam.lookAt({0, 1.2f, 3}, {0, 1, 0}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);

  device->beginFrame();
  renderer.beginScene(cam, {0.05f, 0.05f, 0.06f, 1.0f});
  renderer.submit(res, rd::math::Mat4(1.0f), anim.jointMatrices().data(),
                  uint32_t(anim.jointMatrices().size()));
  auto* cmd = device->acquireCommandBuffer();
  renderer.endScene(cmd, target);
  device->submit(cmd);
  device->waitIdle();
  device->endFrame();

  std::vector<uint8_t> px(size_t(kW) * kH * 4);
  ASSERT_TRUE(device->readbackTarget(target, px.data(), px.size()));
  res->destroy(*device);
  renderer.shutdown();
  const std::string name =
      b == rd::Backend::Metal ? "skinned_quad_metal.png" : "skinned_quad_vulkan.png";
  const std::string path = std::string(RD_TEST_DATA_DIR) + "/golden/" + name;
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(rd::test::savePNG(path, kW, kH, px.data()));
    return;
  }
  auto golden = rd::test::loadPNG(path);
  ASSERT_EQ(golden.pixels.size(), px.size()) << "golden 缺失: " << path;
  auto cmp = rd::test::compareRGBA8(px.data(), golden.pixels.data(), kW, kH, 3, 0.02);
  EXPECT_TRUE(cmp.pass) << "diffRatio=" << cmp.diffRatio;
}
} // namespace

TEST(Skinned, MetalGolden) {
#if defined(__APPLE__)
  runSkinnedGolden(rd::Backend::Metal);
#endif
}
TEST(Skinned, VulkanGolden) {
#if defined(RD_WITH_VULKAN)
  runSkinnedGolden(rd::Backend::Vulkan);
#endif
}
