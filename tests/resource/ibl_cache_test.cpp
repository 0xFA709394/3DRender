// IBL 缓存单测:hash 确定性/雪崩;文件写读往返;坏文件拒绝。
#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include "foundation/hash.h"
#include "resource/ibl_cache.h"

namespace fs = std::filesystem;

TEST(Hash, DeterministicAndAvalanche) {
  const uint8_t a[4] = {1, 2, 3, 4}, b[4] = {1, 2, 3, 5};
  EXPECT_EQ(rd::fnv1a64(a, 4), rd::fnv1a64(a, 4));   // 确定性
  EXPECT_NE(rd::fnv1a64(a, 4), rd::fnv1a64(b, 4));   // 雪崩
  EXPECT_NE(rd::fnv1a64(a, 4), rd::fnv1a64(a, 3));   // 长度敏感
}

TEST(IblCache, RoundTrip) {
  const fs::path dir = fs::temp_directory_path() / "rd_iblcache_test";
  fs::remove_all(dir);
  rd::IblCacheBlob blob;
  blob.size = 4;
  blob.mips = 3;
  for (uint32_t m = 0; m < blob.mips; ++m) {
    const uint32_t s = blob.size >> m;
    for (uint32_t f = 0; f < 6; ++f) {
      std::vector<uint8_t> px(size_t(s) * s * 4);
      for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t((i + f * 7 + m * 13) & 0xFF);
      blob.faces.push_back(std::move(px));
    }
  }
  ASSERT_TRUE(rd::iblCacheWrite(dir.string(), 0xDEADBEEF, blob));
  rd::IblCacheBlob got;
  ASSERT_TRUE(rd::iblCacheRead(dir.string(), 0xDEADBEEF, got));
  EXPECT_EQ(got.size, blob.size);
  EXPECT_EQ(got.mips, blob.mips);
  ASSERT_EQ(got.faces.size(), blob.faces.size());
  for (size_t i = 0; i < blob.faces.size(); ++i)
    EXPECT_EQ(got.faces[i], blob.faces[i]);
  // 未命中
  rd::IblCacheBlob miss;
  EXPECT_FALSE(rd::iblCacheRead(dir.string(), 0x1234, miss));
  fs::remove_all(dir);
}

TEST(IblCache, RejectCorrupt) {
  const fs::path dir = fs::temp_directory_path() / "rd_iblcache_test2";
  fs::remove_all(dir);
  fs::create_directories(dir / "ibl");
  const fs::path p = dir / "ibl" / "0000000000000001.ibc";
  FILE* f = fopen(p.string().c_str(), "wb");
  ASSERT_NE(f, nullptr);
  const char junk[8] = {'X','X','X','X','X','X','X','X'};
  fwrite(junk, 1, 8, f);
  fclose(f);
  rd::IblCacheBlob got;
  EXPECT_FALSE(rd::iblCacheRead(dir.string(), 1, got));
  fs::remove_all(dir);
}
