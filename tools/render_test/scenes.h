// demo 场景构建与渲染(render_test/测试共用;直接驱动 Renderer,不经 engine)。
#pragma once
#include "renderer/renderer.h"
#include "resource/mesh_render_resource.h"
#include "scene/animator.h"
#include "scene/camera.h"
#include <memory>
#include <string>
#include <vector>

namespace rd::tool {

/// demo 场景上下文(资源持有)。
struct DemoScene {
  std::vector<std::shared_ptr<MeshRenderResource>> resources;
  std::vector<math::Mat4> worlds;              // 与 resources 平行(蒙皮项除外)
  scene::Camera camera;
  std::vector<LightData> lights;
  scene::Animator animator;
  bool animated = false;
  float animTime = 0.0f;                       // instanced_field 相位驱动
  bool waveField = false;                      // instanced_field 标记
  std::shared_ptr<MeshRenderResource> skinnedRes;
  float framingCenter[3] = {0, 0, 0};
  float framingRadius = 1.0f;
  /// 可选画质档(默认 nullptr=legacy 档;Bloom/阴影场景显式指定)。
  const QualityPreset* quality = nullptr;
};

/// 按名构建场景;资产缺失(sponza/cesium_man)返回 false 并记日志。
/// modelStorage 为调用方持有的 ModelAsset 存储(动画绑定源生命周期)。
bool buildDemoScene(const char* name, Device& dev, Renderer& renderer, DemoScene& out,
                    ModelAsset& modelStorage);

/// 场景名表(轮转/帮助用)。
const char* const* demoSceneNames(uint32_t& count);

/// 提交场景到渲染队列(动画项带关节调色板;每帧 worlds 波动更新)。
void submitDemoScene(DemoScene& s, Renderer& renderer, float dt);

/// 释放全部 GPU 资源。
void destroyDemoScene(DemoScene& s, Device& dev);

} // namespace rd::tool
