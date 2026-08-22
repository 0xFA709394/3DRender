/**
 * @file ibl_cache.h
 * @brief IBL 预滤波磁盘缓存:内容哈希键 → 逐 face/mip RGBA8 像素 blob。
 * 原子写(临时文件+rename);坏文件(magic/version/尺寸不符)拒绝。
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rd {

/// 预滤波 cube 像素 blob:faces 按 mip-major(faces[mip*6+face]),RGBA8 紧凑。
struct IblCacheBlob {
  uint32_t size = 0;   ///< cube 边长(mip0)
  uint32_t mips = 0;   ///< mip 级数
  std::vector<std::vector<uint8_t>> faces;  ///< faces[mip*6+face]
};

/// 写缓存(目录不存在自动创建 ibl/ 子目录;原子 rename)。成功 true。
bool iblCacheWrite(const std::string& cacheDir, uint64_t key, const IblCacheBlob& blob);
/// 读缓存;未命中/坏文件 false。
bool iblCacheRead(const std::string& cacheDir, uint64_t key, IblCacheBlob& out);

} // namespace rd
