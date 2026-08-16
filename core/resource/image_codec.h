/**
 * @file image_codec.h
 * @brief 图像编解码(stb 封装):内存/文件 → RGBA8,RGBA8 → PNG。
 * 内核与测试/工具共用;解码失败返回 width==0 的 ImageData(优雅降级由调用方决定)。
 */
#pragma once
#include "rhi/rhi_types.h"
#include <cstdint>
#include <vector>

namespace rd {

/// 解码后的图像:默认 RGBA8 紧凑排列(行主序,顶向下);
/// KTX2 压缩图像时 format 为压缩格式、pixels 为逐 mip 紧凑 block 数据。
struct ImageData {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels;
  Format format = Format::RGBA8_UNORM;  ///< 像素格式(压缩格式时 mipLevels 通常 >1)
  uint32_t mipLevels = 1;               ///< mip 级数(仅 KTX2 路径 >1)
};

/// 内存解码(PNG/JPEG 等 stb 支持格式)→ RGBA8;失败返回 width==0。
/// maxDim>0 且图像超限时等比降采样到不超过 maxDim。
ImageData decodeImageRGBA8(const void* data, uint64_t size, uint32_t maxDim = 0);
/// 文件解码;失败返回 width==0。
ImageData loadImageRGBA8(const char* path);
/// 保存 RGBA8 为 PNG;成功返回 true。
bool saveImagePNG(const char* path, uint32_t w, uint32_t h, const uint8_t* rgba);

} // namespace rd
