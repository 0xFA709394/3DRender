/**
 * @file cube_scene.h
 * @brief 顶点色立方体演示场景（P0 的内置渲染内容）。
 */
#pragma once
#include "rhi/rhi_device.h"
#include <cstddef>
#include <cstdint>

namespace rd::demo {

/**
 * @brief 旋转顶点色立方体：8 顶点 / 36 索引，凸体 + 背面剔除（P0 无需深度缓冲）。
 *
 * shader 字节由调用方注入（测试从文件读，engine 用内嵌字节——见
 * api/embedded_shaders.h），入口名与颜色格式也由调用方按后端给出。
 *
 * 生命周期：init（建缓冲/着色器/管线）→ 每帧 render → shutdown（按依赖逆序释放）。
 */
class CubeScene {
public:
  /**
   * @brief 创建场景全部 GPU 资源。
   * @param vsCode/fsCode shader 字节（按后端为 SPIR-V/metallib/GLSL ES）。
   * @param entryPoint 入口名（Metal="main0"，其余="main"）。
   * @param colorFormat 渲染目标颜色格式（渲染到 swapchain 时传
   *        Device::swapChainColorFormat() 的返回值）。
   * @return 任一资源创建失败返回 false（细节见日志）；失败后仍应调用 shutdown
   *         清理已成功创建的部分资源。
   */
  bool init(Device& device, const uint8_t* vsCode, size_t vsSize, const uint8_t* fsCode,
            size_t fsSize, const char* entryPoint,
            Format colorFormat = Format::RGBA8_UNORM);
  /**
   * @brief 渲染一帧到指定目标。
   * @param target 离屏目标或 swapchain 帧目标。
   * @param angleRad 当前旋转角（弧度，绕 (1,1,0) 轴）。
   */
  void render(Device& device, TargetHandle target, uint32_t width, uint32_t height, float angleRad);
  /// 释放场景全部资源（对无效句柄安全，幂等）。
  void shutdown(Device& device);

private:
  BufferHandle vbo_, ibo_, ubo_;   ///< 顶点/索引/uniform（MVP 矩阵 64 字节）
  ShaderModuleHandle vs_, fs_;     ///< 顶点/片段着色器
  PipelineHandle pipeline_;        ///< 渲染管线
};

} // namespace rd::demo
