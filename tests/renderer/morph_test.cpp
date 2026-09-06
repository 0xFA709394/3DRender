// morph targets 语义:零权重 == 无 morph 逐像素一致(零操作);权重变化可见;
// + golden 三件(AnimatedMorphCube/MorphPrimitivesTest/morph+skin 组合;缺失自动 skip)。
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <glm/glm.hpp>
#include "common/golden_test.h"
#include "common/image.h"
#include "common/morph_gen.h"
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
rd::test::Image renderModel(rd::Backend b, const rd::ModelAsset& model,
                            const float* morphWeights = nullptr,
                            uint32_t morphCount = 0, bool animate = false) {
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
  rd::scene::Animator anim;
  const bool hasAnim = animate && anim.bind(model);
  if (hasAnim) {
    anim.play(0);
    anim.update(0.5f);
  }
  rd::scene::Scene scene;
  auto node = std::make_unique<rd::scene::MeshNode>();
  node->mesh = res;
  scene.root().addChild(std::move(node));
  rd::scene::Camera cam;
  const float dist = model.boundingRadius * 2.5f;
  glm::vec3 c(model.boundingCenter[0], model.boundingCenter[1], model.boundingCenter[2]);
  glm::vec3 eye = c + glm::vec3(dist * 0.65f, dist * 0.35f, dist * 0.65f);
  cam.lookAt({eye.x, eye.y, eye.z}, {c.x, c.y, c.z}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, dist * 0.1f, dist * 10.0f);
  device->beginFrame();
  renderer.beginScene(cam, {0.05f, 0.05f, 0.06f, 1.0f});
  if (hasAnim) {
    // 蒙皮/权重动画路径:Animator 关节 + 权重
    const float* w = anim.morphWeights().empty() ? morphWeights
                                                 : anim.morphWeights().data();
    const uint32_t wc =
        anim.morphWeights().empty() ? morphCount : anim.morphTargetCount();
    renderer.submit(res, rd::math::Mat4(1.0f), anim.jointMatrices().data(),
                    uint32_t(anim.jointMatrices().size()), w, wc);
  } else {
    scene.collect(renderer);
    if (morphWeights && morphCount > 0)
      ;  // collect 路径不支持覆盖——调用方须用 animate 或静态权重;此处不达
  }
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

// 程序化 morph 球:单目标 pos 增量(权重由调用方)
rd::ModelAsset morphSphere(float dx, float w) {
  rd::ModelAsset model;
  auto mesh = rd::primitives::makeSphere(0.5f, 48, 24);
  const size_t vcount = mesh.vertices.size() / 12;
  mesh.morph = true;
  mesh.morphPosDeltas.assign(vcount * 3, dx);
  mesh.morphNormalDeltas.assign(vcount * 3, 0.0f);
  mesh.morphWeights.assign(1, w);
  model.meshes.push_back(std::move(mesh));
  model.boundingRadius = 0.5f;
  return model;
}

} // namespace

// 零操作:零权重 morph 渲染 == 无 morph 逐像素一致
TEST(MorphGate, ZeroOpOnZeroWeights) {
  auto withMorph = morphSphere(0.05f, 0.0f);   // 非零增量但权重 0
  auto plain = morphSphere(0.0f, 0.0f);
  plain.meshes[0].morph = false;               // 去 morph
  plain.meshes[0].morphPosDeltas.clear();
  auto a = renderModel(rd::Backend::Metal, withMorph);
  auto b = renderModel(rd::Backend::Metal, plain);
  if (a.width == 0) GTEST_SKIP() << "Metal 不可用";
  ASSERT_EQ(a.pixels.size(), b.pixels.size());
  EXPECT_TRUE(rd::test::compareSSIM(a.pixels.data(), b.pixels.data(), kW, kH, 0.0).pass);
}

// 权重可见性:同增量 w=0 vs w=1 显著不同
TEST(MorphGate, WeightChangesGeometry) {
  // 静态权重路径:模型自带权重 1 vs 0
  auto w1 = morphSphere(0.8f, 1.0f);   // 球整体 +0.8 位移
  auto w0 = morphSphere(0.8f, 0.0f);
  auto a = renderModel(rd::Backend::Metal, w1);
  auto b = renderModel(rd::Backend::Metal, w0);
  if (a.width == 0) GTEST_SKIP() << "Metal 不可用";
  ASSERT_EQ(a.pixels.size(), b.pixels.size());
  auto cmp = rd::test::compareSSIM(a.pixels.data(), b.pixels.data(), kW, kH, 0.05);
  EXPECT_GT(cmp.error, 0.01) << "morph 权重未产生差异";
}

// ---- golden 三件(fetch_assets 下载;缺失自动 skip)----

namespace {

std::string findAsset(const char* rel) {
  if (!getenv("RD_ASSETS_DIR"))
    setenv("RD_ASSETS_DIR", (std::string(RD_TEST_DATA_DIR) + "/../assets").c_str(), 1);
  std::string p = std::string(getenv("RD_ASSETS_DIR")) + "/" + rel;
  return std::filesystem::exists(p) ? p : std::string();
}

rd::test::Image renderAsset(rd::Backend b, const char* rel, bool animate) {
  const std::string path = findAsset(rel);
  if (path.empty()) return {};
  auto model = rd::loadGltf(path.c_str());
  if (!model.valid()) return {};
  return renderModel(b, model, nullptr, 0, animate);
}

} // namespace

rd::test::Image renderMorphCube(rd::Backend b) {
  return renderAsset(b, "AnimatedMorphCube.glb", true);
}
rd::test::Image renderMorphPrimitives(rd::Backend b) {
  return renderAsset(b, "MorphPrimitivesTest.glb", true);
}
rd::test::Image renderMorphCombo(rd::Backend b) {
  const std::string dir =
      (std::filesystem::temp_directory_path() / "rd_morph_combo").string();
  const std::string gltfPath = rd::test::writeMorphSkinnedQuad(dir);
  if (gltfPath.empty()) return {};
  auto model = rd::loadGltf(gltfPath.c_str());
  if (!model.valid()) return {};
  return renderModel(b, model, nullptr, 0, true);  // 弯折+权重中点(确定性)
}

RD_GOLDEN_TEST(Morph, Cube, "morph_cube", 0.05, renderMorphCube)
RD_GOLDEN_TEST(Morph, Primitives, "morph_primitives", 0.05, renderMorphPrimitives)
RD_GOLDEN_TEST(Morph, Combo, "morph_combo", 0.05, renderMorphCombo)
