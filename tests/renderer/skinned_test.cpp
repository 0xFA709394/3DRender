// 蒙皮 golden:运行时生成 2 骨 quad,弯折中点(t=0.5)渲染,双后端 golden。
#include <gtest/gtest.h>
#include "common/golden_test.h"
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

// golden 渲染函数(SSIM 判据由宏统一)
rd::test::Image renderSkinned(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!((device) != (nullptr))) return {};
  const std::string dir =
      (std::filesystem::temp_directory_path() / "rd_skin_golden").string();
  const std::string gltfPath = rd::test::writeSkinnedQuad(dir);
  auto model = rd::loadGltf(gltfPath.c_str());
  if (!(model.valid())) return {};

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
                            {},{}, {},{}, {}, unlitVs.entry, rd::Format::RGBA8_UNORM};
  if (!(renderer.init(*device, sd))) return {};

  auto res = rd::MeshRenderResource::upload(*device, model);
  if (!((res) != (nullptr))) return {};
  rd::scene::Animator anim;
  if (!(anim.bind(model))) return {};
  anim.play(0);
  anim.update(0.5f);  // 弯折中点(确定性)

  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  if (!(target.valid())) return {};
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

// Fox.glb(24 关节/真纹理/条带索引)冒烟:非背景覆盖率断言(资产缺失则跳过)。
TEST(Skinned, FoxSmoke) {
  if (!getenv("RD_ASSETS_DIR"))
    setenv("RD_ASSETS_DIR", (std::string(RD_TEST_DATA_DIR) + "/../assets").c_str(), 1);
  if (!getenv("RD_ASSETS_DIR"))
    setenv("RD_ASSETS_DIR", (std::string(RD_TEST_DATA_DIR) + "/../assets").c_str(), 1);
  const std::string foxPath =
      std::string(getenv("RD_ASSETS_DIR") ? getenv("RD_ASSETS_DIR") : "assets") +
      "/Fox.glb";
  if (!std::filesystem::exists(foxPath)) GTEST_SKIP() << "Fox.glb 未下载";
#if defined(__APPLE__)
  rd::DeviceDesc d;
  d.backend = rd::Backend::Metal;
  auto device = rd::createDevice(d);
  ASSERT_TRUE((device) != (nullptr));
  auto model = rd::loadGltf(foxPath.c_str());
  ASSERT_TRUE(model.valid());
  auto load = [&](const char* n) { return rd::test::loadShaderCode(rd::Backend::Metal, RD_SHADER_DIR, n); };
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
                            {},{}, {},{}, {}, unlitVs.entry, rd::Format::RGBA8_UNORM};
  ASSERT_TRUE(renderer.init(*device, sd));
  auto res = rd::MeshRenderResource::upload(*device, model);
  ASSERT_TRUE((res) != (nullptr));
  rd::scene::Animator anim;
  ASSERT_TRUE(anim.bind(model));
  anim.play(0);
  anim.update(0.0f);  // 首帧姿态
  rd::OffscreenTargetDesc td;
  td.width = 256;
  td.height = 256;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  ASSERT_TRUE(target.valid());
  rd::scene::Camera cam;
  // 模型包围球取景(与场景一致)
  const float dist = model.boundingRadius * 2.5f;
  glm::vec3 c(model.boundingCenter[0], model.boundingCenter[1], model.boundingCenter[2]);
  glm::vec3 eye = c + glm::vec3(dist * 0.65f, dist * 0.35f, dist * 0.65f);
  cam.lookAt({eye.x, eye.y, eye.z}, {c.x, c.y, c.z}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, dist * 0.1f, dist * 10.0f);
  device->beginFrame();
  renderer.beginScene(cam, {0.05f, 0.05f, 0.06f, 1.0f});
  renderer.submit(res, rd::math::Mat4(1.0f), anim.jointMatrices().data(),
                  uint32_t(anim.jointMatrices().size()));
  auto* cmd = device->acquireCommandBuffer();
  renderer.endScene(cmd, target);
  device->submit(cmd);
  device->waitIdle();
  device->endFrame();
  std::vector<uint8_t> px(size_t(256) * 256 * 4);
  ASSERT_TRUE(device->readbackTarget(target, px.data(), px.size()));
  // 非背景像素占比
  uint32_t hit = 0;
  for (size_t i = 0; i < px.size(); i += 4)
    if (abs(px[i] - 13) + abs(px[i + 1] - 13) + abs(px[i + 2] - 15) > 12) ++hit;
  EXPECT_GT(double(hit) * 4 / (256.0 * 256.0), 0.02f) << "fox 未渲染";
  res->destroy(*device);
  renderer.shutdown();
#endif
}

RD_GOLDEN_TEST(Skinned, Golden, "skinned_quad", 0.05, renderSkinned)
