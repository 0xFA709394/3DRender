// HDR 环境加载单测:stbi_write_hdr 生成 → loadHdrEnv 解码(尺寸/浮点值域/错误路径)。
#include <gtest/gtest.h>
#include <algorithm>
#include "resource/hdr_env.h"
// stb_image_write 实现宏在 image_codec.cpp;测试侧只调 API(链接 rd_core 内 stb)
#include <stb_image_write.h>

TEST(HdrEnv, LoadWritten) {
  // 生成 8×4 渐变 hdr(值域 >1 验证 HDR)
  const int W = 8, H = 4;
  float px[W * H * 3];
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      px[(y * W + x) * 3 + 0] = float(x) / W * 4.0f;   // 0..4
      px[(y * W + x) * 3 + 1] = float(y) / H * 2.0f;   // 0..2
      px[(y * W + x) * 3 + 2] = 1.5f;
    }
  const char* path = "/tmp/rd_hdr_test.hdr";
  ASSERT_EQ(stbi_write_hdr(path, W, H, 3, px), 1);
  rd::HdrEnv env;
  ASSERT_TRUE(rd::loadHdrEnv(path, env));
  EXPECT_EQ(env.width, uint32_t(W));
  EXPECT_EQ(env.height, uint32_t(H));
  ASSERT_EQ(env.pixels.size(), size_t(W) * H * 4);  // RGBA float
  // 浮点值域:>1 保留(HDR 区别于 LDR 的本质)
  float maxV = 0;
  for (float v : env.pixels) maxV = std::max(maxV, v);
  EXPECT_GT(maxV, 1.0f);
}

TEST(HdrEnv, BadPath) {
  rd::HdrEnv env;
  EXPECT_FALSE(rd::loadHdrEnv("/tmp/rd_no_such.hdr", env));
}
