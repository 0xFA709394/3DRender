// ============================================================================
// rd_api.h 的实现：C API → C++ 内核的桥接层。
//
// rd_engine 持有：RHI 设备 + swapchain 句柄 + Renderer/Scene/Camera +
// 当前模型资源 + 画质状态 + 最近错误字符串。所有函数遵循头文件的线程约定
// （同一线程串行调用，无内部锁）。
// ============================================================================
#include "api/rd_api.h"
#include "api/command_bus.h"
#include "api/embedded_shaders.h"
#include "foundation/log.h"
#include <atomic>
#include <filesystem>
#include <thread>
#include "foundation/task_queue.h"
#include "options_generated.h"
#include "renderer/quality.h"
#include "renderer/renderer.h"
#include "renderer/water.h"
#include "resource/gltf_loader.h"
#include "resource/hdr_env.h"
#include "resource/mesh_render_resource.h"
#include "resource/primitives.h"
#include "rhi/rhi_device.h"
#include "scene/animator.h"
#include "scene/camera.h"
#include "scene/orbit_controller.h"
#include "scene/picking.h"
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
  bool renderDirty = true;  ///< 按需渲染脏标记(初始 true,首帧必渲)
  rd::Options options;                  ///< 声明式选项(render_frame 映射进 renderer)
  rd::HdrEnv hdrEnv;                    ///< HDR 环境源(引擎持有,重建期指针有效)
  rd::CommandBus bus;                   ///< 命令总线(create 时注册内建命令)
  char cmdOutput[256] = {};             ///< 最近一次命令输出(get 等)
  rd::ModelAsset modelAsset;            ///< 当前模型 CPU 资产(Animator 绑定源)
  std::vector<float> morphOverride_;    ///< 手动 morph 权重(加载时=静态初始值)
  rd::scene::Animator animator;
  bool hasAnimation = false;
  // ---- water_pool 程序场景 ----
  struct WaterScene {
    bool active = false;
    std::shared_ptr<rd::MeshRenderResource> surface;
    std::vector<std::shared_ptr<rd::MeshRenderResource>> receivers;
    std::vector<rd::math::Mat4> worlds;
    float rainTimer = 0.4f;
    uint32_t rainSeed = 0x9E3779B9u;  // LCG(确定性)
  } waterScene;
  char lastError[256] = {};             ///< 最近错误描述（rd_get_last_error 返回）
  // 异步加载:工作线程解析;完成队列由 render_frame 消费(含无 surface 早退前)
  rd::TaskQueue loadWork;               ///< 渲染线程投递 → 工作线程消费
  rd::TaskQueue loadDone;               ///< 工作线程投递 → 渲染线程消费
  std::unique_ptr<std::thread> loadThread;
  std::atomic<bool> loadQuit{false};
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
/// 当前生效相机(无 surface 时用 512×512 默认投影,pick/测试可用)。
void applyCamera(rd_engine* e, float vw, float vh) {
  e->orbit.applyTo(e->camera);
  e->camera.setPerspective(e->options.camera.fov_deg * 0.0174532925f, vw / vh,
                          std::max(0.01f, e->orbit.distance() * 0.02f),
                          e->orbit.distance() * 20.0f);
}
/// 安装模型:GPU 上传 + 场景重建 + 取景 + 灯光 + 动画(渲染线程)。
void installModel(rd_engine* e, rd::ModelAsset&& model) {
  e->device->waitIdle();  // 防旧模型在飞引用
  if (e->waterScene.active) {  // 模型替换程序场景:卸载水
    e->waterScene.active = false;
    e->waterScene.receivers.clear();
    e->waterScene.worlds.clear();
    e->waterScene.surface = nullptr;
    e->renderer.disableWater();
  }
  auto res = rd::MeshRenderResource::upload(*e->device, model);
  if (!res) {
    setError(e, "模型 GPU 上传失败");
    return;
  }
  if (e->model) e->model->destroy(*e->device);
  e->model = res;
  auto scene = std::make_unique<rd::scene::Scene>();
  auto node = std::make_unique<rd::scene::MeshNode>();
  node->mesh = res;
  scene->root().addChild(std::move(node));
  e->scene = std::move(scene);
  e->orbit.frameModel(model.boundingCenter, model.boundingRadius);
  e->gltfLights = model.lights;
  e->lightsDirty = true;
  e->renderer.setLightFraming(model.boundingCenter, model.boundingRadius);
  e->modelAsset = std::move(model);
  const bool hasMorph = !e->modelAsset.meshes.empty() && e->modelAsset.meshes[0].morph;
  e->hasAnimation = !e->modelAsset.animations.empty() &&
                    (!e->modelAsset.skins.empty() || hasMorph);
  e->morphOverride_.clear();
  if (hasMorph) {
    e->morphOverride_ = e->modelAsset.meshes[0].morphWeights;
    e->morphOverride_.resize(std::min<size_t>(e->morphOverride_.size(), 8), 0.0f);
  }
  if (e->hasAnimation) {
    e->animator.bind(e->modelAsset);
    e->animator.play(0);
  }
  e->renderDirty = true;
}

/// water_pool 池场景:池底/四壁/两球一柱(受水体)+ 水面网格。
/// 布局常量与 tools/render_test/scenes.cpp 的 buildWaterPool 保持一致(池 4×4,底 y=-1)。
bool buildWaterPoolScene(rd_engine* e) {
  if (!e->rendererReady) {
    setError(e, "load_scene 需 surface 就绪后调用");
    return false;
  }
  // 卸载模型路径状态
  if (e->model) {
    e->device->waitIdle();
    e->model->destroy(*e->device);
    e->model = nullptr;
  }
  e->scene = std::make_unique<rd::scene::Scene>();
  e->modelAsset = rd::ModelAsset();
  e->hasAnimation = false;
  e->animator = rd::scene::Animator();
  auto& ws = e->waterScene;
  ws.receivers.clear();
  ws.worlds.clear();
  auto put = [&](rd::MeshData&& m, const rd::math::Mat4& w) {
    rd::ModelAsset a;
    a.meshes.push_back(std::move(m));
    a.boundingRadius = 3.0f;
    auto res = rd::MeshRenderResource::upload(*e->device, a);
    if (!res) return false;
    ws.receivers.push_back(res);
    ws.worlds.push_back(w);
    return true;
  };
  const rd::math::Mat4 I(1.0f);
  // 池底(y=-1)
  auto floor = rd::primitives::makePlane(4.0f, 4.0f);
  floor.material.roughnessFactor = 0.9f;
  floor.material.baseColorFactor[0] = floor.material.baseColorFactor[1] =
      floor.material.baseColorFactor[2] = 0.72f;
  if (!put(std::move(floor), glm::translate(I, rd::math::Vec3(0, -1.0f, 0)))) return false;
  // 四壁(薄盒 4×1.25×0.1,y -1..0.25;左右壁绕 Y 转 90°)
  const rd::math::Vec3 wallPos[4] = {{0, -0.375f, -2.0f}, {0, -0.375f, 2.0f},
                                     {-2.0f, -0.375f, 0}, {2.0f, -0.375f, 0}};
  for (int i = 0; i < 4; ++i) {
    auto wm = rd::primitives::makeBox(4.0f, 1.25f, 0.1f);
    wm.material.roughnessFactor = 0.85f;
    wm.material.baseColorFactor[0] = 0.8f;
    wm.material.baseColorFactor[1] = 0.78f;
    wm.material.baseColorFactor[2] = 0.74f;
    rd::math::Mat4 w = glm::translate(I, wallPos[i]);
    if (i >= 2) w = w * glm::rotate(I, 1.5707963f, rd::math::Vec3(0, 1, 0));
    if (!put(std::move(wm), w)) return false;
  }
  // 两球 + 一柱(水下物体)
  auto sph = rd::primitives::makeSphere(0.4f, 32, 16);
  sph.material.roughnessFactor = 0.4f;
  sph.material.baseColorFactor[2] = 0.9f;
  if (!put(std::move(sph), glm::translate(I, rd::math::Vec3(-0.9f, -0.6f, -0.6f))))
    return false;
  auto sph2 = rd::primitives::makeSphere(0.4f, 32, 16);
  sph2.material.roughnessFactor = 0.4f;
  sph2.material.baseColorFactor[0] = 0.9f;
  if (!put(std::move(sph2), glm::translate(I, rd::math::Vec3(0.9f, -0.55f, 0.7f))))
    return false;
  auto col = rd::primitives::makeBox(0.5f, 1.6f, 0.5f);
  col.material.roughnessFactor = 0.7f;
  if (!put(std::move(col), glm::translate(I, rd::math::Vec3(0.1f, -0.2f, -1.2f))))
    return false;
  // 水面网格
  auto surf = rd::primitives::makeGrid(4.0f, 128);
  surf.material.alphaBlend = true;
  surf.material.roughnessFactor = 0.05f;
  surf.material.baseColorFactor[0] = 0.15f;
  surf.material.baseColorFactor[1] = 0.35f;
  surf.material.baseColorFactor[2] = 0.4f;
  rd::ModelAsset sm;
  sm.meshes.push_back(std::move(surf));
  sm.boundingRadius = 3.0f;
  ws.surface = rd::MeshRenderResource::upload(*e->device, sm);
  if (!ws.surface) return false;
  // 水系统(默认 desc;simSize/caustics 走画质档联动)
  rd::WaterDesc wd;
  if (!e->renderer.enableWater(wd)) {
    setError(e, "水面不可用(caps/shader 缺失)");
    return false;
  }
  ws.active = true;
  // 取景 + 灯光(暖方向光)
  e->orbit.frameModel((const float[]){0, -0.3f, 0}, 3.2f);
  e->manualLights.clear();
  rd::LightData dl;
  dl.type = rd::LightType::Directional;
  const float n = std::sqrt(0.3f * 0.3f + 1.0f + 0.45f * 0.45f);
  dl.direction[0] = 0.3f / n;
  dl.direction[1] = 1.0f / n;
  dl.direction[2] = 0.45f / n;
  dl.color[0] = 3.2f;
  dl.color[1] = 3.0f;
  dl.color[2] = 2.7f;
  e->manualLights.push_back(dl);
  e->gltfLights.clear();
  e->lightsDirty = true;
  e->renderer.setLightFraming((const float[]){0, -0.5f, 0}, 2.8f);
  e->renderDirty = true;
  return true;
}

/// 把 options 映射进 renderer(每帧开头;幂等——setQuality/setShadow* 内部按值去重)。
void applyOptions(rd_engine* e) {
  if (!e->rendererReady) return;
  auto& o = e->options;
  // tier 字符串落到 e->quality(AUTO 时 resolveTier 走启发式)
  if (o.quality.tier == "high") e->quality = RD_QUALITY_HIGH;
  else if (o.quality.tier == "mid") e->quality = RD_QUALITY_MID;
  else if (o.quality.tier == "low") e->quality = RD_QUALITY_LOW;
  else e->quality = RD_QUALITY_AUTO;
  // 组合档:post=preset&&opt;fxaa=preset||opt;ibl/阴影尺寸按选项覆盖
  const rd::QualityTier tier = resolveTier(e);
  rd::QualityPreset preset = rd::qualityPreset(tier);
  preset.postEnabled = (preset.postEnabled != 0 && o.quality.post) ? 1 : 0;
  preset.fxaaEnabled = (preset.fxaaEnabled != 0 || o.quality.fxaa) ? 1 : 0;
  preset.iblPrefilterSize = uint32_t(o.ibl.prefilter_size);
  preset.iblPrefilterMips = uint32_t(o.ibl.prefilter_mips);
  if (o.shadow.map_size > 0) preset.shadowMapSize = uint32_t(o.shadow.map_size);
  e->renderer.setQuality(preset);
  e->renderer.setShadowEnabled(o.quality.shadow && e->shadowEnabled);
  e->renderer.setExposure(o.render.exposure);
  e->renderer.setShadowBias(o.shadow.bias);
  e->renderer.setSpotShadowEnabled(o.shadow.spot);
  e->renderer.setExtMaterialsEnabled(o.render.ext_materials);
  e->renderer.setTransmissionEnabled(o.render.transmission);
  e->renderer.setSkyboxEnabled(o.env.skybox);
  e->renderer.setFrustumCulling(o.render.frustum_culling);
  e->renderer.setEnvYaw(o.env.yaw_deg);
  rd::WaterParams wp;
  wp.waveScale = o.water.wave_scale;
  wp.causticsIntensity = o.water.caustics_intensity;
  wp.depth = o.water.depth;
  e->renderer.setWaterParams(wp);
}

/// increase/decrease:range 域按 step 增减并钳制。
bool optionsStep(rd_engine* e, const std::string& name, int dir) {
  double mn, mx, step;
  if (!rd::optionsRange(name, mn, mx, step)) return false;
  std::string cur;
  if (!rd::optionsGet(e->options, name, cur)) return false;
  double v = std::stod(cur) + dir * step;
  v = std::max(mn, std::min(mx, v));
  std::string s = std::to_string(v);
  s.erase(s.find_last_not_of('0') + 1);
  if (!s.empty() && s.back() == '.') s.pop_back();
  return rd::optionsSet(e->options, name, s);
}
/// cycle:enum 域循环。
bool optionsCycle(rd_engine* e, const std::string& name) {
  std::vector<std::string> vals;
  if (!rd::optionsEnumValues(name, vals) || vals.empty()) return false;
  std::string cur;
  if (!rd::optionsGet(e->options, name, cur)) return false;
  for (size_t i = 0; i < vals.size(); ++i)
    if (vals[i] == cur)
      return rd::optionsSet(e->options, name, vals[(i + 1) % vals.size()]);
  return false;
}

/// 注册内建命令(选项族 + 引擎族)。
void registerCommands(rd_engine* e) {
  auto& bus = e->bus;
  // ---- 选项族 ----
  bus.add("set", [e](const std::string& a, std::string&) {
    const auto sp = a.find(' ');
    if (sp == std::string::npos) return false;
    return rd::optionsSet(e->options, a.substr(0, sp), a.substr(sp + 1));
  });
  bus.add("get", [e](const std::string& a, std::string& out) {
    return rd::optionsGet(e->options, a, out);
  });
  bus.add("toggle", [e](const std::string& a, std::string&) {
    std::string cur;
    if (!rd::optionsGet(e->options, a, cur)) return false;
    if (cur != "true" && cur != "false") return false;
    return rd::optionsSet(e->options, a, cur == "true" ? "false" : "true");
  });
  bus.add("increase", [e](const std::string& a, std::string&) {
    return optionsStep(e, a, +1);
  });
  bus.add("decrease", [e](const std::string& a, std::string&) {
    return optionsStep(e, a, -1);
  });
  bus.add("cycle", [e](const std::string& a, std::string&) {
    return optionsCycle(e, a);
  });
  // ---- 引擎族 ----
  bus.add("load_model", [e](const std::string& a, std::string&) {
    return rd_engine_load_gltf(e, a.c_str()) == RD_OK;
  });
  bus.add("load_scene", [e](const std::string& a, std::string&) {
    return rd_engine_load_scene(e, a.c_str()) == RD_OK;
  });
  bus.add("water_disturb", [e](const std::string& a, std::string&) {
    const auto sp = a.find(' ');
    if (sp == std::string::npos) return false;
    rd_engine_water_disturb(e, float(atof(a.substr(0, sp).c_str())),
                            float(atof(a.substr(sp + 1).c_str())));
    return true;
  });
  bus.add("play_animation", [e](const std::string& a, std::string&) {
    rd_engine_play_animation(e, atoi(a.c_str()));
    return true;
  });
  bus.add("crossfade_animation", [e](const std::string& a, std::string&) {
    const auto sp = a.find(' ');
    if (sp == std::string::npos) return false;
    rd_engine_crossfade_animation(e, atoi(a.substr(0, sp).c_str()),
                                  float(atof(a.substr(sp + 1).c_str())));
    return true;
  });
  bus.add("pause_animation", [e](const std::string& a, std::string&) {
    rd_engine_pause_animation(e, atoi(a.c_str()));
    return true;
  });
  bus.add("quality", [e](const std::string& a, std::string&) {
    return rd::optionsSet(e->options, "quality.tier", a);
  });
  bus.add("reset_view", [e](const std::string&, std::string&) {
    e->orbit.onDoubleTap();
    return true;
  });
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
  registerCommands(e);  // 命令总线内建命令
  // 初始取景(模型加载后由 frameModel 重取景)
  e->orbit.frameModel((const float[]){0, 0, 0}, 1.2f);
  return e;
}

void rd_engine_destroy(rd_engine* e) {
  if (!e) return;
  // 停异步加载工作线程(置退标志 + 投递哨兵唤醒 + join;完成队列直接丢弃)
  if (e->loadThread) {
    e->loadQuit = true;
    e->loadWork.post([] {});
    e->loadThread->join();
  }
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
        !get("equirect_to_cube", rd::ShaderStage::Fragment, sd.equirectFs) ||
        !get("skybox", rd::ShaderStage::Vertex, sd.skyboxVs) ||
        !get("skybox", rd::ShaderStage::Fragment, sd.skyboxFs) ||
        !get("pbr_forward_instanced", rd::ShaderStage::Vertex, sd.instancedVs) ||
        !get("pbr_forward_instanced", rd::ShaderStage::Fragment, sd.instancedFs) ||
        !get("shadow_depth_mask", rd::ShaderStage::Vertex, sd.shadowMaskVs) ||
        !get("shadow_depth_mask", rd::ShaderStage::Fragment, sd.shadowMaskFs) ||
        !get("shadow_depth_instanced", rd::ShaderStage::Vertex, sd.shadowInstVs) ||
        !get("blit", rd::ShaderStage::Vertex, sd.blitVs) ||
        !get("blit", rd::ShaderStage::Fragment, sd.blitFs) ||
        !get("shadow_depth", rd::ShaderStage::Vertex, sd.shadowVs) ||
        !get("shadow_depth", rd::ShaderStage::Fragment, sd.shadowFs) ||
        !get("bloom_extract", rd::ShaderStage::Fragment, sd.extractFs) ||
        !get("bloom_blur", rd::ShaderStage::Fragment, sd.blurFs) ||
        !get("composite", rd::ShaderStage::Fragment, sd.compositeFs) ||
        !get("fxaa", rd::ShaderStage::Fragment, sd.fxaaFs) ||
        !get("pbr_forward_skinned", rd::ShaderStage::Vertex, sd.skinnedVs) ||
        !get("shadow_depth_skinned", rd::ShaderStage::Vertex, sd.skinnedShadowVs) ||
        !get("pbr_forward_morph", rd::ShaderStage::Vertex, sd.morphVs) ||
        !get("pbr_forward_morph_skinned", rd::ShaderStage::Vertex, sd.morphSkinnedVs) ||
        !get("shadow_depth_morph", rd::ShaderStage::Vertex, sd.morphShadowVs) ||
        !get("shadow_depth_morph_skinned", rd::ShaderStage::Vertex, sd.morphSkinnedShadowVs)) {
      setError(e, "内嵌 shader 缺失");
      return RD_ERROR_SHADER;
    }
    // water 系(可选:缺失只禁用水面,不阻塞引擎;enableWater 优雅降级)
    get("water_step", rd::ShaderStage::Fragment, sd.waterStepFs);
    get("water_caustics", rd::ShaderStage::Fragment, sd.waterCausticsFs);
    get("water_surface", rd::ShaderStage::Vertex, sd.waterSurfaceVs);
    get("water_surface", rd::ShaderStage::Fragment, sd.waterSurfaceFs);
    get("water_receiver", rd::ShaderStage::Vertex, sd.waterReceiverVs);
    get("water_receiver", rd::ShaderStage::Fragment, sd.waterReceiverFs);
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
  if (e) while (e->loadDone.tryPop()) {}  // 异步加载完成队列(回调在渲染线程)
  // 无表面/渲染器未就绪：安全跳过（节流日志，约每 300 次记一次避免刷屏）
  if (!e || !e->swapChain.valid() || !e->rendererReady) {
    static int skipLog = 0;
    if (skipLog++ % 300 == 0)
      RD_LOGW("api", "render_frame 跳过: engine=%p swapChain=%d rendererReady=%d", (void*)e,
              e ? e->swapChain.value() : 0, e ? int(e->rendererReady) : -1);
    return;
  }
  // 按需渲染:干净且无动画/惯性/水面时零 GPU 工作(平台 vsync 照常调,省电)
  if (!e->renderDirty && !e->animator.playing() && !e->orbit.isMoving() &&
      !e->waterScene.active)
    return;
  e->renderDirty = false;
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
  applyOptions(e);  // 选项映射(幂等)
  // 雨滴:确定性 LCG,~0.8s 一滴(避开池边 12%)
  if (e->waterScene.active && e->options.water.rain) {
    e->waterScene.rainTimer -= dt;
    if (e->waterScene.rainTimer <= 0.0f) {
      e->waterScene.rainTimer = 0.8f;
      uint32_t& r = e->waterScene.rainSeed;
      r = r * 1664525u + 1013904223u;
      const float u = float((r >> 16) & 0xFFFF) / 65535.0f;
      r = r * 1664525u + 1013904223u;
      const float v = float((r >> 16) & 0xFFFF) / 65535.0f;
      e->renderer.disturbWater(0.12f + u * 0.76f, 0.12f + v * 0.76f, 0.035f, 3.0f);
    }
  }
  e->orbit.update(dt);  // 惯性积分(无指针按下时生效)
  applyCamera(e, float(e->width), float(e->height));
  e->renderer.beginScene(e->camera, {0.05f, 0.05f, 0.06f, 1.0f});
  if (e->waterScene.active) {  // water_pool:受水体 + 水面(tick 驱动仿真)
    e->renderer.tick(dt);
    for (size_t i = 0; i < e->waterScene.receivers.size(); ++i)
      e->renderer.submitWaterReceiver(e->waterScene.receivers[i],
                                      e->waterScene.worlds[i]);
    e->renderer.submitWaterSurface(e->waterScene.surface, rd::math::Mat4(1.0f));
  } else if (e->hasAnimation && e->model) {  // 蒙皮/morph 路径:Animator 驱动
    e->animator.update(dt);
    // 权重:动画播放中用采样值;暂停/静止用手动覆盖(加载时=静态初始值)
    const float* mw = nullptr;
    uint32_t mc = 0;
    if (!e->morphOverride_.empty()) {
      if (e->animator.playing()) {
        mw = e->animator.morphWeights().data();
        mc = e->animator.morphTargetCount();
      } else {
        mw = e->morphOverride_.data();
        mc = uint32_t(e->morphOverride_.size());
      }
    }
    e->renderer.submit(e->model, rd::math::Mat4(1.0f),
                       e->animator.jointMatrices().data(),
                       uint32_t(e->animator.jointMatrices().size()), mw, mc);
  } else {
    e->scene->collect(e->renderer);
  }
  auto* cmd = e->device->acquireCommandBuffer();
  e->renderer.endScene(cmd, target);
  e->device->submit(cmd);
  e->device->present(e->swapChain);
  e->device->endFrame();
}

rd_result_t rd_engine_set_morph_weight(rd_engine* e, uint32_t target, float weight) {
  if (!e) return RD_ERROR_INVALID_ARG;
  if (target >= e->morphOverride_.size()) return RD_ERROR_INVALID_ARG;
  e->morphOverride_[target] = weight;
  e->animator.pause(true);  // 手动值生效(暂停动画轨道)
  e->renderDirty = true;
  return RD_OK;
}

void rd_engine_play_animation(rd_engine* e, int32_t clip) {
  if (!e || !e->hasAnimation || clip < 0) return;
  e->animator.play(uint32_t(clip));
}
void rd_engine_crossfade_animation(rd_engine* e, int32_t clip, float fade) {
  if (!e || !e->hasAnimation || clip < 0) return;
  e->animator.playWithFade(uint32_t(clip), fade);
}
void rd_engine_pause_animation(rd_engine* e, int32_t paused) {
  if (!e || !e->hasAnimation) return;
  e->animator.pause(paused != 0);
}

rd_pick_result_t rd_engine_pick(rd_engine* e, float x, float y) {
  rd_pick_result_t out = {};
  out.mesh_index = -1;
  if (!e || !e->model) return out;
  const float vw = e->width ? float(e->width) : 512.0f;
  const float vh = e->height ? float(e->height) : 512.0f;
  applyCamera(e, vw, vh);
  float o[3], d[3];
  rd::scene::screenRay(e->camera, x, y, vw, vh, o, d);
  auto r = rd::scene::pickModel(e->modelAsset, rd::math::Mat4(1.0f), o, d);
  if (!r.hit) return out;
  out.hit = 1;
  out.mesh_index = r.meshIndex;
  out.distance = r.distance;
  out.px = r.point[0];
  out.py = r.point[1];
  out.pz = r.point[2];
  if (r.meshIndex >= 0 && size_t(r.meshIndex) < e->modelAsset.meshes.size()) {
    std::strncpy(out.mesh_name, e->modelAsset.meshes[size_t(r.meshIndex)].name.c_str(),
                 sizeof(out.mesh_name) - 1);
  }
  return out;
}

rd_result_t rd_engine_set_quality(rd_engine* e, rd_quality_t q) {
  if (!e) return RD_ERROR_INVALID_ARG;
  if (q != RD_QUALITY_AUTO && q != RD_QUALITY_HIGH && q != RD_QUALITY_MID &&
      q != RD_QUALITY_LOW)
    return RD_ERROR_INVALID_ARG;
  e->quality = q;
  applyQuality(e);
  e->renderDirty = true;
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
  e->renderDirty = true;
  switch (a) {
    case RD_POINTER_DOWN: e->orbit.onPointerDown(int(id), x, y); break;
    case RD_POINTER_MOVE: e->orbit.onPointerMove(int(id), x, y); break;
    case RD_POINTER_UP:
    case RD_POINTER_CANCEL: e->orbit.onPointerUp(int(id), x, y); break;
  }
}
void rd_engine_on_scroll(rd_engine* e, float dy) {
  if (e) { e->renderDirty = true; e->orbit.onScroll(dy); }
}
void rd_engine_on_pinch(rd_engine* e, float r) {
  if (e) { e->renderDirty = true; e->orbit.onPinch(r); }
}
void rd_engine_on_double_tap(rd_engine* e, float, float) {
  if (e) { e->renderDirty = true; e->orbit.onDoubleTap(); }
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
  installModel(e, std::move(model));
  return std::string(e->lastError).find("GPU 上传失败") != std::string::npos
             ? RD_ERROR_ASSET
             : RD_OK;
}

/// 异步加载:工作线程解析+解码,完成队列由 render_frame(渲染线程)消费安装。
rd_result_t rd_engine_load_gltf_async(rd_engine* e, const char* path,
                                      rd_load_callback_t cb, void* userdata) {
  if (!e || !path) return RD_ERROR_INVALID_ARG;
  // 懒启动工作线程
  if (!e->loadThread) {
    e->loadQuit = false;
    e->loadThread = std::make_unique<std::thread>([e]() {
      struct Req {
        std::string path;
        rd_load_callback_t cb;
        void* userdata;
      };
      while (!e->loadQuit.load()) {
        // waitAndPop 阻塞;退出由 destroy 投递哨兵
        // 任务体 = 解析 + 回投完成
        // (waitAndPop 在当前线程执行任务——即本工作线程)
        e->loadWork.waitAndPop();
        if (e->loadQuit.load()) break;
      }
    });
  }
  const std::string p = path;
  rd::TextureLoadPref pref;
  pref.ktx2Target = rd::pickTranscodeTarget(
      e->device->caps().supports(rd::Capability::texture_compression_astc),
      e->device->caps().supports(rd::Capability::texture_compression_etc2));
  pref.maxDim = 4096;  // 解析期 renderer 未必就绪;上传时 renderer 不动纹理尺寸
  e->loadWork.post([e, p, pref, cb, userdata]() {
    if (e->loadQuit.load()) return;
    auto model = rd::loadGltf(p.c_str(), pref);  // CPU 解析/解码(线程安全)
    e->loadDone.post([e, m = std::move(model), cb, userdata]() mutable {
      rd_result_t r = RD_OK;
      if (!m.valid()) {
        setError(e, "异步加载失败");
        r = RD_ERROR_ASSET;
      } else {
        installModel(e, std::move(m));
        if (std::string(e->lastError).find("GPU 上传失败") != std::string::npos)
          r = RD_ERROR_ASSET;
      }
      if (cb) cb(r, userdata);
    });
  });
  return RD_OK;
}

rd_result_t rd_engine_load_scene(rd_engine* e, const char* name) {
  if (!e || !name) return RD_ERROR_INVALID_ARG;
  if (std::string(name) != "water_pool") {
    setError(e, (std::string("未知场景: ") + name).c_str());
    return RD_ERROR_ASSET;
  }
  if (!buildWaterPoolScene(e)) return RD_ERROR_SCENE;
  return RD_OK;
}

void rd_engine_water_disturb(rd_engine* e, float x, float y) {
  if (!e || !e->waterScene.active || !e->rendererReady) return;
  const float vw = e->width ? float(e->width) : 512.0f;
  const float vh = e->height ? float(e->height) : 512.0f;
  applyCamera(e, vw, vh);
  float o[3], d[3];
  rd::scene::screenRay(e->camera, x, y, vw, vh, o, d);
  const float planeY = 0.0f;  // water_pool 水面高度(WaterDesc 默认)
  if (std::fabs(d[1]) < 1e-5f) return;
  const float t = (planeY - o[1]) / d[1];
  if (t <= 0.0f) return;
  const float px = o[0] + d[0] * t, pz = o[2] + d[2] * t;
  const float u = px / 4.0f + 0.5f, v = pz / 4.0f + 0.5f;  // 池 4×4
  if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return;
  e->renderer.disturbWater(u, v, 0.06f, 5.0f);
  e->renderDirty = true;
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
  e->renderDirty = true;
  if (e->rendererReady) e->renderer.setShadowEnabled(e->shadowEnabled);
}

const char* rd_get_last_error(rd_engine* e) { return e ? e->lastError : ""; }

rd_result_t rd_engine_exec_command(rd_engine* e, const char* command) {
  if (!e || !command) return RD_ERROR_INVALID_ARG;
  std::string out;
  if (!e->bus.exec(command, out)) {
    setError(e, out.c_str());
    return RD_ERROR_INVALID_ARG;
  }
  e->renderDirty = true;  // 命令成功即可能改状态,置脏
  std::strncpy(e->cmdOutput, out.c_str(), sizeof(e->cmdOutput) - 1);
  e->cmdOutput[sizeof(e->cmdOutput) - 1] = '\0';
  return RD_OK;
}

void rd_engine_request_render(rd_engine* e) {
  if (e) e->renderDirty = true;
}

void rd_engine_set_cache_dir(rd_engine* e, const char* path) {
  if (!e) return;
  e->renderer.setCacheDir(path);
  // pipeline 缓存同目录:<dir>/pipelines/<backend>.bin
  if (e->device && path && path[0]) {
    const std::string dir = std::string(path) + "/pipelines";
    std::filesystem::create_directories(dir);
    const char* bn = e->device->backend() == rd::Backend::Vulkan ? "vulkan"
                   : e->device->backend() == rd::Backend::Metal ? "metal" : "gles";
    e->device->setPipelineCachePath((dir + "/" + bn + ".bin").c_str());
  } else if (e->device) {
    e->device->setPipelineCachePath("");
  }
  e->renderDirty = true;
}

rd_result_t rd_engine_set_environment_hdri(rd_engine* e, const char* path) {
  if (!e || !path) return RD_ERROR_INVALID_ARG;
  if (!e->rendererReady) {
    setError(e, "HDR 环境需 surface 就绪后设置");
    return RD_ERROR_SCENE;
  }
  rd::HdrEnv env;
  if (!rd::loadHdrEnv(path, env)) {
    setError(e, (std::string("HDR 加载失败: ") + path).c_str());
    return RD_ERROR_ASSET;
  }
  e->hdrEnv = std::move(env);  // 引擎持有(重建期指针有效)
  if (!e->renderer.setHdrEnvironment(&e->hdrEnv)) {
    e->hdrEnv = rd::HdrEnv();
    setError(e, "HDR 环境构建失败(GPU)");
    return RD_ERROR_SCENE;
  }
  e->renderDirty = true;
  return RD_OK;
}

void rd_engine_set_environment_procedural(rd_engine* e) {
  if (!e) return;
  if (e->rendererReady) e->renderer.setHdrEnvironment(nullptr);
  e->hdrEnv = rd::HdrEnv();
  e->renderDirty = true;
}

const char* rd_engine_command_output(rd_engine* e) { return e ? e->cmdOutput : ""; }

rd_result_t rd_engine_exec_script(rd_engine* e, const char* path) {
  if (!e || !path) return RD_ERROR_INVALID_ARG;
  FILE* f = fopen(path, "r");
  if (!f) {
    setError(e, (std::string("脚本不存在: ") + path).c_str());
    return RD_ERROR_INVALID_ARG;
  }
  char line[512];
  int lineNo = 0;
  rd_result_t result = RD_OK;
  while (fgets(line, sizeof(line), f)) {
    ++lineNo;
    std::string s(line);
    // 去首尾空白
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) continue;       // 空行
    if (s[a] == '#') continue;                  // 注释
    const auto b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
    std::string out;
    if (!e->bus.exec(s, out)) {
      snprintf(e->cmdOutput, sizeof(e->cmdOutput), "%d: %s", lineNo, out.c_str());
      setError(e, e->cmdOutput);
      result = RD_ERROR_INVALID_ARG;
      break;
    }
    e->renderDirty = true;
    std::strncpy(e->cmdOutput, out.c_str(), sizeof(e->cmdOutput) - 1);
  }
  fclose(f);
  return result;
}

int32_t rd_options_count() { return rd::optionsCount(); }
const char* rd_options_name(int32_t index) {
  if (index < 0 || index >= rd::optionsCount()) return nullptr;
  return rd::optionsAllNames()[index];
}

rd_result_t rd_engine_set_option(rd_engine* e, const char* name, const char* value) {
  if (!e || !name || !value) return RD_ERROR_INVALID_ARG;
  if (!rd::optionsSet(e->options, name, value)) {
    setError(e, (std::string("选项非法: ") + name).c_str());
    return RD_ERROR_INVALID_ARG;
  }
  e->renderDirty = true;
  return RD_OK;
}

rd_result_t rd_engine_get_option(rd_engine* e, const char* name, char* out,
                                 uint32_t size) {
  if (!e || !name || !out || size == 0) return RD_ERROR_INVALID_ARG;
  std::string s;
  if (!rd::optionsGet(e->options, name, s)) {
    setError(e, (std::string("选项不存在: ") + name).c_str());
    return RD_ERROR_INVALID_ARG;
  }
  if (s.size() + 1 > size) return RD_ERROR_INVALID_ARG;
  std::strncpy(out, s.c_str(), size - 1);
  out[size - 1] = '\0';
  return RD_OK;
}

rd_result_t rd_engine_save_options(rd_engine* e, const char* path) {
  if (!e || !path) return RD_ERROR_INVALID_ARG;
  std::string json = "{\n";
  const int32_t n = rd::optionsCount();
  for (int32_t i = 0; i < n; ++i) {
    const char* name = rd::optionsAllNames()[i];
    std::string v;
    rd::optionsGet(e->options, name, v);
    json += std::string("  \"") + name + "\": \"" + v + "\"";
    json += (i + 1 < n) ? ",\n" : "\n";
  }
  json += "}\n";
  const std::string tmp = std::string(path) + ".tmp";
  FILE* f = fopen(tmp.c_str(), "w");
  if (!f) {
    setError(e, "选项保存失败: 不可写");
    return RD_ERROR_INVALID_ARG;
  }
  const bool ok = fwrite(json.data(), 1, json.size(), f) == json.size();
  fclose(f);
  if (!ok) {
    std::filesystem::remove(tmp);
    setError(e, "选项保存失败: 写入截断");
    return RD_ERROR_INVALID_ARG;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    setError(e, "选项保存失败: rename");
    return RD_ERROR_INVALID_ARG;
  }
  return RD_OK;
}

rd_result_t rd_engine_load_options(rd_engine* e, const char* path) {
  if (!e || !path) return RD_ERROR_INVALID_ARG;
  FILE* f = fopen(path, "r");
  if (!f) {
    setError(e, (std::string("选项文件不存在: ") + path).c_str());
    return RD_ERROR_INVALID_ARG;
  }
  std::string json;
  char buf[1024];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) json.append(buf, n);
  fclose(f);
  // 扁平 JSON 扫描:"key" : "value"(或裸值);嵌套/数组不支持
  int applied = 0;
  size_t p = 0;
  bool syntaxOk = false;
  while ((p = json.find('"', p)) != std::string::npos) {
    const size_t ke = json.find('"', p + 1);
    if (ke == std::string::npos) break;
    const std::string key = json.substr(p + 1, ke - p - 1);
    p = ke + 1;
    const size_t colon = json.find(':', p);
    if (colon == std::string::npos) break;
    syntaxOk = true;
    p = colon + 1;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    std::string value;
    if (p < json.size() && json[p] == '"') {
      const size_t ve = json.find('"', p + 1);
      if (ve == std::string::npos) break;
      value = json.substr(p + 1, ve - p - 1);
      p = ve + 1;
    } else {
      const size_t ve = json.find_first_of(",} \t\r\n", p);
      value = json.substr(p, ve == std::string::npos ? ve : ve - p);
      p = ve == std::string::npos ? json.size() : ve;
    }
    // 未知名跳过(向前兼容)
    std::string dummy;
    if (rd::optionsGet(e->options, key, dummy)) {
      if (!rd::optionsSet(e->options, key, value)) {
        setError(e, (std::string("选项值非法: ") + key + "=" + value).c_str());
        return RD_ERROR_INVALID_ARG;
      }
      ++applied;
    }
  }
  if (!syntaxOk) {
    setError(e, "选项文件格式错误");
    return RD_ERROR_INVALID_ARG;
  }
  if (applied > 0) e->renderDirty = true;
  return RD_OK;
}
