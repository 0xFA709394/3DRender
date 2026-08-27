// 阴影 golden:helmet + 程序化地面 quad + 方向光(包围球取景)。
// LightUBO 布局 static_assert 也在此。
#include <gtest/gtest.h>
#include "common/golden_test.h"
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

static_assert(sizeof(rd::LightUBOData) == 432, "LightUBO 必须 432B");

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

// golden 渲染函数(SSIM 判据由宏统一)
rd::test::Image renderShadow(rd::Backend b) {
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
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,   {},{}, {},{}, {},{}, {}, {}, unlitVs.entry,
                            rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  auto helmet = rd::loadGltf(RD_TEST_DATA_DIR "/assets/DamagedHelmet.glb");
  auto ground = makeGround(-0.55f, 1.6f);
  if (!(target.valid() && helmet.valid())) return {};
  if (!(renderer.init(*device, sd))) return {};
  auto helmetRes = rd::MeshRenderResource::upload(*device, helmet);
  auto groundRes = rd::MeshRenderResource::upload(*device, ground);
  if (!((helmetRes) != (nullptr))) return {};
  if (!((groundRes) != (nullptr))) return {};
  rd::scene::Scene scene;
  auto n1 = std::make_unique<rd::scene::MeshNode>();
  n1->mesh = helmetRes;
  scene.root().addChild(std::move(n1));
  auto n2 = std::make_unique<rd::scene::MeshNode>();
  n2->mesh = groundRes;
  scene.root().addChild(std::move(n2));

  // 方向光(斜上方)+ 阴影开(High 档 2048)
  rd::LightData light;
  light.type = rd::LightType::Directional;
  float dl = std::sqrt(0.5f * 0.5f + 0.8f * 0.8f + 0.3f * 0.3f);
  light.direction[0] = 0.5f / dl;
  light.direction[1] = 0.8f / dl;
  light.direction[2] = 0.3f / dl;
  light.color[0] = light.color[1] = light.color[2] = 3.0f;
  renderer.setLights({light});
  renderer.setQuality(rd::qualityPreset(rd::QualityTier::High));
  renderer.setLightFraming(helmet.boundingCenter, helmet.boundingRadius);

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

  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(size_t(kW) * kH * 4);
  auto& px = img.pixels;
  if (!(device->readbackTarget(target, px.data(), px.size()))) return {};
  helmetRes->destroy(*device);
  groundRes->destroy(*device);
  renderer.shutdown();
    return img;
}

// 聚光阴影 golden:helmet + 地面 + 头顶聚光(锥罩内亮、罩外暗,helmet 投影到地面)
rd::test::Image renderSpotShadow(rd::Backend b) {
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
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,   {},{}, {},{}, {},{}, {}, {}, unlitVs.entry,
                            rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  auto helmet = rd::loadGltf(RD_TEST_DATA_DIR "/assets/DamagedHelmet.glb");
  auto ground = makeGround(-0.55f, 1.6f);
  if (!(target.valid() && helmet.valid())) return {};
  if (!(renderer.init(*device, sd))) return {};
  auto helmetRes = rd::MeshRenderResource::upload(*device, helmet);
  auto groundRes = rd::MeshRenderResource::upload(*device, ground);
  if (!((helmetRes) != (nullptr))) return {};
  if (!((groundRes) != (nullptr))) return {};
  rd::scene::Scene scene;
  auto n1 = std::make_unique<rd::scene::MeshNode>();
  n1->mesh = helmetRes;
  scene.root().addChild(std::move(n1));
  auto n2 = std::make_unique<rd::scene::MeshNode>();
  n2->mesh = groundRes;
  scene.root().addChild(std::move(n2));

  // 聚光灯:斜上方指向 helmet 中心(内锥 15°/外锥 25°,range 10)
  rd::LightData light;
  light.type = rd::LightType::Spot;
  glm::vec3 center(helmet.boundingCenter[0], helmet.boundingCenter[1],
                   helmet.boundingCenter[2]);
  const glm::vec3 pos = center + glm::vec3(0.9f, 2.2f, 0.9f);
  const glm::vec3 dir = glm::normalize(center - pos);
  light.position[0] = pos.x; light.position[1] = pos.y; light.position[2] = pos.z;
  light.direction[0] = dir.x; light.direction[1] = dir.y; light.direction[2] = dir.z;
  light.innerCone = 0.2618f;   // 15°
  light.outerCone = 0.4363f;   // 25°
  light.range = 10.0f;
  light.color[0] = light.color[1] = light.color[2] = 8.0f;
  renderer.setLights({light});
  renderer.setQuality(rd::qualityPreset(rd::QualityTier::High));

  rd::scene::Camera cam;
  const float dist = helmet.boundingRadius * 2.5f;
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
  helmetRes->destroy(*device);
  groundRes->destroy(*device);
  renderer.shutdown();
    return img;
}
} // namespace

RD_GOLDEN_TEST(Shadow, Golden, "helmet_shadow", 0.05, renderShadow)

RD_GOLDEN_TEST(SpotShadow, Golden, "helmet_spot_shadow", 0.05, renderSpotShadow)
