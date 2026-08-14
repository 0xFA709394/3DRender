// ============================================================================
// rd_api.h 的实现：C API → C++ 内核的桥接层。
//
// rd_engine 持有：RHI 设备 + swapchain 句柄 + 演示场景（CubeScene）+ 动画状态 +
// 最近错误字符串。所有函数遵循头文件的线程约定（同一线程串行调用，无内部锁）。
// ============================================================================
#include "api/rd_api.h"
#include "api/embedded_shaders.h"
#include "foundation/log.h"
#include "rhi/rhi_device.h"
#include "scene/cube_scene.h"
#include <cstring>
#include <memory>

/// 引擎实例本体（C 侧不透明）。
struct rd_engine {
  std::unique_ptr<rd::Device> device;   ///< RHI 设备（后端在 create 时选定）
  rd::SwapChainHandle swapChain;        ///< 当前交换链（无表面时无效）
  rd::demo::CubeScene scene;            ///< 内置演示场景（旋转顶点色立方体）
  bool sceneReady = false;              ///< 场景资源是否已初始化（首次 set_surface 时完成）
  uint32_t width = 0, height = 0;       ///< 表面尺寸（render 时用于视口/投影）
  float angle = 0.0f;                   ///< 累计旋转角（弧度）
  char lastError[256] = {};             ///< 最近错误描述（rd_get_last_error 返回）
};

namespace {
/// 记录错误：写入 engine 的 lastError 并输出错误日志。
void setError(rd_engine* e, const char* msg) {
  std::strncpy(e->lastError, msg, sizeof(e->lastError) - 1);
  e->lastError[sizeof(e->lastError) - 1] = '\0';
  RD_LOGE("api", "%s", msg);
}
} // namespace

rd_engine* rd_engine_create(rd_backend_t backend) {
  rd::DeviceDesc desc;
  desc.backend = static_cast<rd::Backend>(backend);  // 枚举值与 rd_backend_t 一一对应
  auto device = rd::createDevice(desc);
  if (!device) return nullptr;
  auto* e = new rd_engine();
  e->device = std::move(device);
  return e;
}

void rd_engine_destroy(rd_engine* e) {
  if (!e) return;
  if (e->device) {
    e->device->waitIdle();  // 先等 GPU 空闲，再按依赖逆序释放
    if (e->sceneReady) e->scene.shutdown(*e->device);
    if (e->swapChain.valid()) e->device->destroySwapChain(e->swapChain);
  }
  delete e;
}

rd_result_t rd_engine_set_surface(rd_engine* e, void* nativeWindow, uint32_t width,
                                  uint32_t height) {
  if (!e || !nativeWindow || width == 0 || height == 0) return RD_ERROR_INVALID_ARG;
  // 重复调用：先销毁旧 swapchain（等 GPU 空闲防止在飞引用）
  if (e->swapChain.valid()) {
    e->device->waitIdle();
    e->device->destroySwapChain(e->swapChain);
    e->swapChain = rd::SwapChainHandle();
  }
  e->swapChain = e->device->createSwapChain(nativeWindow, width, height);
  if (!e->swapChain.valid()) {
    setError(e, "createSwapChain 失败");
    return RD_ERROR_SURFACE;
  }
  e->width = width;
  e->height = height;
  // 首次设置表面时初始化场景（shader 按后端取内嵌字节；须在 swapchain 创建之后，
  // 因为 Vulkan 后端可能在 createSwapChain 内重建了 render pass/表面格式）
  if (!e->sceneReady) {
    const uint8_t* vs = nullptr;
    const uint8_t* fs = nullptr;
    size_t vsSize = 0, fsSize = 0;
    rd::Backend b = e->device->backend();
    if (!rd::embeddedCubeShader(b, rd::ShaderStage::Vertex, &vs, &vsSize) ||
        !rd::embeddedCubeShader(b, rd::ShaderStage::Fragment, &fs, &fsSize)) {
      setError(e, "内嵌 shader 缺失");
      return RD_ERROR_SHADER;
    }
    // 入口名约定：Metal(metallib)="main0"（spirv-cross），SPIR-V/GLSL ES="main"
    const char* entry = (b == rd::Backend::Metal) ? "main0" : "main";
    // pipeline 颜色格式须与 swapchain 一致（Metal layer 为 BGRA8）
    const rd::Format surfaceFormat = e->device->swapChainColorFormat(e->swapChain);
    if (!e->scene.init(*e->device, vs, vsSize, fs, fsSize, entry, surfaceFormat)) {
      setError(e, "场景初始化失败");
      return RD_ERROR_SCENE;
    }
    e->sceneReady = true;
  }
  return RD_OK;
}

void rd_engine_clear_surface(rd_engine* e) {
  if (!e || !e->swapChain.valid()) return;
  e->device->waitIdle();
  e->device->destroySwapChain(e->swapChain);
  e->swapChain = rd::SwapChainHandle();
}

void rd_engine_resize(rd_engine* e, uint32_t width, uint32_t height) {
  if (!e || !e->swapChain.valid() || width == 0 || height == 0) return;
  e->width = width;
  e->height = height;
  e->device->resizeSwapChain(e->swapChain, width, height);
}

void rd_engine_render_frame(rd_engine* e, float dt) {
  // 无表面/场景未就绪：安全跳过（节流日志，约每 300 次记一次避免刷屏）
  if (!e || !e->swapChain.valid() || !e->sceneReady) {
    static int skipLog = 0;
    if (skipLog++ % 300 == 0)
      RD_LOGW("api", "render_frame 跳过: engine=%p swapChain=%d sceneReady=%d", (void*)e,
              e ? e->swapChain.value() : 0, e ? int(e->sceneReady) : -1);
    return;
  }
  e->angle += dt * 1.0f;  // 1 rad/s 的演示旋转速度
  e->device->beginFrame();  // 帧括号:驱动资源退休
  rd::TargetHandle target = e->device->acquireSwapChainTarget(e->swapChain);
  if (!target.valid()) {
    static int acqLog = 0;
    if (acqLog++ % 300 == 0) RD_LOGW("api", "acquireSwapChainTarget 失败");
    e->device->endFrame();
    return; // 表面重建中，跳过本帧
  }
  e->scene.render(*e->device, target, e->width, e->height, e->angle);
  e->device->present(e->swapChain);
  e->device->endFrame();
}

const char* rd_get_last_error(rd_engine* e) { return e ? e->lastError : ""; }
