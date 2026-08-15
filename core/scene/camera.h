/**
 * @file camera.h
 * @brief 相机:位置/朝向 + 透视投影;view/proj 矩阵供 Renderer 场景 UBO。
 * 投影遵循全局 GLM_FORCE_DEPTH_ZERO_TO_ONE(NDC z∈[0,1],右手系)。
 */
#pragma once
#include "foundation/math.h"

namespace rd::scene {

class Camera {
public:
  void lookAt(const math::Vec3& eye, const math::Vec3& center, const math::Vec3& up) {
    eye_ = eye;
    center_ = center;
    up_ = up;
  }
  void setPerspective(float fovYRad, float aspect, float zNear, float zFar) {
    fovY_ = fovYRad;
    aspect_ = aspect;
    zNear_ = zNear;
    zFar_ = zFar;
  }
  math::Mat4 viewMatrix() const { return math::lookAt(eye_, center_, up_); }
  math::Mat4 projMatrix() const { return math::perspective(fovY_, aspect_, zNear_, zFar_); }

private:
  math::Vec3 eye_{0, 0, 3}, center_{0}, up_{0, 1, 0};
  float fovY_ = 0.78539816f, aspect_ = 1.0f, zNear_ = 0.1f, zFar_ = 100.0f;
};

} // namespace rd::scene
