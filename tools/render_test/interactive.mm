// render_test --interactive 的实现(macOS):
// GLFW(NO_API 模式)出窗口/鼠标事件,CAMetalLayer 挂到窗口 contentView。
// --model/默认:经 rd_engine C API(顺带验证 host swapchain 与 C API 输入路径);
// --scene:demo 场景直驱(Renderer + OrbitController,不经 C API)。
#include "tools/render_test/interactive.h"
#include "api/rd_api.h"
#include "tools/render_test/scenes.h"
#include "common/shader_code.h"
#include "renderer/renderer.h"
#include "rhi/rhi_device.h"
#include "scene/orbit_controller.h"
#include "rd_shader_dir.h"
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
struct Ctx {
  rd_engine* engine = nullptr;              // C API 路径
  rd::scene::OrbitController* orbit = nullptr;  // 场景直驱路径
  bool dragging = false;
  double lastClickTime = 0;
  FILE* recordFile = nullptr;               // 录制输出(非空则写事件)
  int winW = 960, winH = 720;
};

/// 录制一行:t(ms) action id x y(归一化)。
void recordEvent(Ctx* c, const char* action, int id, float px, float py) {
  if (!c->recordFile) return;
  const long long t = (long long)(glfwGetTime() * 1000.0);
  fprintf(c->recordFile, "%lld %s %d %.4f %.4f\n", t, action, id, px / c->winW,
          py / c->winH);
  fflush(c->recordFile);
}

void onMouseButton(GLFWwindow* w, int button, int action, int) {
  Ctx* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
  if (button != GLFW_MOUSE_BUTTON_LEFT) return;
  double x, y;
  glfwGetCursorPos(w, &x, &y);
  float sx, sy;
  glfwGetWindowContentScale(w, &sx, &sy);
  const float px = float(x * sx), py = float(y * sy);
  if (action == GLFW_PRESS) {
    c->dragging = true;
    recordEvent(c, "down", 0, px, py);
    if (c->orbit) c->orbit->onPointerDown(0, px, py);
    else rd_engine_on_pointer(c->engine, RD_POINTER_DOWN, 0, px, py);
    const double now = glfwGetTime();
    if (now - c->lastClickTime < 0.3) {
      recordEvent(c, "dtap", 0, px, py);
      if (c->orbit) c->orbit->onDoubleTap();
      else rd_engine_on_double_tap(c->engine, px, py);
    }
    c->lastClickTime = now;
  } else {
    c->dragging = false;
    recordEvent(c, "up", 0, px, py);
    if (c->orbit) c->orbit->onPointerUp(0, px, py);
    else rd_engine_on_pointer(c->engine, RD_POINTER_UP, 0, px, py);
  }
}
void onCursorPos(GLFWwindow* w, double x, double y) {
  Ctx* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
  if (!c->dragging) return;
  float sx, sy;
  glfwGetWindowContentScale(w, &sx, &sy);
  recordEvent(c, "move", 0, float(x * sx), float(y * sy));
  if (c->orbit) c->orbit->onPointerMove(0, float(x * sx), float(y * sy));
  else rd_engine_on_pointer(c->engine, RD_POINTER_MOVE, 0, float(x * sx), float(y * sy));
}
void onScroll(GLFWwindow* w, double, double dy) {
  Ctx* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
  recordEvent(c, "scroll", 0, float(dy * 120.0), 0);  // y 通道存像素量纲
  if (c->orbit) c->orbit->onScroll(float(dy * 120.0));
  else rd_engine_on_scroll(c->engine, float(dy * 120.0));
}

/// 场景直驱路径(Renderer + DemoScene + OrbitController)。
int runSceneInteractive(GLFWwindow* win, CAMetalLayer* layer, const char* sceneName) {
  rd::DeviceDesc dd;
  dd.backend = rd::Backend::Metal;
  auto device = rd::createDevice(dd);
  if (!device) return 1;
  auto swap = device->createSwapChain((__bridge void*)layer, 960, 720);
  if (!swap.valid()) return 1;
  const rd::Format scFmt = device->swapChainColorFormat(swap);
  auto load = [&](const char* n) {
    return rd::test::loadShaderCode(rd::Backend::Metal, RD_SHADER_DIR, n);
  };
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
                            unlitVs.entry, scFmt};
  if (!renderer.init(*device, sd)) return 1;
  rd::ModelAsset storage;
  rd::tool::DemoScene scene;
  if (!rd::tool::buildDemoScene(sceneName, *device, renderer, scene, storage)) {
    fprintf(stderr, "场景构建失败: %s\n", sceneName);
    return 1;
  }
  renderer.setQuality(rd::qualityPreset(rd::QualityTier::High));

  rd::scene::OrbitController orbit;
  orbit.frameModel(scene.framingCenter, scene.framingRadius);
  Ctx ctx;
  ctx.orbit = &orbit;
  glfwSetWindowUserPointer(win, &ctx);

  const char* framesEnv = getenv("RD_INTERACTIVE_FRAMES");
  const long maxFrames = framesEnv ? atol(framesEnv) : 0;
  auto last = std::chrono::steady_clock::now();
  long frame = 0;
  while (!glfwWindowShouldClose(win)) {
    glfwPollEvents();
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(win, &fbw, &fbh);
    CGSize cur = layer.drawableSize;
    if (int(cur.width) != fbw || int(cur.height) != fbh) {
      layer.drawableSize = CGSizeMake(fbw, fbh);
      device->resizeSwapChain(swap, uint32_t(fbw), uint32_t(fbh));
    }
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    orbit.update(dt);
    orbit.applyTo(scene.camera);
    scene.camera.setPerspective(0.78539816f, float(fbw) / float(fbh),
                                std::max(0.01f, orbit.distance() * 0.02f),
                                orbit.distance() * 20.0f);
    device->beginFrame();
    rd::TargetHandle target = device->acquireSwapChainTarget(swap);
    if (target.valid()) {
      renderer.beginScene(scene.camera, {0.05f, 0.05f, 0.06f, 1.0f});
      rd::tool::submitDemoScene(scene, renderer, dt);
      auto* cmd = device->acquireCommandBuffer();
      renderer.endScene(cmd, target);
      device->submit(cmd);
      device->present(swap);
    }
    device->endFrame();
    if (maxFrames > 0 && ++frame >= maxFrames) break;
  }
  rd::tool::destroyDemoScene(scene, *device);
  renderer.shutdown();
  device->destroySwapChain(swap);
  return 0;
}
} // namespace

int rd::tool::runInteractive(const char* modelPath, const char* sceneName,
                             const char* recordPath, const char* playPath,
                             const char* scriptPath) {
  if (!glfwInit()) {
    fprintf(stderr, "glfwInit 失败(headless 环境?)\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // 渲染走 rd_engine,不需 GL 上下文
  GLFWwindow* win = glfwCreateWindow(960, 720, "render_test --interactive", nullptr, nullptr);
  if (!win) {
    glfwTerminate();
    return 1;
  }
  NSWindow* nswin = glfwGetCocoaWindow(win);
  NSView* view = [nswin contentView];
  [view setWantsLayer:YES];
  CAMetalLayer* layer = [CAMetalLayer layer];
  [view setLayer:layer];

  // demo 场景直驱路径
  if (sceneName && sceneName[0]) {
    const int rc = runSceneInteractive(win, layer, sceneName);
    glfwDestroyWindow(win);
    glfwTerminate();
    return rc;
  }

  rd_engine* engine = rd_engine_create(RD_BACKEND_METAL);
  if (!engine) {
    glfwDestroyWindow(win);
    glfwTerminate();
    return 1;
  }
  int fbw = 0, fbh = 0;
  glfwGetFramebufferSize(win, &fbw, &fbh);
  layer.drawableSize = CGSizeMake(fbw, fbh);
  if (rd_engine_set_surface(engine, (__bridge void*)layer, uint32_t(fbw),
                            uint32_t(fbh)) != RD_OK) {
    fprintf(stderr, "set_surface 失败: %s\n", rd_get_last_error(engine));
    rd_engine_destroy(engine);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 1;
  }
  if (modelPath && modelPath[0] &&
      rd_engine_load_gltf(engine, modelPath) != RD_OK)
    fprintf(stderr, "load_gltf 失败: %s\n", rd_get_last_error(engine));
  if (scriptPath && scriptPath[0] &&
      rd_engine_exec_script(engine, scriptPath) != RD_OK)
    fprintf(stderr, "脚本执行失败: %s\n", rd_get_last_error(engine));

  Ctx ctx;
  ctx.engine = engine;
  if (recordPath && recordPath[0]) {
    ctx.recordFile = fopen(recordPath, "wb");
    if (ctx.recordFile) fprintf(ctx.recordFile, "# viewport %d %d\n", fbw, fbh);
  }
  glfwSetWindowUserPointer(win, &ctx);
  glfwSetMouseButtonCallback(win, onMouseButton);
  glfwSetCursorPosCallback(win, onCursorPos);
  glfwSetScrollCallback(win, onScroll);

  // --play:读日志事件,按虚拟时间轴注入(固定 1/60 dt,确定性)
  struct PlayEv {
    float t;
    std::string action;
    int id;
    float x, y;
  };
  std::vector<PlayEv> playEvs;
  if (playPath && playPath[0]) {
    FILE* pf = fopen(playPath, "rb");
    if (!pf) {
      fprintf(stderr, "play 日志打开失败: %s\n", playPath);
      return 2;
    }
    char line[128];
    while (fgets(line, sizeof(line), pf)) {
      if (line[0] == '#') continue;
      PlayEv e{};
      char act[16] = {};
      if (sscanf(line, "%f %15s %d %f %f", &e.t, act, &e.id, &e.x, &e.y) >= 3) {
        e.action = act;
        playEvs.push_back(std::move(e));
      }
    }
    fclose(pf);
    fprintf(stderr, "[play] 载入 %zu 事件\n", playEvs.size());
  }
  size_t playIdx = 0;
  float virtualMs = 0;

  const char* framesEnv = getenv("RD_INTERACTIVE_FRAMES");
  const long maxFrames = framesEnv ? atol(framesEnv) : 0;  // 0 = 不限
  // RD_DEMO_CYCLE=N:每 N 帧轮换画质档+模型(切换路径复现用)
  const char* cycleEnv = getenv("RD_DEMO_CYCLE");
  const long cycleFrames = cycleEnv ? atol(cycleEnv) : 0;
  const rd_quality_t tiers[3] = {RD_QUALITY_HIGH, RD_QUALITY_MID, RD_QUALITY_LOW};
  auto last = std::chrono::steady_clock::now();
  long frame = 0;
  while (!glfwWindowShouldClose(win)) {
    glfwPollEvents();
    glfwGetFramebufferSize(win, &fbw, &fbh);
    CGSize cur = layer.drawableSize;
    if (int(cur.width) != fbw || int(cur.height) != fbh) {
      layer.drawableSize = CGSizeMake(fbw, fbh);
      rd_engine_resize(engine, uint32_t(fbw), uint32_t(fbh));
    }
    // 回放注入(在渲染前;坐标归一化 → 像素)
    while (playIdx < playEvs.size() && playEvs[playIdx].t <= virtualMs) {
      const PlayEv& e = playEvs[playIdx++];
      const float px = e.x * float(fbw), py = e.y * float(fbh);
      if (e.action == "down") rd_engine_on_pointer(engine, RD_POINTER_DOWN, e.id, px, py);
      else if (e.action == "move") rd_engine_on_pointer(engine, RD_POINTER_MOVE, e.id, px, py);
      else if (e.action == "up") rd_engine_on_pointer(engine, RD_POINTER_UP, e.id, px, py);
      else if (e.action == "scroll") rd_engine_on_scroll(engine, e.y);
      else if (e.action == "dtap") rd_engine_on_double_tap(engine, px, py);
    }
    virtualMs += 1000.0f / 60.0f;
    if (cycleFrames > 0 && frame > 0 && frame % cycleFrames == 0) {
      const long step = frame / cycleFrames;
      rd_engine_set_quality(engine, tiers[step % 3]);
      if (modelPath && step % 2 == 1) {  // 隔轮重载模型
        if (rd_engine_load_gltf(engine, modelPath) != RD_OK)
          fprintf(stderr, "cycle load_gltf 失败: %s\n", rd_get_last_error(engine));
      }
      fprintf(stderr, "[cycle] frame=%ld tier=%d\n", frame, int(step % 3));
    }
    // 回放模式:固定 dt(与注入时间轴一致);否则真实 dt
    const auto now = std::chrono::steady_clock::now();
    const float dt = playEvs.empty() ? std::chrono::duration<float>(now - last).count()
                                     : 1.0f / 60.0f;
    last = now;
    rd_engine_render_frame(engine, dt);
    if (maxFrames > 0 && ++frame >= maxFrames) break;
    // 回放结束:日志耗尽再渲 5 帧收尾即退出
    if (!playEvs.empty() && playIdx >= playEvs.size()) {
      static int tail = 0;
      if (++tail > 5) break;
    }
  }
  rd_engine_clear_surface(engine);
  rd_engine_destroy(engine);
  if (ctx.recordFile) fclose(ctx.recordFile);
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
