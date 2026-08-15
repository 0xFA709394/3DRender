// 切线计算单测:平面三角形的切线方向与手性;uv 退化输入返回 false。
#include <gtest/gtest.h>
#include "resource/mesh_utils.h"
#include <cmath>
#include <cstring>

namespace {
// 单三角形,pos 在 z=0 平面,uv 与 xy 对齐 → tangent=(1,0,0,1)
// 布局 pos3|normal3|tangent4|uv2 交错(12 float/顶点)
void makeTri(float* verts) {
  const float v[36] = {
      0, 0, 0,  0, 0, 1,  0, 0, 0, 0,  0, 0,
      1, 0, 0,  0, 0, 1,  0, 0, 0, 0,  1, 0,
      0, 1, 0,  0, 0, 1,  0, 0, 0, 0,  0, 1,
  };
  memcpy(verts, v, sizeof(v));
}
} // namespace

TEST(MeshUtils, TangentOrthogonal) {
  float verts[36];
  makeTri(verts);
  const uint16_t idx[3] = {0, 1, 2};
  ASSERT_TRUE(rd::computeTangents(verts, 3, idx, 3, rd::IndexType::UInt16, 12));
  for (int v = 0; v < 3; ++v) {
    const float* t = verts + v * 12 + 6;  // tangent @ float 偏移 6
    EXPECT_NEAR(t[0], 1.0f, 1e-4f);
    EXPECT_NEAR(t[1], 0.0f, 1e-4f);
    EXPECT_NEAR(t[2], 0.0f, 1e-4f);
    EXPECT_NEAR(t[3], 1.0f, 1e-4f);   // 手性
  }
}

TEST(MeshUtils, DegenerateUvReturnsFalse) {
  float verts[36];
  makeTri(verts);
  for (int v = 0; v < 3; ++v) {  // uv 全部清零 → 退化
    verts[v * 12 + 10] = 0;
    verts[v * 12 + 11] = 0;
  }
  const uint16_t idx[3] = {0, 1, 2};
  EXPECT_FALSE(rd::computeTangents(verts, 3, idx, 3, rd::IndexType::UInt16, 12));
}
