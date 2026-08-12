// 冒烟测试：验证测试基建与最基本的内核符号可链接、可运行。
#include <gtest/gtest.h>
#include "foundation/version.h"

// version() 返回非空字符串即视为内核可用（P0 的最小 sanity check）。
TEST(Smoke, VersionIsSet) {
  ASSERT_NE(rd::version(), nullptr);
}
