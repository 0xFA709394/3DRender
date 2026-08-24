// 输入回放回归:orbit_drag.log → OrbitController 驱动相机 → 离屏渲染 → 末帧 golden(SSIM)。
// 确定性:固定 dt 序列(日志时间戳差),不经真实时钟。
#include <gtest/gtest.h>
#include "common/golden_test.h"
#include "common/image.h"
#include "common/shader_code.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/orbit_controller.h"
#include "scene/scene.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr uint32_t kW = 512, kH = 512;

/// 日志事件:t(ms) action id x y(坐标归一化 0..1)。
struct Ev {
  float t;
  char action[16];
  int32_t id;
  float x, y;
};

std::vector<Ev> loadLog(const char* path, uint32_t& vpW, uint32_t& vpH) {
  std::vector<Ev> evs;
  FILE* f = fopen(path, "rb");
  if (!f) return evs;
  char line[128];
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#') {
      sscanf(line, "# viewport %u %u", &vpW, &vpH);
      continue;
    }
    Ev e{};
    if (sscanf(line, "%f %15s %d %f %f", &e.t, e.action, &e.id, &e.x, &e.y) >= 3)
      evs.push_back(e);
  }
  fclose(f);
  return evs;
}

/// 按日志驱动 OrbitController 渲染 helmet 末帧。
rd::test::Image renderReplay(rd::Backend b, const char* logName) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  uint32_t vpW = 0, vpH = 0;
  const std::string logPath = std::string(RD_TEST_DATA_DIR) + "/recordings/" + logName;
  auto evs = loadLog(logPath.c_str(), vpW, vpH);
  if (evs.empty() || vpW == 0) return {};
  auto load = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
  auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
  auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
  auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
  auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
  auto sdsVs = load("shadow_depth_skinned.vert");
  auto exFs = load("bloom_extract.frag");
  auto bbFs = load("bloom_blur.frag");
  auto cpFs = load("composite.frag");
  auto fxFs = load("fxaa.frag");
  auto skVs = load("pbr_forward_skinned.vert");
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,
                            {},{}, {},{}, {}, unlitVs.entry, rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  auto model = rd::loadGltf(RD_TEST_DATA_DIR "/assets/DamagedHelmet.glb");
  if (!target.valid() || !model.valid() || !renderer.init(*device, sd)) return {};
  auto res = rd::MeshRenderResource::upload(*device, model);
  if (!res) return {};
  rd::scene::Scene scene;
  auto node = std::make_unique<rd::scene::MeshNode>();
  node->mesh = res;
  scene.root().addChild(std::move(node));

  // Orbit:取景 + 逐事件回放(归一化坐标 → 像素)
  rd::scene::OrbitController orbit;
  orbit.frameModel(model.boundingCenter, model.boundingRadius);
  rd::scene::Camera cam;
  cam.setPerspective(0.78539816f, float(vpW) / float(vpH), 0.1f, 100.0f);
  float vt = 0.0f;
  size_t ei = 0;
  // 帧循环:每帧处理 vt 时刻前全部事件,推进到下一事件间隔;末事件后再渲 5 帧(惯性)
  const float lastT = evs.back().t;
  while (true) {
    while (ei < evs.size() && evs[ei].t <= vt) {
      const Ev& e = evs[ei++];
      const float px = e.x * float(vpW), py = e.y * float(vpH);
      if (!strcmp(e.action, "down")) orbit.onPointerDown(e.id, px, py);
      else if (!strcmp(e.action, "move")) orbit.onPointerMove(e.id, px, py);
      else if (!strcmp(e.action, "up")) orbit.onPointerUp(e.id, px, py);
      else if (!strcmp(e.action, "scroll")) orbit.onScroll(e.y);  // scroll 存 y 通道
      else if (!strcmp(e.action, "dtap")) orbit.onDoubleTap();
    }
    orbit.update(1.0f / 60.0f);
    orbit.applyTo(cam);
    if (vt > lastT + 5.0f / 60.0f * 5.0f) break;  // 末事件后再渲 5 帧
    vt += 1.0f / 60.0f * 1000.0f;                 // 虚拟时间 ms
  }
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
  res->destroy(*device);
  renderer.shutdown();
  return img;
}
// 宏签名适配(RenderFn = Image(Backend))
rd::test::Image renderReplayLog(rd::Backend b) {
  return renderReplay(b, "orbit_drag.log");
}
} // namespace

RD_GOLDEN_TEST(Replay, OrbitDrag, "replay_orbit", 0.05, renderReplayLog)
