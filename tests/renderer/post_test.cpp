// 后处理 golden:helmet + 程序化地面 quad,High 档 Bloom+ACES。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "renderer/light_ubo.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <cstdlib>
#include <cstring>
#include <glm/glm.hpp>



namespace {
constexpr uint32_t kW = 512, kH = 512;

/// 程序化地面(y 平面,2 三角形;48B 交错顶点)
rd::ModelAsset makeGround(float y, float half) {
  rd::ModelAsset m;
  rd::MeshData mesh;
  mesh.name = "ground";
  const float v[4][12] = {
      // pos              normal     tangent        uv
      {-half, y, -half, 0, 1, 0, 1, 0, 0, 1, 0, 0},
      { half, y, -half, 0, 1, 0, 1, 0, 0, 1, 1, 0},
      { half, y,  half, 0, 1, 0, 1, 0, 0, 1, 1, 1},
      {-half, y,  half, 0, 1, 0, 1, 0, 0, 1, 0, 1},
  };
  mesh.vertices.assign(&v[0][0], &v[0][0] + 4 * 12);
  const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
  mesh.indices.resize(sizeof(idx));
  memcpy(mesh.indices.data(), idx, sizeof(idx));
  mesh.indexType = rd::IndexType::UInt16;
  mesh.indexCount = 6;
  mesh.material.baseColorFactor[0] = 0.8f;
  mesh.material.baseColorFactor[1] = 0.8f;
  mesh.material.baseColorFactor[2] = 0.8f;
  mesh.material.baseColorFactor[3] = 1.0f;
  mesh.material.roughnessFactor = 0.9f;
  mesh.material.metallicFactor = 0.0f;
  m.meshes.push_back(std::move(mesh));
  m.boundingRadius = half * 1.5f;
  return m;
}

void runPostGolden(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  ASSERT_NE(device, nullptr);
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
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,
                            unlitVs.entry, rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  auto helmet = rd::loadGltf(RD_TEST_DATA_DIR "/assets/DamagedHelmet.glb");
  auto ground = makeGround(-0.55f, 1.6f);
  ASSERT_TRUE(target.valid() && helmet.valid());
  ASSERT_TRUE(renderer.init(*device, sd));
  auto helmetRes = rd::MeshRenderResource::upload(*device, helmet);
  auto groundRes = rd::MeshRenderResource::upload(*device, ground);
  ASSERT_NE(helmetRes, nullptr);
  ASSERT_NE(groundRes, nullptr);
  rd::scene::Scene scene;
  auto n1 = std::make_unique<rd::scene::MeshNode>();
  n1->mesh = helmetRes;
  scene.root().addChild(std::move(n1));
  auto n2 = std::make_unique<rd::scene::MeshNode>();
  n2->mesh = groundRes;
  scene.root().addChild(std::move(n2));

  // High 档:HDR Bloom + ACES(默认灯)
  renderer.setQuality(rd::qualityPreset(rd::QualityTier::High));

  rd::scene::Camera cam;
  const float dist = helmet.boundingRadius * 2.5f;
  glm::vec3 center(helmet.boundingCenter[0], helmet.boundingCenter[1],
                   helmet.boundingCenter[2]);
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

  std::vector<uint8_t> px(size_t(kW) * kH * 4);
  ASSERT_TRUE(device->readbackTarget(target, px.data(), px.size()));
  helmetRes->destroy(*device);
  groundRes->destroy(*device);
  renderer.shutdown();
  const std::string name =
      b == rd::Backend::Metal ? "helmet_post_metal.png" : "helmet_post_vulkan.png";
  const std::string path = std::string(RD_TEST_DATA_DIR) + "/golden/" + name;
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(rd::test::savePNG(path, kW, kH, px.data()));
    return;
  }
  auto golden = rd::test::loadPNG(path);
  ASSERT_EQ(golden.pixels.size(), px.size()) << "golden 缺失: " << path;
  auto cmp = rd::test::compareSSIM(px.data(), golden.pixels.data(), kW, kH);
  auto pix_cmp = rd::test::compareRGBA8(px.data(), golden.pixels.data(), kW, kH, 3, 1.0);
  EXPECT_TRUE(cmp.pass) << "ssimError=" << cmp.error
      << " pixelDiffRatio=" << pix_cmp.diffRatio;
}
} // namespace

TEST(Post, MetalGolden) {
#if defined(__APPLE__)
  runPostGolden(rd::Backend::Metal);
#endif
}
TEST(Post, VulkanGolden) {
#if defined(RD_WITH_VULKAN)
  runPostGolden(rd::Backend::Vulkan);
#endif
}
