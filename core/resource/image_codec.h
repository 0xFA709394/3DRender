/**
 * @file image_codec.h
 * @brief 图像编解码(stb 封装):内存/文件 → RGBA8,RGBA8 → PNG。
 * 内核与测试/工具共用;解码失败返回 width==0 的 ImageData(优雅降级由调用方决定)。
 */
#pragma once
#include <cstdint>
#include <vector>

namespace rd {

/// 解码后的图像:RGBA8 紧凑排列(行主序,顶向下)。
struct ImageData {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels;
};

/// 内存解码(PNG/JPEG 等 stb 支持格式)→ RGBA8;失败返回 width==0。
ImageData decodeImageRGBA8(const void* data, uint64_t size);
/// 文件解码;失败返回 width==0。
ImageData loadImageRGBA8(const char* path);
/// 保存 RGBA8 为 PNG;成功返回 true。
bool saveImagePNG(const char* path, uint32_t w, uint32_t h, const uint8_t* rgba);

} // namespace rd
