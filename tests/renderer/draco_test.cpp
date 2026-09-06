// draco 解码 golden:生成器资产(确定性 round-trip)双后端渲染。
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <glm/glm.hpp>
#include "common/golden_test.h"
#include "common/image.h"
#include "common/draco_gen.h"
#include "common/shader_code.h"
#include "renderer/renderer.h"
#include "renderer/quality.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "resource/primitives.h"
#include "scene/animator.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 256, kH = 256;

rd::QualityPreset morphPreset() {
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
  q.transmission = 1;
  return q;
}

// 渲染一个 ModelAsset → RGBA8;morphWeights 非空时经重载覆盖;
// animate=true 时绑定 Animator 播 clip0 并 update(0.5f)(确定性中点)。
rd::test::Image renderModel(rd::Backend b, const rd::ModelAsset& model) {
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
  auto skVs = load("pbr_forward_skinned.vert");
  auto sdsVs = load("shadow_depth_skinned.vert");
  auto mVs = load("pbr_forward_morph.vert");
  auto mSkVs = load("pbr_forward_morph_skinned.vert");
  auto mSv = load("shadow_depth_morph.vert");
  auto mSdVs = load("shadow_depth_morph_skinned.vert");
  auto exFs = load("bloom_extract.frag");
  auto bbFs = load("bloom_blur.frag");
  auto cpFs = load("composite.frag");
  auto fxFs = load("fxaa.frag");
  auto eqFs = load("equirect_to_cube.frag");
  auto skyVs = load("skybox.vert"), skyFs = load("skybox.frag");
  auto instVs = load("pbr_forward_instanced.vert");
  auto instFs = load("pbr_forward_instanced.frag");
  auto smVs = load("shadow_depth_mask.vert"), smFs = load("shadow_depth_mask.frag");
  auto siVs = load("shadow_depth_instanced.vert");
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,
                            eqFs.code, skyVs.code, skyFs.code, instVs.code, instFs.code,
                            smVs.code, smFs.code, siVs.code, mVs.code, mSkVs.code,
                            mSv.code, mSdVs.code, unlitVs.entry, rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  if (!target.valid() || !renderer.init(*device, sd)) return {};
  renderer.setQuality(morphPreset());
  auto res = rd::MeshRenderResource::upload(*device, model);
  if (!res) return {};
  rd::scene::Camera cam;
  const float dist = model.boundingRadius * 2.5f;
  glm::vec3 c(model.boundingCenter[0], model.boundingCenter[1], model.boundingCenter[2]);
  glm::vec3 eye = c + glm::vec3(dist * 0.65f, dist * 0.35f, dist * 0.65f);
  cam.lookAt({eye.x, eye.y, eye.z}, {c.x, c.y, c.z}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, dist * 0.1f, dist * 10.0f);
  // 蒙皮模型:Animator 绑定(无动画)取单位关节调色板提交
  rd::scene::Animator anim;
  if (!anim.bind(model)) return {};
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
  device->readbackTarget(target, img.pixels.data(), img.pixels.size());
  renderer.shutdown();
  return img;
}

} // namespace


rd::test::Image renderDracoSphere(rd::Backend b) {
  const std::string dir =
      (std::filesystem::temp_directory_path() / "rd_draco_golden").string();
  const std::string glb = rd::test::writeDracoSphere(dir);
  if (glb.empty()) return {};
  auto model = rd::loadGltf(glb.c_str());
  if (!model.valid()) return {};
  return renderModel(b, model);
}

RD_GOLDEN_TEST(Draco, Sphere, "draco_sphere", 0.05, renderDracoSphere)
