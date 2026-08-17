// render_test --interactive 的实现(macOS):
// GLFW(NO_API 模式)出窗口/鼠标事件,CAMetalLayer 挂到窗口 contentView,
// 渲染全走 rd_engine C API(顺带验证 host swapchain 与 C API 输入路径)。
#include "tools/render_test/interactive.h"
#include "api/rd_api.h"
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
  rd_engine* engine = nullptr;
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
    rd_engine_on_pointer(c->engine, RD_POINTER_DOWN, 0, px, py);
    // 手动双击检测(<0.3s)
    const double now = glfwGetTime();
    if (now - c->lastClickTime < 0.3) rd_engine_on_double_tap(c->engine, px, py);
    c->lastClickTime = now;
  } else {
    c->dragging = false;
    rd_engine_on_pointer(c->engine, RD_POINTER_UP, 0, px, py);
  }
}
void onCursorPos(GLFWwindow* w, double x, double y) {
  Ctx* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
  if (!c->dragging) return;
  float sx, sy;
  glfwGetWindowContentScale(w, &sx, &sy);
  rd_engine_on_pointer(c->engine, RD_POINTER_MOVE, 0, float(x * sx), float(y * sy));
}
void onScroll(GLFWwindow* w, double, double dy) {
  Ctx* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
  rd_engine_on_scroll(c->engine, float(dy * 120.0));  // 滚轮刻度 → 像素量纲
}
} // namespace

int rd::tool::runInteractive(const char* modelPath) {
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
