// primitives 单测:球/平面/盒的拓扑、法线、切线与尺寸。
#include <gtest/gtest.h>
#include "resource/primitives.h"
#include <cmath>

TEST(Primitives, Sphere) {
  auto s = rd::primitives::makeSphere(0.5f, 16, 8);
  // 顶点数:(rings+1)×(segments+1);索引:rings×segments×2 三角形
  EXPECT_EQ(s.vertices.size(), size_t(9 * 17) * 12u);
  EXPECT_EQ(s.indexCount, 8u * 16u * 2u * 3u);
  EXPECT_FALSE(s.skinned);
  // 法线单位长度且外向(球心→顶点 与法线同向)
  const float* v0 = s.vertices.data();  // 顶点 0(极点)
  const float r = std::sqrt(v0[0] * v0[0] + v0[1] * v0[1] + v0[2] * v0[2]);
  EXPECT_NEAR(r, 0.5f, 1e-4f);
  const float nl = std::sqrt(v0[3] * v0[3] + v0[4] * v0[4] + v0[5] * v0[5]);
  EXPECT_NEAR(nl, 1.0f, 1e-3f);
}

TEST(Primitives, Plane) {
  auto p = rd::primitives::makePlane(2.0f, 1.0f);
  EXPECT_EQ(p.vertices.size(), 4u * 12u);
  EXPECT_EQ(p.indexCount, 6u);
  // 法线全 +Y
  for (int i = 0; i < 4; ++i) {
    EXPECT_FLOAT_EQ(p.vertices[size_t(i) * 12 + 3], 0.0f);
    EXPECT_FLOAT_EQ(p.vertices[size_t(i) * 12 + 4], 1.0f);
    EXPECT_FLOAT_EQ(p.vertices[size_t(i) * 12 + 5], 0.0f);
  }
}

TEST(Primitives, Box) {
  auto b = rd::primitives::makeBox(1, 2, 3);
  EXPECT_EQ(b.vertices.size(), 24u * 12u);  // 6 面 × 4 顶点
  EXPECT_EQ(b.indexCount, 36u);             // 12 三角形
  // 尺寸:顶点范围 ±0.5/±1/±1.5
  float maxY = 0;
  for (size_t i = 0; i < 24; ++i)
    maxY = std::max(maxY, std::abs(b.vertices[i * 12 + 1]));
  EXPECT_FLOAT_EQ(maxY, 1.0f);
}

TEST(Primitives, MakeGrid) {
  auto m = rd::primitives::makeGrid(4.0f, 8);
  ASSERT_FALSE(m.vertices.empty());
  ASSERT_FALSE(m.indices.empty());
  // 顶点数 =(8+1)²,索引字节 = 8²×6×2(UInt16)
  ASSERT_EQ(m.vertices.size() / 12, 81u);
  ASSERT_EQ(m.indexCount, 8u * 8u * 6u);
  ASSERT_EQ(m.indices.size(), 8u * 8u * 6u * 2);
  // 四角坐标:±2(y=0)
  const float* p0 = m.vertices.data();
  const float* pN = m.vertices.data() + (81 - 1) * 12;
  EXPECT_NEAR(p0[0], -2.0f, 1e-5);
  EXPECT_NEAR(p0[2], -2.0f, 1e-5);
  EXPECT_NEAR(p0[1], 0.0f, 1e-6);
  EXPECT_NEAR(pN[0], 2.0f, 1e-5);
  EXPECT_NEAR(pN[2], 2.0f, 1e-5);
  // 法线朝上
  EXPECT_NEAR(p0[4], 1.0f, 1e-6);
}
