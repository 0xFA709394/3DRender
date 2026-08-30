// KHR 扩展材质门控语义:程序化 clearcoat 球 开/关 渲染应不同;
// DamagedHelmet(无扩展)开/关应逐像素一致(零操作语义)。
// (Task 7 追加三 golden 模型用例。)
#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include "common/image.h"
#include "common/shader_code.h"
#include "renderer/renderer.h"
#include "renderer/quality.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "resource/primitives.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 256, kH = 256;

// 确定性画质档:阴影/后处理关,extMaterials=1
rd::QualityPreset extPreset() {
  rd::QualityPreset q{};
  q.renderScale = 1.0f;
  q.msaa = 1;
  q.iblPrefilterSize = 64;
  q.iblPrefilterMips = 5;
  q.maxTextureDim = 4096;
  q.shadowMapSize = 0;
  q.postEnabled = 0;
  q.fxaaEnabled = 0;
  q.extMaterials = 1;
  return q;
}

// 渲染一个 ModelAsset(extOn 控制门控)→ RGBA8;失败返回空(width==0)
rd::test::Image renderModel(rd::Backend b, const rd::ModelAsset& model, bool extOn) {
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
  auto skVs = load("pbr_forward_skinned.vert");
  auto sdsVs = load("shadow_depth_skinned.vert");
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,
                            {}, {}, {}, {}, {}, {}, {}, {}, unlitVs.entry,
                            rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  if (!target.valid() || !renderer.init(*device, sd)) return {};
  renderer.setQuality(extPreset());
  renderer.setExtMaterialsEnabled(extOn);
  auto res = rd::MeshRenderResource::upload(*device, model);
  if (!res) return {};
  rd::scene::Scene scene;
  auto node = std::make_unique<rd::scene::MeshNode>();
  node->mesh = res;
  scene.root().addChild(std::move(node));
  // 包围球取景:45° 方位角、20° 仰角
  rd::scene::Camera cam;
  const float dist = model.boundingRadius * 2.5f;
  glm::vec3 c(model.boundingCenter[0], model.boundingCenter[1], model.boundingCenter[2]);
  glm::vec3 eye = c + glm::vec3(dist * 0.65f, dist * 0.35f, dist * 0.65f);
  cam.lookAt({eye.x, eye.y, eye.z}, {c.x, c.y, c.z}, {0, 1, 0});
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
  renderer.shutdown();
  return img;
}

} // namespace

TEST(MaterialExtGate, ZeroOpOnPlainMaterial) {
  // DamagedHelmet 无扩展材质:ext 开/关逐像素一致(零操作硬约束)
  auto model = rd::loadGltf(RD_TEST_DATA_DIR "/assets/DamagedHelmet.glb");
  ASSERT_TRUE(model.valid());
  auto on = renderModel(rd::Backend::Metal, model, true);
  auto off = renderModel(rd::Backend::Metal, model, false);
  if (on.width == 0) GTEST_SKIP() << "Metal 不可用";
  ASSERT_EQ(on.pixels.size(), off.pixels.size());
  EXPECT_TRUE(rd::test::compareSSIM(on.pixels.data(), off.pixels.data(), kW, kH, 0.0).pass);
}

TEST(MaterialExtGate, ToggleChangesClearcoat) {
  // 程序化 clearcoat 球:开/关渲染应显著不同
  auto mesh = rd::primitives::makeSphere(0.5f, 48, 24);
  mesh.material.clearcoatFactor = 1.0f;
  mesh.material.clearcoatRoughnessFactor = 0.1f;
  mesh.material.roughnessFactor = 0.6f;
  mesh.material.metallicFactor = 0.1f;
  rd::ModelAsset model;
  model.meshes.push_back(std::move(mesh));
  model.boundingRadius = 0.5f;
  auto on = renderModel(rd::Backend::Metal, model, true);
  auto off = renderModel(rd::Backend::Metal, model, false);
  if (on.width == 0) GTEST_SKIP() << "Metal 不可用";
  ASSERT_EQ(on.pixels.size(), off.pixels.size());
  auto cmp = rd::test::compareSSIM(on.pixels.data(), off.pixels.data(), kW, kH, 0.05);
  EXPECT_GT(cmp.error, 0.01) << "clearcoat 开关未产生可见差异";
}
