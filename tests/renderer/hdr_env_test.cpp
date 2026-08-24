// HDR 环境 + 天空盒 golden:运行时生成渐变 hdr → setHdrEnvironment + 天空盒开。
#include <gtest/gtest.h>
#include "common/golden_test.h"
#include "common/image.h"
#include "common/shader_code.h"
#include "rd_shader_dir.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/hdr_env.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include <glm/glm.hpp>
#include <stb_image_write.h>

namespace {
constexpr uint32_t kW = 512, kH = 512;

/// 生成 64×32 渐变 HDR(蓝天顶/暖地平,值域 >1)。
std::string writeTestHdr() {
  const int W = 64, H = 32;
  std::vector<float> px(size_t(W) * H * 3);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const float v = float(y) / float(H - 1);  // 0=顶
      px[(size_t(y) * W + x) * 3 + 0] = (1.0f - v) * 0.3f + v * 2.0f;  // R 地平暖
      px[(size_t(y) * W + x) * 3 + 1] = 0.5f + 0.5f * (1.0f - v);
      px[(size_t(y) * W + x) * 3 + 2] = (1.0f - v) * 2.5f + v * 0.4f;  // B 天顶蓝
    }
  const std::string path = "/tmp/rd_golden_hdr.hdr";
  if (stbi_write_hdr(path.c_str(), W, H, 3, px.data()) != 1) return "";
  return path;
}

rd::test::Image renderHdrEnv(rd::Backend b) {
  const std::string hdrPath = writeTestHdr();
  if (hdrPath.empty()) return {};
  rd::HdrEnv hdr;
  if (!rd::loadHdrEnv(hdrPath.c_str(), hdr)) return {};
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto load = [&](const char* name) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, name); };
  auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
  auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
  auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
  auto eqFs = load("equirect_to_cube.frag");
  auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
  auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
  auto exFs = load("bloom_extract.frag");
  auto bbFs = load("bloom_blur.frag");
  auto cpFs = load("composite.frag");
  auto fxFs = load("fxaa.frag");
  auto skVs = load("pbr_forward_skinned.vert");
  auto sdsVs = load("shadow_depth_skinned.vert");
  auto skyVs = load("skybox.vert"), skyFs = load("skybox.frag");
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,
                            eqFs.code,   skyVs.code,  skyFs.code,{}, {}, {}, {}, {}, unlitVs.entry,
                            rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  auto model = rd::loadGltf(RD_TEST_DATA_DIR "/assets/DamagedHelmet.glb");
  if (!target.valid() || !model.valid() || !renderer.init(*device, sd)) return {};
  // HDR 环境 + 天空盒
  if (!renderer.setHdrEnvironment(&hdr)) return {};
  renderer.setSkyboxEnabled(true);
  auto res = rd::MeshRenderResource::upload(*device, model);
  if (!res) return {};
  rd::scene::Scene scene;
  auto node = std::make_unique<rd::scene::MeshNode>();
  node->mesh = res;
  scene.root().addChild(std::move(node));
  rd::scene::Camera cam;
  const float dist = model.boundingRadius * 2.5f;
  cam.lookAt({dist * 0.65f, dist * 0.35f, dist * 0.65f}, {0, 0, 0}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, dist * 0.1f, dist * 10.0f);
  device->beginFrame();
  renderer.beginScene(cam, {0.0f, 0.0f, 0.0f, 1.0f});  // 清黑(天空盒覆盖)
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
  if (!device->readbackTarget(target, img.pixels.data(), img.pixels.size())) return {};
  res->destroy(*device);
  renderer.shutdown();
  device->destroyTarget(target);
  return img;
}
} // namespace

RD_GOLDEN_TEST(HdrEnv, Golden, "hdr_env", 0.05, renderHdrEnv)
