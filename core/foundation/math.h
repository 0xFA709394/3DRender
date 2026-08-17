/**
 * @file math.h
 * @brief 数学库统一入口（glm 薄封装）。
 *
 * 统一约定：右手坐标系，NDC z ∈ [0,1]（GLM_FORCE_DEPTH_ZERO_TO_ONE 由构建系统定义，
 * 匹配 Vulkan/Metal；GLES 深度范围为 [-1,1]，仅影响深度精度不影响遮挡关系）。
 *
 * 全项目只允许通过本头使用 glm，保证上述宏约定在所有编译单元一致；
 * shader 侧的对应约定见 shaders/ 下各文件顶部注释。
 */
#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace rd::math {

/// @name 常用类型别名（与 glm 一一对应）
/// @{
using Mat3 = glm::mat3;
using Mat4 = glm::mat4;
using Vec2 = glm::vec2;
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using Quat = glm::quat;   ///< 四元数(动画旋转插值;构造序 w,x,y,z)
/// @}

/// 角度制转弧度制（glm 的三角/旋转接口均使用弧度）。
inline float radians(float degrees) { return glm::radians(degrees); }

/**
 * @brief 透视投影矩阵。
 * @param fovYRad 垂直视场角（弧度）。
 * @param aspect  宽高比 width/height。
 * @note 受 GLM_FORCE_DEPTH_ZERO_TO_ONE 影响，输出 NDC z ∈ [0,1]，匹配 Vulkan/Metal。
 */
inline Mat4 perspective(float fovYRad, float aspect, float zNear, float zFar) {
  return glm::perspective(fovYRad, aspect, zNear, zFar);
}

/// 视图矩阵：从 eye 看向 center，up 为上方向（右手系约定）。
inline Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
  return glm::lookAt(eye, center, up);
}

/// 在矩阵 m 基础上绕 axis 旋转 angleRad（弧度），返回累积后的矩阵。
inline Mat4 rotate(const Mat4& m, float angleRad, const Vec3& axis) {
  return glm::rotate(m, angleRad, axis);
}

} // namespace rd::math
