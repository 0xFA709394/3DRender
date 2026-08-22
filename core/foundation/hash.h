/**
 * @file hash.h
 * @brief FNV-1a 64 位内容哈希(缓存键/内容寻址)。
 */
#pragma once
#include <cstddef>
#include <cstdint>

namespace rd {

inline uint64_t fnv1a64(const void* data, size_t size,
                        uint64_t h = 14695981039346656037ull) {
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}
/// 链式混入标量(尺寸/参数)。
inline uint64_t fnv1a64(uint64_t v, uint64_t h) { return fnv1a64(&v, sizeof(v), h); }

} // namespace rd
