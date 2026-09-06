// KTX2 渲染 golden:运行时生成 basisu gltf(tri.ktx2 纹理)→ 按 caps 转码上传
// → Renderer 渲染 512x512 → golden 感知容差比对。验证转码+压缩上传+采样全链稳定。
#include <gtest/gtest.h>
#include "common/golden_test.h"
#include "common/image.h"
#include "common/ktx2_gen.h"
#include "common/shader_code.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace {
constexpr uint32_t kW = 512, kH = 512;

// 与 gltf_test 的 BasisuExternalUri 同构:pos|uv 双 bufferView 三角形
std::string writeTriGltf(const std::filesystem::path& dir) {
  const float pos[9] = {-0.8f, -0.8f, 0, 0.8f, -0.8f, 0, 0, 0.8f, 0};
  const float uv[6] = {0, 0, 1, 0, 0.5f, 1};
  const uint16_t idx[3] = {0, 1, 2};
  const std::string binPath = (dir / "tri.bin").string();
  FILE* f = fopen(binPath.c_str(), "wb");
  fwrite(pos, 4, 9, f);
  fwrite(uv, 4, 6, f);
  fwrite(idx, 2, 3, f);
  fclose(f);
  const char* json = R"({
    "asset": {"version": "2.0"},
    "extensionsUsed": ["KHR_texture_basisu"],
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                                "indices": 2, "material": 0}]}],
    "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
      "metallicFactor": 0.0, "roughnessFactor": 0.9}}],
    "textures": [{"extensions": {"KHR_texture_basisu": {"source": 0}}}],
    "images": [{"uri": "tex.ktx2"}],
    "buffers": [{"uri": "tri.bin", "byteLength": 66}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 24},
      {"buffer": 0, "byteOffset": 60, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
      {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  const std::string gltfPath = (dir / "tri.gltf").string();
  f = fopen(gltfPath.c_str(), "wb");
  fwrite(json, 1, strlen(json), f);
  fclose(f);
  return gltfPath;
}

// golden 渲染函数(SSIM 判据由宏统一)
rd::test::Image renderKtx2(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!((device) != (nullptr))) return {};
  const auto dir = std::filesystem::temp_directory_path() / "rd_ktx2_golden";
  std::filesystem::create_directories(dir);
  if (!(rd::test::writeTestKtx2((dir / "tex.ktx2").string().c_str(), 64))) return {};
  const std::string gltfPath = writeTriGltf(dir);

  rd::TextureLoadPref pref;
  pref.ktx2Target = rd::pickTranscodeTarget(
      device->caps().supports(rd::Capability::texture_compression_astc),
      device->caps().supports(rd::Capability::texture_compression_etc2));
  auto model = rd::loadGltf(gltfPath.c_str(), pref);
  if (!(model.valid())) return {};

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
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code, sdVs.code, sdFs.code,
                            exFs.code,   bbFs.code,   cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,{},{},{},{},{},{},{},{},{},{},{},{}, unlitVs.entry, rd::Format::RGBA8_UNORM};
  if (!(renderer.init(*device, sd))) return {};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  if (!(target.valid())) return {};
  auto res = rd::MeshRenderResource::upload(*device, model);
  if (!((res) != (nullptr))) return {};
  rd::scene::Scene scene;
  auto node = std::make_unique<rd::scene::MeshNode>();
  node->mesh = res;
  scene.root().addChild(std::move(node));
  rd::scene::Camera cam;
  cam.lookAt({0, 0, 2}, {0, 0, 0}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);

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
  res->destroy(*device);
  renderer.shutdown();
    return img;
}
} // namespace

RD_GOLDEN_TEST(Ktx2Render, Golden, "ktx2_tri", 0.05, renderKtx2)
