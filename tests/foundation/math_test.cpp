// math.h 的单元测试：验证全局坐标系约定在 glm 封装上成立。
#include <gtest/gtest.h>
#include "foundation/math.h"

using rd::math::Mat4;
using rd::math::Vec3;
using rd::math::Vec4;

// 透视投影：GLM_FORCE_DEPTH_ZERO_TO_ONE 约定下，近平面应映射到 NDC z=0
// （匹配 Vulkan/Metal 深度范围；这是全项目数学约定，见 foundation/math.h）。
TEST(Math, PerspectiveMapsNearToZero) {
  // GLM_FORCE_DEPTH_ZERO_TO_ONE 约定：近平面 NDC z=0
  Mat4 p = rd::math::perspective(rd::math::radians(60.0f), 1.0f, 0.1f, 100.0f);
  Vec4 near = p * Vec4(0.0f, 0.0f, -0.1f, 1.0f);
  EXPECT_NEAR(near.z / near.w, 0.0f, 1e-5f);
}

// 视图矩阵：lookAt 从 (0,0,4) 看原点，原点在视图空间应位于 z=-4（相机前方为 -z，右手系）。
TEST(Math, LookAtMovesCameraBack) {
  Mat4 v = rd::math::lookAt(Vec3(0, 0, 4), Vec3(0, 0, 0), Vec3(0, 1, 0));
  Vec4 origin = v * Vec4(0, 0, 0, 1);
  EXPECT_NEAR(origin.z, -4.0f, 1e-5f); // 原点在相机前方 4 个单位（视图空间 -z）
}

// 旋转：绕 +Y 轴 90°（右手系），+X 方向应转到 -Z。
TEST(Math, RotateNinetyDegreesAboutY) {
  Mat4 r = rd::math::rotate(Mat4(1.0f), rd::math::radians(90.0f), Vec3(0, 1, 0));
  Vec4 x = r * Vec4(1, 0, 0, 1);
  EXPECT_NEAR(x.x, 0.0f, 1e-5f);
  EXPECT_NEAR(x.z, -1.0f, 1e-5f);
}
