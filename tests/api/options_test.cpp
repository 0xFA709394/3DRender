// 声明式选项单测:默认值/get/set/reset/domain 钳制/未知名。
#include <gtest/gtest.h>
#include "options_generated.h"

TEST(Options, Defaults) {
  rd::Options o;
  EXPECT_EQ(o.quality.tier, "auto");
  EXPECT_TRUE(o.quality.shadow);
  EXPECT_TRUE(o.quality.post);
  EXPECT_FALSE(o.quality.fxaa);
  EXPECT_FLOAT_EQ(o.render.exposure, 1.0f);
  EXPECT_FLOAT_EQ(o.camera.fov_deg, 45.0f);
  EXPECT_EQ(o.ibl.prefilter_size, 64);
  EXPECT_EQ(o.shadow.map_size, 0);
}

TEST(Options, GetSetReset) {
  rd::Options o;
  std::string s;
  EXPECT_TRUE(rd::optionsSet(o, "render.exposure", "2.0"));
  EXPECT_FLOAT_EQ(o.render.exposure, 2.0f);
  EXPECT_TRUE(rd::optionsGet(o, "render.exposure", s));
  EXPECT_EQ(s, "2.000000");
  EXPECT_TRUE(rd::optionsReset(o, "render.exposure"));
  EXPECT_FLOAT_EQ(o.render.exposure, 1.0f);
  // bool 与 enum
  EXPECT_TRUE(rd::optionsSet(o, "quality.fxaa", "true"));
  EXPECT_TRUE(o.quality.fxaa);
  EXPECT_TRUE(rd::optionsSet(o, "quality.tier", "low"));
  EXPECT_EQ(o.quality.tier, "low");
  EXPECT_FALSE(rd::optionsSet(o, "quality.tier", "nope"));  // 域外拒绝
  EXPECT_FALSE(rd::optionsSet(o, "nope.x", "1"));           // 未知名拒绝
}

TEST(Options, DomainClamp) {
  rd::Options o;
  EXPECT_TRUE(rd::optionsSet(o, "camera.fov_deg", "999"));
  EXPECT_FLOAT_EQ(o.camera.fov_deg, 120.0f);  // 钳到 max
  EXPECT_TRUE(rd::optionsSet(o, "render.exposure", "0.01"));
  EXPECT_FLOAT_EQ(o.render.exposure, 0.1f);   // 钳到 min
}

TEST(Options, NameTable) {
  EXPECT_GT(rd::optionsCount(), 0);
  EXPECT_NE(rd::optionsAllNames(), nullptr);
  EXPECT_FALSE(rd::optionsDomainJson("quality.tier").empty());
}
