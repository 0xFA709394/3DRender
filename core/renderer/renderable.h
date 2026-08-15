/**
 * @file renderable.h
 * @brief Renderable 基类:一帧内可录制命令的渲染项。
 * prepass 为 2b IBL 预滤波等 GPU 预处理的钩子(对齐 GGUI 两阶段录制,默认空)。
 */
#pragma once
#include "rhi/rhi_device.h"

namespace rd {

class Renderable {
public:
  virtual ~Renderable() = default;
  /// render pass 前的预处理钩子(默认空)。
  virtual void prepass(CommandBuffer* cmd) { (void)cmd; }
  /// 录制本项绘制命令;sceneUBO 为 per-item mvp 动态缓冲(offset 子区间绑定)。
  virtual void record(CommandBuffer* cmd, BufferHandle sceneUBO, uint64_t uboOffset) = 0;
};

} // namespace rd
