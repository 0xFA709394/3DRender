// OrbitController 合成事件单测:旋转/pinch/平移/惯性/钳制/重置/相机产出。
#include <gtest/gtest.h>
#include "scene/orbit_controller.h"
#include <cmath>

namespace {
constexpr float kEps = 1e-4f;
}

TEST(Orbit, DragRotates) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){0, 0, 0}, 1.0f);  // dist = 2.5
  const float yaw0 = c.yaw(), pitch0 = c.pitch();
  c.onPointerDown(0, 100, 100);
  c.onPointerMove(0, 200, 150);  // dx=+100, dy=+50
  c.onPointerUp(0, 200, 150);
  // 约定:右拖 yaw 减(相机向右绕),下拖 pitch 增
  EXPECT_NEAR(c.yaw(), yaw0 - 100 * 0.005f, kEps);
  EXPECT_NEAR(c.pitch(), pitch0 + 50 * 0.005f, kEps);
}

TEST(Orbit, PinchZooms) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){0, 0, 0}, 1.0f);
  const float d0 = c.distance();
  c.onPointerDown(0, 100, 100);
  c.onPointerDown(1, 200, 100);
  c.onPointerMove(1, 300, 100);  // 指距 100→200,ratio 2
  EXPECT_NEAR(c.distance(), d0 / 2.0f, d0 * 0.01f);
  c.onPointerUp(0, 100, 100);
  c.onPointerUp(1, 300, 100);
}

TEST(Orbit, TwoFingerPan) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){0, 0, 0}, 1.0f);
  c.onPointerDown(0, 100, 100);
  c.onPointerDown(1, 200, 100);
  // 双指同向移动(质心 +50,+0)→ target 平移(yaw 不变)
  const float yaw0 = c.yaw();
  c.onPointerMove(0, 150, 100);
  c.onPointerMove(1, 250, 100);
  EXPECT_NEAR(c.yaw(), yaw0, kEps);
  rd::scene::Camera cam;
  c.applyTo(cam);
  // eye 已偏离初始位(发生了平移)
  EXPECT_TRUE(std::abs(cam.eye().x) > 0.001f || std::abs(cam.eye().y) > 0.001f);
  c.onPointerUp(0, 150, 100);
  c.onPointerUp(1, 250, 100);
}

TEST(Orbit, InertiaDecays) {
  rd::scene::OrbitController c;
  rd::scene::OrbitController::Params prm;
  prm.dampingTau = 0.12f;
  c.setParams(prm);
  c.frameModel((const float[]){0, 0, 0}, 1.0f);
  c.onPointerDown(0, 0, 0);
  for (int i = 1; i <= 5; ++i) c.onPointerMove(0, float(i * 20), 0);  // 快速拖动
  c.onPointerUp(0, 100, 0);
  const float y0 = c.yaw();
  c.update(0.016f);  // 惯性继续
  const float y1 = c.yaw();
  EXPECT_LT(y1, y0) << "惯性应继续旋转方向";
  for (int i = 0; i < 600; ++i) c.update(0.016f);  // 10 秒收敛
  const float y2 = c.yaw();
  c.update(0.016f);
  EXPECT_NEAR(c.yaw(), y2, 1e-5f) << "惯性应收敛停止";
}

TEST(Orbit, PitchClamped) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){0, 0, 0}, 1.0f);
  c.onPointerDown(0, 0, 0);
  c.onPointerMove(0, 0, 100000);  // 疯狂下拖
  EXPECT_LE(c.pitch(), 1.55f);
  c.onPointerMove(0, 0, -200000);
  EXPECT_GE(c.pitch(), -1.55f);
  c.onPointerUp(0, 0, 0);
}

TEST(Orbit, DoubleTapResets) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){0, 0, 0}, 1.0f);
  const float yaw0 = c.yaw(), d0 = c.distance();
  c.onPointerDown(0, 0, 0);
  c.onPointerMove(0, 300, 200);
  c.onPointerUp(0, 300, 200);
  c.onScroll(-5);
  EXPECT_NE(c.distance(), d0);
  c.onDoubleTap();
  EXPECT_NEAR(c.yaw(), yaw0, kEps);
  EXPECT_NEAR(c.distance(), d0, kEps);
}

TEST(Orbit, ApplyToCameraDistance) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){1, 2, 3}, 2.0f);  // dist = 5
  rd::scene::Camera cam;
  c.applyTo(cam);
  const auto& e = cam.eye();
  const float d = std::sqrt((e.x - 1) * (e.x - 1) + (e.y - 2) * (e.y - 2) +
                            (e.z - 3) * (e.z - 3));
  EXPECT_NEAR(d, 5.0f, 0.01f);
}

// 按需渲染:无输入静止;拖拽/惯性期间在动;惯性收敛后静止
TEST(Orbit, IsMoving) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){0, 0, 0}, 1.0f);
  EXPECT_FALSE(c.isMoving());   // 静止
  c.onPointerDown(0, 0, 0);
  EXPECT_TRUE(c.isMoving());    // 指针按下
  c.onPointerUp(0, 0, 0);
  c.update(0.016f);
  EXPECT_FALSE(c.isMoving());   // 无惯性(未拖)
  c.onPointerDown(0, 0, 0);
  for (int i = 1; i <= 5; ++i) c.onPointerMove(0, float(i * 20), 0);
  c.onPointerUp(0, 100, 0);
  EXPECT_TRUE(c.isMoving());    // 惯性
  for (int i = 0; i < 600; ++i) c.update(0.016f);
  EXPECT_FALSE(c.isMoving());   // 收敛停止
}
