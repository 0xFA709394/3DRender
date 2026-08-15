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

/// 渲染项录制上下文:per-frame 与 per-item UBO + 环境纹理(Renderer 注入)。
struct RenderContext {
  BufferHandle frameUbo;                    ///< slot0:FrameUBO(256B)
  BufferHandle itemUbo;                     ///< slot1:ItemUBO(per-item 256B 步进)
  uint64_t itemOffset = 0;                  ///< 本项在 itemUbo 中的偏移
  const renderer::Environment* env = nullptr;  ///< 环境纹理(prefilter/LUT)
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
