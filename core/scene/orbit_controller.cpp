// OrbitController 的实现:指针事件状态机 + 惯性积分。
#include "scene/orbit_controller.h"
#include <algorithm>
#include <cmath>

namespace rd::scene {
namespace {
constexpr float kPitchLimit = 1.55f;
float clampPitch(float p) { return std::max(-kPitchLimit, std::min(kPitchLimit, p)); }
} // namespace

void OrbitController::frameModel(const float center[3], float radius) {
  target_ = homeTarget_ = math::Vec3(center[0], center[1], center[2]);
  distance_ = homeDistance_ = std::max(radius * 2.5f, params_.minDistance);
  yaw_ = homeYaw_ = 0.65f;
  pitch_ = homePitch_ = 0.35f;
  vyaw_ = vpitch_ = 0;
  p0_ = p1_ = Pointer{};
}

void OrbitController::resetView() {
  target_ = homeTarget_;
  yaw_ = homeYaw_;
  pitch_ = homePitch_;
  distance_ = homeDistance_;
  vyaw_ = vpitch_ = 0;
}

void OrbitController::onPointerDown(int id, float x, float y) {
  if (p0_.id < 0) {
    p0_ = {id, x, y};
  } else if (p1_.id < 0) {
    p1_ = {id, x, y};
    // 进入双指:初始化 pinch/质心基准,清惯性
    lastPinchDist_ = std::hypot(p1_.x - p0_.x, p1_.y - p0_.y);
    lastCentroidX_ = (p0_.x + p1_.x) * 0.5f;
    lastCentroidY_ = (p0_.y + p1_.y) * 0.5f;
  }
  vyaw_ = vpitch_ = 0;
}

void OrbitController::onPointerMove(int id, float x, float y) {
  Pointer* p = p0_.id == id ? &p0_ : p1_.id == id ? &p1_ : nullptr;
  if (!p) return;
  if (pointerCount() == 1) {
    // 单指旋转(先取旧值算 delta 再更新)
    const float dx = x - p->x, dy = y - p->y;
    p->x = x;
    p->y = y;
    const float dyaw = -dx * params_.rotateSpeed;
    const float dpitch = dy * params_.rotateSpeed;
    yaw_ += dyaw;
    pitch_ = clampPitch(pitch_ + dpitch);
    // 惯性 EMA(按 60Hz 事件节奏折算为速度量纲)
    vyaw_ = vyaw_ * 0.8f + dyaw * 60.0f * 0.2f;
    vpitch_ = vpitch_ * 0.8f + dpitch * 60.0f * 0.2f;
    return;
  }
  // 双指:先更新位置
  p->x = x;
  p->y = y;
  const float pinchDist = std::hypot(p1_.x - p0_.x, p1_.y - p0_.y);
  const float cx = (p0_.x + p1_.x) * 0.5f, cy = (p0_.y + p1_.y) * 0.5f;
  // 缩放主导:指距在变(|ratio-1|>1%)→ 只缩放不平移(避免双指张开时模型漂移)
  const float ratio = lastPinchDist_ > 1e-3f ? pinchDist / lastPinchDist_ : 1.0f;
  const bool pinching = std::fabs(ratio - 1.0f) > 0.01f;
  if (lastPinchDist_ > 1e-3f && pinchDist > 1e-3f) onPinch(ratio);
  // 质心平移:沿相机 right/up 移动 target(仅在非缩放帧)
  const float ddx = cx - lastCentroidX_, ddy = cy - lastCentroidY_;
  if (!pinching && (ddx != 0 || ddy != 0)) {
    const float s = distance_ * params_.panFactor;
    const float cyaw = std::cos(yaw_), syaw = std::sin(yaw_);
    // 相机 right = (cyaw, 0, -syaw);up 近似取世界 +Y 分量方向
    target_.x -= ddx * s * cyaw;
    target_.z += ddx * s * syaw;
    target_.y += ddy * s;
  }
  lastPinchDist_ = pinchDist;
  lastCentroidX_ = cx;
  lastCentroidY_ = cy;
  vyaw_ = vpitch_ = 0;
}

void OrbitController::onPointerUp(int id, float, float) {
  if (p0_.id == id) p0_.id = -1;
  if (p1_.id == id) p1_.id = -1;
  lastPinchDist_ = 0;
}

void OrbitController::onScroll(float deltaY) {
  distance_ = std::max(params_.minDistance,
                       std::min(params_.maxDistance, distance_ * std::exp(-deltaY * 0.002f)));
}

void OrbitController::onPinch(float ratio) {
  if (ratio <= 1e-3f) return;
  distance_ = std::max(params_.minDistance,
                       std::min(params_.maxDistance, distance_ / ratio));
}

void OrbitController::update(float dt) {
  if (pointerCount() > 0) return;  // 拖拽中不积分惯性
  if (vyaw_ == 0 && vpitch_ == 0) return;
  yaw_ += vyaw_ * dt;
  pitch_ = clampPitch(pitch_ + vpitch_ * dt);
  const float decay = std::exp(-dt / params_.dampingTau);
  vyaw_ *= decay;
  vpitch_ *= decay;
  if (std::abs(vyaw_) < 1e-3f) vyaw_ = 0;
  if (std::abs(vpitch_) < 1e-3f) vpitch_ = 0;
}

void OrbitController::applyTo(Camera& cam) const {
  const float cp = std::cos(pitch_), sp = std::sin(pitch_);
  const float cy = std::cos(yaw_), sy = std::sin(yaw_);
  // yaw=0/pitch=0 → +Z;yaw>0 向 -X 绕(右手)
  const math::Vec3 dir(cp * sy, sp, cp * cy);
  cam.lookAt(target_ + dir * distance_, target_, math::Vec3(0, 1, 0));
}

} // namespace rd::scene
