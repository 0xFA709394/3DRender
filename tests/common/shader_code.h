/**
 * @file shader_code.h
 * @brief 测试用 shader 产物加载工具（rd::test 命名空间）。
 *
 * 按后端从离线编译产物目录（RD_SHADER_DIR）读取 shader 字节：
 * Metal→.metallib，Vulkan→.spv，GLES→.gles；入口名 Metal="main0"，其他="main"。
 */
#pragma once
#include "rhi/rhi_types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace rd::test {

/// cube.vert/cube.frag 一对 shader 的字节 + 入口名。
struct ShaderCode {
  std::vector<uint8_t> vs;  ///< 顶点着色器字节
  std::vector<uint8_t> fs;  ///< 片段着色器字节
  std::string entry; // Metal="main0"，其他="main"
};

/// 单个 shader 文件的字节 + 入口名。
struct ShaderBlob {
  std::vector<uint8_t> code;
  std::string entry;
};

// 从 shader 产物目录加载 cube shader（Metal→metallib，Vulkan→spv，GLES→gles）
ShaderCode loadCubeShaderCode(Backend backend, const std::string& shaderDir);

// 按后端加载任意 shader 产物（Metal→metallib，Vulkan→spv，GLES→gles）
ShaderBlob loadShaderCode(Backend backend, const std::string& shaderDir, const std::string& name);

} // namespace rd::test
