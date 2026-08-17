// ============================================================================
// rd_api.h 的实现：C API → C++ 内核的桥接层。
//
// rd_engine 持有：RHI 设备 + swapchain 句柄 + Renderer/Scene/Camera +
// 当前模型资源 + 画质状态 + 最近错误字符串。所有函数遵循头文件的线程约定
// （同一线程串行调用，无内部锁）。
// ============================================================================
#include "api/rd_api.h"
#include "api/embedded_shaders.h"
#include "foundation/log.h"
#include "renderer/quality.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "rhi/rhi_device.h"
#include "scene/camera.h"
#include "scene/orbit_controller.h"
#include "scene/scene.h"
#include <cstring>
#include <memory>
#include <string>

/// 引擎实例本体（C 侧不透明）。
struct rd_engine {
  std::unique_ptr<rd::Device> device;   ///< RHI 设备（后端在 create 时选定）
  rd::SwapChainHandle swapChain;        ///< 当前交换链（无表面时无效）
  rd::Renderer renderer;                ///< 渲染器（首次 set_surface 时 init）
  std::unique_ptr<rd::scene::Scene> scene;  ///< 场景(load_gltf 后挂模型节点)
  rd::scene::Camera camera;             ///< 相机(每帧由 OrbitController 驱动)
  rd::scene::OrbitController orbit;     ///< Orbit 相机控制器(输入事件喂入)
  std::shared_ptr<rd::MeshRenderResource> model;  ///< 当前模型(无模型仅清屏)
  bool rendererReady = false;           ///< renderer 是否已初始化
  uint32_t width = 0, height = 0;       ///< 表面尺寸
  rd_quality_t quality = RD_QUALITY_AUTO;  ///< 配置档(AUTO 时按 caps 解析)
  std::vector<rd::LightData> manualLights;  ///< C API 灯(非空则覆盖 glTF 灯)
  std::vector<rd::LightData> gltfLights;    ///< glTF 解析灯(load_gltf 时存)
  bool shadowEnabled = true;
  bool lightsDirty = false;
  char lastError[256] = {};             ///< 最近错误描述（rd_get_last_error 返回）
};

namespace {
/// 记录错误：写入 engine 的 lastError 并输出错误日志。
void setError(rd_engine* e, const char* msg) {
  std::strncpy(e->lastError, msg, sizeof(e->lastError) - 1);
  e->lastError[sizeof(e->lastError) - 1] = '\0';
  RD_LOGE("api", "%s", msg);
}

/// 解析配置档(AUTO→caps 启发式)。
rd::QualityTier resolveTier(rd_engine* e) {
  if (e->quality != RD_QUALITY_AUTO)
    return e->quality == RD_QUALITY_HIGH   ? rd::QualityTier::High
           : e->quality == RD_QUALITY_MID  ? rd::QualityTier::Mid
                                           : rd::QualityTier::Low;
  return rd::qualityFromCaps(e->device->caps().get(rd::Capability::msaa),
                             e->device->caps().get(rd::Capability::max_texture_size));
}
/// 应用解析后的画质档到 renderer。
void applyQuality(rd_engine* e) {
  if (e->rendererReady) e->renderer.setQuality(rd::qualityPreset(resolveTier(e)));
}
/// 生效灯表:手动灯非空覆盖 glTF 灯;皆空 → 空(Renderer 默认灯兜底)。
const std::vector<rd::LightData>& activeLights(rd_engine* e) {
  return !e->manualLights.empty() ? e->manualLights : e->gltfLights;
}
} // namespace

rd_engine* rd_engine_create(rd_backend_t backend) {
  rd::DeviceDesc desc;
  desc.backend = static_cast<rd::Backend>(backend);  // 枚举值与 rd_backend_t 一一对应
  auto device = rd::createDevice(desc);
  if (!device) return nullptr;
  auto* e = new rd_engine();
  e->device = std::move(device);
  e->scene = std::make_unique<rd::scene::Scene>();
  // 初始取景(模型加载后由 frameModel 重取景)
  e->orbit.frameModel((const float[]){0, 0, 0}, 1.2f);
  return e;
}

void rd_engine_destroy(rd_engine* e) {
  if (!e) return;
  if (e->device) {
    e->device->waitIdle();  // 先等 GPU 空闲，再按依赖逆序释放
    if (e->model) e->model->destroy(*e->device);
    if (e->rendererReady) e->renderer.shutdown();
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
  // 首次设置表面时初始化渲染器（shader 按后端取内嵌字节；须在 swapchain 创建之后，
  // 因为 Vulkan 后端可能在 createSwapChain 内重建了 render pass/表面格式）
  if (!e->rendererReady) {
    const rd::Backend b = e->device->backend();
    const char* entry = (b == rd::Backend::Metal) ? "main0" : "main";
    auto get = [&](const char* name, rd::ShaderStage st, std::vector<uint8_t>& out) {
      const uint8_t* d = nullptr;
      size_t n = 0;
      if (!rd::embeddedShader(b, name, st, &d, &n)) return false;
      out.assign(d, d + n);
      return true;
    };
    rd::RendererShaderDesc sd;
    sd.entry = entry;
    // pipeline 颜色格式须与 swapchain 一致（Metal layer 为 BGRA8）
    sd.colorFormat = e->device->swapChainColorFormat(e->swapChain);
    if (!get("unlit", rd::ShaderStage::Vertex, sd.unlitVs) ||
        !get("unlit", rd::ShaderStage::Fragment, sd.unlitFs) ||
        !get("pbr_forward", rd::ShaderStage::Vertex, sd.pbrVs) ||
        !get("pbr_forward", rd::ShaderStage::Fragment, sd.pbrFs) ||
        !get("prefilter", rd::ShaderStage::Vertex, sd.prefilterVs) ||
        !get("prefilter", rd::ShaderStage::Fragment, sd.prefilterFs) ||
        !get("blit", rd::ShaderStage::Vertex, sd.blitVs) ||
        !get("blit", rd::ShaderStage::Fragment, sd.blitFs) ||
        !get("shadow_depth", rd::ShaderStage::Vertex, sd.shadowVs) ||
        !get("shadow_depth", rd::ShaderStage::Fragment, sd.shadowFs) ||
        !get("bloom_extract", rd::ShaderStage::Fragment, sd.extractFs) ||
        !get("bloom_blur", rd::ShaderStage::Fragment, sd.blurFs) ||
        !get("composite", rd::ShaderStage::Fragment, sd.compositeFs) ||
        !get("fxaa", rd::ShaderStage::Fragment, sd.fxaaFs) ||
        !get("pbr_forward_skinned", rd::ShaderStage::Vertex, sd.skinnedVs) ||
        !get("shadow_depth_skinned", rd::ShaderStage::Vertex, sd.skinnedShadowVs)) {
      setError(e, "内嵌 shader 缺失");
      return RD_ERROR_SHADER;
    }
    if (!e->renderer.init(*e->device, sd)) {
      setError(e, "渲染器初始化失败");
      return RD_ERROR_SCENE;
    }
    e->rendererReady = true;
    applyQuality(e);  // 初始画质:AUTO → caps 启发式
    e->renderer.setShadowEnabled(e->shadowEnabled);  // 同步(可能早于 surface 设置)
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
  // 无表面/渲染器未就绪：安全跳过（节流日志，约每 300 次记一次避免刷屏）
  if (!e || !e->swapChain.valid() || !e->rendererReady) {
    static int skipLog = 0;
    if (skipLog++ % 300 == 0)
      RD_LOGW("api", "render_frame 跳过: engine=%p swapChain=%d rendererReady=%d", (void*)e,
              e ? e->swapChain.value() : 0, e ? int(e->rendererReady) : -1);
    return;
  }
  e->device->beginFrame();  // 帧括号:驱动资源退休
  rd::TargetHandle target = e->device->acquireSwapChainTarget(e->swapChain);
  if (!target.valid()) {
    static int acqLog = 0;
    if (acqLog++ % 300 == 0) RD_LOGW("api", "acquireSwapChainTarget 失败");
    e->device->endFrame();
    return; // 表面重建中，跳过本帧
  }
  if (e->lightsDirty) {  // 灯表变更(手动/glTF)下发
    e->renderer.setLights(activeLights(e));
    e->lightsDirty = false;
  }
  e->orbit.update(dt);  // 惯性积分(无指针按下时生效)
  e->orbit.applyTo(e->camera);
  e->camera.setPerspective(0.78539816f, float(e->width) / float(e->height),
                          std::max(0.01f, e->orbit.distance() * 0.02f),
                          e->orbit.distance() * 20.0f);
  e->renderer.beginScene(e->camera, {0.05f, 0.05f, 0.06f, 1.0f});
  e->scene->collect(e->renderer);
  auto* cmd = e->device->acquireCommandBuffer();
  e->renderer.endScene(cmd, target);
  e->device->submit(cmd);
  e->device->present(e->swapChain);
  e->device->endFrame();
}

rd_result_t rd_engine_set_quality(rd_engine* e, rd_quality_t q) {
  if (!e) return RD_ERROR_INVALID_ARG;
  if (q != RD_QUALITY_AUTO && q != RD_QUALITY_HIGH && q != RD_QUALITY_MID &&
      q != RD_QUALITY_LOW)
    return RD_ERROR_INVALID_ARG;
  e->quality = q;
  applyQuality(e);
  return RD_OK;
}

rd_quality_t rd_engine_get_quality(rd_engine* e) {
  if (!e) return RD_QUALITY_LOW;
  switch (resolveTier(e)) {
    case rd::QualityTier::High: return RD_QUALITY_HIGH;
    case rd::QualityTier::Mid: return RD_QUALITY_MID;
    case rd::QualityTier::Low: return RD_QUALITY_LOW;
  }
  return RD_QUALITY_LOW;
}

void rd_engine_on_pointer(rd_engine* e, rd_pointer_action_t a, int32_t id, float x,
                          float y) {
  if (!e) return;
  switch (a) {
    case RD_POINTER_DOWN: e->orbit.onPointerDown(int(id), x, y); break;
    case RD_POINTER_MOVE: e->orbit.onPointerMove(int(id), x, y); break;
    case RD_POINTER_UP:
    case RD_POINTER_CANCEL: e->orbit.onPointerUp(int(id), x, y); break;
  }
}
void rd_engine_on_scroll(rd_engine* e, float dy) { if (e) e->orbit.onScroll(dy); }
void rd_engine_on_pinch(rd_engine* e, float r) { if (e) e->orbit.onPinch(r); }
void rd_engine_on_double_tap(rd_engine* e, float, float) {
  if (e) e->orbit.onDoubleTap();
}

rd_result_t rd_engine_load_gltf(rd_engine* e, const char* path) {
  if (!e || !path) return RD_ERROR_INVALID_ARG;
  // 纹理偏好:压缩目标按 caps,尺寸上限按当前画质档
  rd::TextureLoadPref pref;
  pref.ktx2Target = rd::pickTranscodeTarget(
      e->device->caps().supports(rd::Capability::texture_compression_astc),
      e->device->caps().supports(rd::Capability::texture_compression_etc2));
  pref.maxDim = e->rendererReady ? e->renderer.maxTextureDim() : 4096;
  auto model = rd::loadGltf(path, pref);
  if (!model.valid()) {
    setError(e, (std::string("glTF 加载失败: ") + path).c_str());
    return RD_ERROR_ASSET;
  }
  e->device->waitIdle();  // 防旧模型在飞引用
  auto res = rd::MeshRenderResource::upload(*e->device, model);
  if (!res) {
    setError(e, "模型 GPU 上传失败");
    return RD_ERROR_ASSET;
  }
  if (e->model) e->model->destroy(*e->device);
  e->model = res;
  // 重建场景:单 MeshNode
  auto scene = std::make_unique<rd::scene::Scene>();
  auto node = std::make_unique<rd::scene::MeshNode>();
  node->mesh = res;
  scene->root().addChild(std::move(node));
  e->scene = std::move(scene);
  e->orbit.frameModel(model.boundingCenter, model.boundingRadius);
  // 灯光 + 阴影取景(setLights/setLightFraming 只存 CPU 状态,随时可调)
  e->gltfLights = model.lights;
  e->lightsDirty = true;
  e->renderer.setLightFraming(model.boundingCenter, model.boundingRadius);
  return RD_OK;
}

void rd_engine_clear_lights(rd_engine* e) {
  if (!e) return;
  e->manualLights.clear();
  e->lightsDirty = true;
}

void rd_engine_add_dir_light(rd_engine* e, float dx, float dy, float dz, float r,
                             float g, float b, float intensity) {
  if (!e) return;
  rd::LightData l;
  l.type = rd::LightType::Directional;
  l.direction[0] = dx;
  l.direction[1] = dy;
  l.direction[2] = dz;
  l.color[0] = r * intensity;
  l.color[1] = g * intensity;
  l.color[2] = b * intensity;
  e->manualLights.push_back(l);
  e->lightsDirty = true;
}

void rd_engine_add_point_light(rd_engine* e, float px, float py, float pz, float range,
                               float r, float g, float b, float intensity) {
  if (!e) return;
  rd::LightData l;
  l.type = rd::LightType::Point;
  l.position[0] = px;
  l.position[1] = py;
  l.position[2] = pz;
  l.range = range;
  l.color[0] = r * intensity;
  l.color[1] = g * intensity;
  l.color[2] = b * intensity;
  e->manualLights.push_back(l);
  e->lightsDirty = true;
}

void rd_engine_add_spot_light(rd_engine* e, float px, float py, float pz, float dx,
                              float dy, float dz, float innerDeg, float outerDeg,
                              float range, float r, float g, float b, float intensity) {
  if (!e) return;
  rd::LightData l;
  l.type = rd::LightType::Spot;
  l.position[0] = px;
  l.position[1] = py;
  l.position[2] = pz;
  l.direction[0] = dx;
  l.direction[1] = dy;
  l.direction[2] = dz;
  l.innerCone = innerDeg * 0.0174532925f;
  l.outerCone = outerDeg * 0.0174532925f;
  l.range = range;
  l.color[0] = r * intensity;
  l.color[1] = g * intensity;
  l.color[2] = b * intensity;
  e->manualLights.push_back(l);
  e->lightsDirty = true;
}

void rd_engine_set_shadow_enabled(rd_engine* e, int en) {
  if (!e) return;
  e->shadowEnabled = en != 0;
  if (e->rendererReady) e->renderer.setShadowEnabled(e->shadowEnabled);
}

const char* rd_get_last_error(rd_engine* e) { return e ? e->lastError : ""; }
