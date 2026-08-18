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

namespace {
struct Ctx {
  rd_engine* engine = nullptr;              // C API 路径
  rd::scene::OrbitController* orbit = nullptr;  // 场景直驱路径
  bool dragging = false;
  double lastClickTime = 0;
};

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
    if (c->orbit) c->orbit->onPointerDown(0, px, py);
    else rd_engine_on_pointer(c->engine, RD_POINTER_DOWN, 0, px, py);
    const double now = glfwGetTime();
    if (now - c->lastClickTime < 0.3) {
      if (c->orbit) c->orbit->onDoubleTap();
      else rd_engine_on_double_tap(c->engine, px, py);
    }
    c->lastClickTime = now;
  } else {
    c->dragging = false;
    if (c->orbit) c->orbit->onPointerUp(0, px, py);
    else rd_engine_on_pointer(c->engine, RD_POINTER_UP, 0, px, py);
  }
}
void onCursorPos(GLFWwindow* w, double x, double y) {
  Ctx* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
  if (!c->dragging) return;
  float sx, sy;
  glfwGetWindowContentScale(w, &sx, &sy);
  if (c->orbit) c->orbit->onPointerMove(0, float(x * sx), float(y * sy));
  else rd_engine_on_pointer(c->engine, RD_POINTER_MOVE, 0, float(x * sx), float(y * sy));
}
void onScroll(GLFWwindow* w, double, double dy) {
  Ctx* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
  if (c->orbit) c->orbit->onScroll(float(dy * 120.0));
  else rd_engine_on_scroll(c->engine, float(dy * 120.0));  // 滚轮刻度 → 像素量纲
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

int rd::tool::runInteractive(const char* modelPath, const char* sceneName) {
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

  Ctx ctx;
  ctx.engine = engine;
  glfwSetWindowUserPointer(win, &ctx);
  glfwSetMouseButtonCallback(win, onMouseButton);
  glfwSetCursorPosCallback(win, onCursorPos);
  glfwSetScrollCallback(win, onScroll);

  const char* framesEnv = getenv("RD_INTERACTIVE_FRAMES");
  const long maxFrames = framesEnv ? atol(framesEnv) : 0;  // 0 = 不限
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
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    rd_engine_render_frame(engine, dt);
    if (maxFrames > 0 && ++frame >= maxFrames) break;
  }
  rd_engine_clear_surface(engine);
  rd_engine_destroy(engine);
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
