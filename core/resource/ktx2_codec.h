/**
 * @file ktx2_codec.h
 * @brief KTX2/BasisU 解码(libktx 封装):内存 → 逐 mip 紧凑图像数据。
 * 转码目标由调用方按 device caps 推导(astc > etc2 > rgba32 兜底);
 * 本模块不碰 GPU,纯 CPU 解码。
 */
#pragma once
#include "rhi/rhi_types.h"
#include <cstdint>
#include <vector>

namespace rd {

/// KTX2 转码目标。
enum class Ktx2Target { Astc, Etc2, Rgba32 };

/// 按能力选转码目标:astc > etc2 > rgba32 兜底。
Ktx2Target pickTranscodeTarget(bool astc, bool etc2);

/// 解码后的 KTX2 图像:逐 mip 紧凑排列(与 TextureDesc::data 布局一致);
/// 压缩格式时 data 为 block 数据(formatMipBytes 对齐)。
struct Ktx2Image {
  Format format = Format::RGBA8_UNORM;
  uint32_t width = 0, height = 0;
  uint32_t mipLevels = 1;
  std::vector<uint8_t> data;
};

/// 内存解码 KTX2;BasisU supercompressed 则 transcode 到 target。
/// 失败(非 KTX2/损坏/不支持的 vkFormat)返回 width==0 并记日志。
Ktx2Image decodeKtx2(const void* data, uint64_t size, Ktx2Target target);

/// KTX2 魔数探测(«KTX 20»)。
bool isKtx2(const void* data, uint64_t size);

} // namespace rd
