#include "common/golden_test.h"
#include <cstdlib>
#include <gtest/gtest.h>

namespace rd::test {

void runGoldenPair(Backend b, const char* goldenBase, GoldenRenderFn fn, double tol) {
  Image img = fn(b);
  if (img.width == 0) GTEST_SKIP() << "后端不可用或渲染失败: " << int(b);
  const std::string name =
      std::string(goldenBase) + (b == Backend::Metal ? "_metal.png" : "_vulkan.png");
  const std::string path = std::string(RD_TEST_DATA_DIR) + "/golden/" + name;
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(savePNG(path, img.width, img.height, img.pixels.data()));
    GTEST_SKIP() << "golden 已更新: " << path;
  }
  Image golden = loadPNG(path);
  ASSERT_EQ(golden.pixels.size(), img.pixels.size()) << "golden 缺失: " << path;
  auto cmp = compareSSIM(img.pixels.data(), golden.pixels.data(), img.width, img.height, tol);
  auto pix = compareRGBA8(img.pixels.data(), golden.pixels.data(), img.width, img.height, 3, 1.0);
  EXPECT_TRUE(cmp.pass) << "ssimError=" << cmp.error
                        << " pixelDiffRatio=" << pix.diffRatio;
}

} // namespace rd::test
