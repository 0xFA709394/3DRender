/**
 * @file orbit_controller.h
 * @brief Orbit 相机控制器:单指旋转 / 双指 pinch 缩放 / 双指平移 / 双击重置,
 * 带指数阻尼惯性。输入为像素坐标指针事件(平台层喂入);纯数学,不碰 GPU。
 *
 * 方向约定:yaw=0/pitch=0 时 eye 在 target 的 +Z 方向(与默认相机 (0,0,3) 一致);
 * 右拖 yaw 减(相机向右绕),下拖 pitch 增;pitch 钳制 ±1.55 rad。
 */
#pragma once
#include "foundation/math.h"
#include "scene/camera.h"

namespace rd::scene {

class OrbitController {
public:
  struct Params {
    float rotateSpeed = 0.005f;  ///< 弧度/像素
    float panFactor = 0.0015f;   ///< 平移系数(× distance,世界单位/像素)
    float dampingTau = 0.12f;    ///< 惯性衰减时间常数(秒)
    float minDistance = 0.01f;
    float maxDistance = 1e4f;
  };

  void setParams(const Params& p) { params_ = p; }

  /// 取景:target=center,distance=radius*2.5,视角 yaw=0.65/pitch=0.35
  /// (与 render_test --pbr 的 45°/20° 取景风格一致);记录为重置基准。
  void frameModel(const float center[3], float radius);
  /// 双击:回到重置基准位。
  void resetView();

  /// 指针事件(像素坐标;id 区分多指,最多跟踪 2 个)。
  void onPointerDown(int id, float x, float y);
  void onPointerMove(int id, float x, float y);
  void onPointerUp(int id, float x, float y);
  /// host 滚轮:deltaY>0 拉近。
  void onScroll(float deltaY);
  /// Android 探测器路径:双指比例缩放(ratio>1 放大→距离拉近)。
  void onPinch(float ratio);
  void onDoubleTap() { resetView(); }

  /// 每帧积分惯性(无指针按下时生效);dt 秒。
  void update(float dt);
  /// 写相机 eye/center(up 恒 (0,1,0))。
  void applyTo(Camera& cam) const;

  float yaw() const { return yaw_; }
  float pitch() const { return pitch_; }
  float distance() const { return distance_; }
  /// 是否在动(指针按下或惯性速度非零);按需渲染用。
  bool isMoving() const {
    return pointerCount() > 0 || vyaw_ != 0.0f || vpitch_ != 0.0f;
  }

private:
  struct Pointer {
    int id = -1;
    float x = 0, y = 0;
  };
  int pointerCount() const { return (p0_.id >= 0 ? 1 : 0) + (p1_.id >= 0 ? 1 : 0); }

  Params params_;
  math::Vec3 target_{0, 0, 0};
  float yaw_ = 0.0f, pitch_ = 0.0f, distance_ = 3.0f;
  // 重置基准
  math::Vec3 homeTarget_{0, 0, 0};
  float homeYaw_ = 0, homePitch_ = 0, homeDistance_ = 3.0f;
  // 惯性速度(EMA,弧度/秒级)
  float vyaw_ = 0, vpitch_ = 0;
  // 双指状态
  float lastPinchDist_ = 0;
  float lastCentroidX_ = 0, lastCentroidY_ = 0;
  Pointer p0_, p1_;
};

} // namespace rd::scene
