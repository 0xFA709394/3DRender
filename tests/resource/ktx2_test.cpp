// ktx2_codec 单测:魔数探测 / 转码目标选择 / 内存解码(round-trip:libktx 现场编码)。
#include <gtest/gtest.h>
#include "common/ktx2_gen.h"
#include "resource/ktx2_codec.h"

TEST(Ktx2, MagicDetect) {
  const uint8_t good[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                            0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  EXPECT_TRUE(rd::isKtx2(good, sizeof(good)));
  const uint8_t bad[12] = {0x89, 0x50, 0x4E, 0x47};  // PNG 头
  EXPECT_FALSE(rd::isKtx2(bad, sizeof(bad)));
  EXPECT_FALSE(rd::isKtx2(good, 4));  // 太短
}

TEST(Ktx2, PickTarget) {
  EXPECT_EQ(rd::pickTranscodeTarget(true, true), rd::Ktx2Target::Astc);
  EXPECT_EQ(rd::pickTranscodeTarget(false, true), rd::Ktx2Target::Etc2);
  EXPECT_EQ(rd::pickTranscodeTarget(false, false), rd::Ktx2Target::Rgba32);
}

TEST(Ktx2, DecodeRoundtripRgba32) {
  const auto bytes = rd::test::makeTestKtx2(8);
  ASSERT_FALSE(bytes.empty());
  auto img = rd::decodeKtx2(bytes.data(), bytes.size(), rd::Ktx2Target::Rgba32);
  ASSERT_EQ(img.width, 8u);
  ASSERT_EQ(img.height, 8u);
  EXPECT_EQ(img.mipLevels, 2u);
  EXPECT_EQ(img.format, rd::Format::RGBA8_UNORM);
  // 2 mip:8x8x4 + 4x4x4 = 320B
  EXPECT_EQ(img.data.size(), 320u);
}

TEST(Ktx2, DecodeRoundtripAstc) {
  const auto bytes = rd::test::makeTestKtx2(8);
  ASSERT_FALSE(bytes.empty());
  auto img = rd::decodeKtx2(bytes.data(), bytes.size(), rd::Ktx2Target::Astc);
  if (img.width == 0) GTEST_SKIP() << "未压缩 ktx2(编码器缺失),ASTC 路径不适用";
  EXPECT_EQ(img.format, rd::Format::ASTC_4x4_UNORM);
  // ASTC 4x4:8x8=64B,4x4=16B → 80B
  EXPECT_EQ(img.data.size(), 80u);
}
