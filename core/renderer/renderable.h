/**
 * @file renderable.h
 * @brief Renderable 基类:一帧内可录制命令的渲染项。
 * prepass 为 2b IBL 预滤波等 GPU 预处理的钩子(对齐 GGUI 两阶段录制,默认空)。
 */
#pragma once
#include "rhi/rhi_device.h"

namespace rd {
namespace renderer {
class Environment;
}

/// 渲染项录制上下文:per-frame 与 per-item UBO + 环境/灯光纹理(Renderer 注入)。
struct RenderContext {
  BufferHandle frameUbo;                    ///< slot0:FrameUBO(256B)
  BufferHandle itemUbo;                     ///< slot1:ItemUBO(per-item 256B 步进)
  uint64_t itemOffset = 0;                  ///< 本项在 itemUbo 中的偏移
  const renderer::Environment* env = nullptr;  ///< 环境纹理(prefilter/LUT)
  BufferHandle lightUbo;                    ///< slot2:LightUBO(352B,多光源+阴影)
  TextureHandle shadowMap;                  ///< slot7:阴影深度图(Renderer 保证恒有效)
  SamplerHandle shadowSampler;              ///< 比较采样器
  PipelineHandle shadowPipe;                ///< shadowPass 时使用的深度管线
  bool shadowPass = false;                  ///< true=只写深度(shadow_depth.vert)
  /// 场景 pass 管线(按 SceneTarget 格式/采样数匹配,endScene 时注入)
  PipelineHandle pbrPipeline;
  PipelineHandle unlitPipeline;
  /// 蒙皮管线(同匹配规则)
  PipelineHandle skinnedPipeline;
  PipelineHandle skinnedShadowPipe;
  /// 混合管线(alphaBlend 材质用;pbr 布局,blend 开 depthWrite 关)
  PipelineHandle blendPipeline;
  /// 共享 JointUBO(slot3;per-item 偏移)
  BufferHandle jointUbo;
  uint64_t jointOffset = 0;               ///< 本项在 JointUBO 中的偏移(蒙皮项)
};

class Renderable {
public:
  virtual ~Renderable() = default;
  /// render pass 前的预处理钩子(默认空)。
  virtual void prepass(CommandBuffer* cmd) { (void)cmd; }
  /// 录制本项绘制命令。
  virtual void record(CommandBuffer* cmd, const RenderContext& ctx) = 0;
};

} // namespace rd
