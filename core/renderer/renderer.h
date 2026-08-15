/**
 * @file renderer.h
 * @brief Renderer:帧流程编排——场景相机/清屏 → 收集渲染项 → pass 序列执行。
 *
 * 立即模式执行(GGUI 哲学):每帧 beginScene → submit×N → endScene,
 * 帧末渲染项清空。UBO 分层:slot0=FrameUBO(256B,viewProj/相机/光照/SH),
 * slot1=ItemUBO(per-item 256B 步进,mvp/world/normalMatrix/factors/uvTransform)。
 * 管线:PBR uber-shader 与 unlit 双管线,按材质 unlit 标记选择。
 */
#pragma once
#include "renderer/renderable.h"
#include "renderer/environment.h"
#include "foundation/math.h"
#include <memory>
#include <vector>

namespace rd::scene {
class Camera;
}

namespace rd {

class MeshRenderResource;

/// Renderer init 的 shader 描述(调用方从离线产物加载传入)。
struct RendererShaderDesc {
  std::vector<uint8_t> unlitVs, unlitFs;        ///< unlit 管线(KHR_materials_unlit)
  std::vector<uint8_t> pbrVs, pbrFs;            ///< pbr uber-shader
  std::vector<uint8_t> prefilterVs, prefilterFs;  ///< 环境预滤波
  std::string entry;                            // Metal="main0",其他="main"
  Format colorFormat = Format::RGBA8_UNORM;
};

class Renderer {
public:
  ~Renderer() { shutdown(); }
  /// 初始化:双管线 + 环境(SH/LUT/GPU 预滤波)+ 双层 UBO。失败返回 false。
  bool init(Device& dev, const RendererShaderDesc& desc);
  /// 释放 GPU 资源(设备销毁前调用)。
  void shutdown();

  /// 帧开始:相机 viewProj/位置入 FrameUBO,存清屏值。
  void beginScene(const scene::Camera& camera, const ClearColor& clear);
  /// 提交一个网格渲染项(资源 shared_ptr 持久持有,本帧引用)。
  void submit(const std::shared_ptr<MeshRenderResource>& mesh, const math::Mat4& world);
  /// pass 序列执行:prepass 钩子 → MainPass(depth);帧末队列清空。
  void endScene(CommandBuffer* cmd, TargetHandle target);

private:
  static constexpr uint32_t kUboStride = 256;   // 三后端对齐最小公倍
  static constexpr uint32_t kMaxItems = 64;     // 动态 UBO 容量(超出记警告截断)

  Device* dev_ = nullptr;
  ShaderModuleHandle vs_, fs_;                  // pbr shader(unlit 模块创建后即销毁)
  PipelineHandle unlitPipeline_;
  PipelineHandle pbrPipeline_;
  BufferHandle frameUbo_;       // hostWrite,256B
  BufferHandle itemUbo_;        // hostWrite,kUboStride*kMaxItems
  renderer::Environment env_;
  math::Mat4 viewProj_{1.0f};
  ClearColor clear_;
  std::vector<std::unique_ptr<Renderable>> queue_;
  std::vector<math::Mat4> worldStack_;  // 与 queue_ 平行的 world 矩阵(endScene 算 mvp)
};

} // namespace rd
