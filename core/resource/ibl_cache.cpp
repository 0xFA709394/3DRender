// IBL 预滤波磁盘缓存 IO:原子写(临时文件+rename);坏文件拒绝。
#include "resource/ibl_cache.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace rd {
namespace fs = std::filesystem;

namespace {
constexpr uint32_t kMagic = 0x52444249;  // 'RDBI'
constexpr uint32_t kVersion = 1;

std::string pathFor(const std::string& dir, uint64_t key) {
  char name[32];
  snprintf(name, sizeof(name), "%016llx.ibc", (unsigned long long)key);
  return dir + "/ibl/" + name;
}
} // namespace

bool iblCacheWrite(const std::string& cacheDir, uint64_t key, const IblCacheBlob& blob) {
  if (cacheDir.empty() || blob.faces.empty()) return false;
  std::error_code ec;
  fs::create_directories(cacheDir + "/ibl", ec);
  if (ec) return false;
  const std::string tmp = pathFor(cacheDir, key) + ".tmp";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = true;
  const uint32_t hdr[4] = {kMagic, kVersion, blob.size, blob.mips};
  ok &= fwrite(hdr, 4, 4, f) == 4;
  for (const auto& px : blob.faces) {
    const uint32_t n = uint32_t(px.size());
    ok &= fwrite(&n, 4, 1, f) == 1;
    ok &= fwrite(px.data(), 1, n, f) == n;
  }
  fclose(f);
  if (!ok) {
    fs::remove(tmp, ec);
    return false;
  }
  fs::rename(tmp, pathFor(cacheDir, key), ec);
  return !ec;
}

bool iblCacheRead(const std::string& cacheDir, uint64_t key, IblCacheBlob& out) {
  if (cacheDir.empty()) return false;
  FILE* f = fopen(pathFor(cacheDir, key).c_str(), "rb");
  if (!f) return false;
  uint32_t hdr[4];
  if (fread(hdr, 4, 4, f) != 4 || hdr[0] != kMagic || hdr[1] != kVersion) {
    fclose(f);
    return false;
  }
  IblCacheBlob b;
  b.size = hdr[2];
  b.mips = hdr[3];
  if (b.size == 0 || b.mips == 0 || b.size > 4096 || b.mips > 12) {
    fclose(f);
    return false;  // 尺寸离谱 → 坏文件
  }
  const uint32_t nFace = b.mips * 6;
  for (uint32_t i = 0; i < nFace; ++i) {
    uint32_t n = 0;
    if (fread(&n, 4, 1, f) != 1) { fclose(f); return false; }
    const uint32_t m = i / 6;
    const uint32_t s = std::max(1u, b.size >> m);
    if (n != s * s * 4) { fclose(f); return false; }  // 尺寸不符 → 坏文件
    std::vector<uint8_t> px(n);
    if (fread(px.data(), 1, n, f) != n) { fclose(f); return false; }
    b.faces.push_back(std::move(px));
  }
  fclose(f);
  out = std::move(b);
  return true;
}

} // namespace rd
