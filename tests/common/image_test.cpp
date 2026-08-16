// image.h 的单元测试：PNG 读写回环、compareRGBA8 的容差语义。
#include <gtest/gtest.h>
#include "common/image.h"
#include "resource/image_codec.h"
#include <cstdio>
#include <filesystem>
#include <vector>

namespace {
// 构造纯色 RGBA8 测试图
std::vector<uint8_t> solidImage(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
  std::vector<uint8_t> img(w * h * 4);
  for (uint32_t i = 0; i < w * h; ++i) {
    img[i * 4 + 0] = r;
    img[i * 4 + 1] = g;
    img[i * 4 + 2] = b;
    img[i * 4 + 3] = 255;
  }
  return img;
}
} // namespace

// 保存→加载回环后尺寸与像素值一致（PNG 无损）
TEST(Image, RoundTripSaveLoad) {
  auto img = solidImage(16, 16, 200, 10, 30);
  std::string path = std::string(RD_TEST_DATA_DIR) + "/../build/image_roundtrip.png";
  ASSERT_TRUE(rd::test::savePNG(path, 16, 16, img.data()));
  auto loaded = rd::test::loadPNG(path);
  ASSERT_EQ(loaded.width, 16u);
  ASSERT_EQ(loaded.height, 16u);
  ASSERT_EQ(loaded.pixels.size(), img.size());
  EXPECT_EQ(loaded.pixels[0], 200);
  EXPECT_EQ(loaded.pixels[1], 10);
  EXPECT_EQ(loaded.pixels[2], 30);
}

// 通道差 ≤ channelTol：pass，且 maxChannelDiff 报告正确
TEST(Image, CompareWithinTolerancePasses) {
  auto a = solidImage(8, 8, 100, 100, 100);
  auto b = solidImage(8, 8, 102, 100, 100); // 通道差 2
  auto r = rd::test::compareRGBA8(a.data(), b.data(), 8, 8, /*channelTol=*/3, /*ratioTol=*/0.0);
  EXPECT_TRUE(r.pass);
  EXPECT_EQ(r.maxChannelDiff, 2);
}

// 通道差 > channelTol 且 ratioTol=0：fail
TEST(Image, CompareBeyondToleranceFails) {
  auto a = solidImage(8, 8, 100, 100, 100);
  auto b = solidImage(8, 8, 110, 100, 100);
  auto r = rd::test::compareRGBA8(a.data(), b.data(), 8, 8, 3, 0.0);
  EXPECT_FALSE(r.pass);
}

// 少量超差像素：ratioTol 决定是否放行（1/16=6.25% 超差，0.10 放行、0.0 拒绝）
TEST(Image, CompareRatioTolerance) {
  auto a = solidImage(4, 4, 0, 0, 0);
  auto b = a;
  b[0] = 255; // 1/16 像素超差
  EXPECT_FALSE(rd::test::compareRGBA8(a.data(), b.data(), 4, 4, 3, 0.0).pass);
  EXPECT_TRUE(rd::test::compareRGBA8(a.data(), b.data(), 4, 4, 3, 0.10).pass);
}

// maxDim 等比降采样:16x8 PNG 限 8 → 8x4(image_codec 层,内核共用)
TEST(Image, MaxDimDownscale) {
  std::vector<uint8_t> px(16 * 8 * 4);
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 16; ++x) {
      uint8_t* p = px.data() + (size_t(y) * 16 + x) * 4;
      p[0] = uint8_t(x * 16);
      p[1] = uint8_t(y * 32);
      p[3] = 255;
    }
  const std::string path =
      (std::filesystem::temp_directory_path() / "rd_maxdim.png").string();
  ASSERT_TRUE(rd::saveImagePNG(path.c_str(), 16, 8, px.data()));
  std::vector<uint8_t> bytes;
  {
    FILE* f = fopen(path.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bytes.resize(size_t(n));
    ASSERT_EQ(fread(bytes.data(), 1, bytes.size(), f), bytes.size());
    fclose(f);
  }
  auto img = rd::decodeImageRGBA8(bytes.data(), bytes.size(), 8);
  EXPECT_EQ(img.width, 8u);
  EXPECT_EQ(img.height, 4u);
  auto full = rd::decodeImageRGBA8(bytes.data(), bytes.size());
  EXPECT_EQ(full.width, 16u);
  EXPECT_EQ(full.height, 8u);
}
