/**
 * @file hdr_env.h
 * @brief .hdr(Radiance)equirect 环境图加载(stb_image float 解码)。
 */
#pragma once
#include <cstdint>
#include <vector>

namespace rd {

/// HDR equirect 环境:RGBA float 像素(A 通道无义,统一 1)。
struct HdrEnv {
  uint32_t width = 0, height = 0;
  std::vector<float> pixels;  ///< RGBA32F 紧凑
  bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

/// 加载 .hdr;失败(不存在/非 hdr)返回 false。
bool loadHdrEnv(const char* path, HdrEnv& out);

} // namespace rd
