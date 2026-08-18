/**
 * @file picking.h
 * @brief 拾取:屏幕坐标→世界射线 → 射线与模型三角形精确求交(Möller–Trumbore)。
 * 纯 CPU;蒙皮模型按绑定姿态(已知限制)。
 */
#pragma once
#include "foundation/math.h"
#include "resource/gltf_loader.h"
#include "scene/camera.h"

namespace rd::scene {

struct PickResult {
  bool hit = false;
  int32_t meshIndex = -1;    ///< meshes[] 下标
  float distance = 0;        ///< 沿(归一化)射线距离
  float point[3] = {};       ///< 世界命中点
};

/// 屏幕像素坐标 → 世界射线(origin/归一化 dir;屏幕 y 向下)。
void screenRay(const Camera& cam, float px, float py, float vpW, float vpH,
               float outOrigin[3], float outDir[3]);

/// 射线与 ModelAsset 逐三角形求交(包围球预筛;world 逆变换入模型空间)。
/// 最近命中胜出;退化三角形跳过。
PickResult pickModel(const ModelAsset& model, const math::Mat4& world,
                     const float origin[3], const float dir[3]);

} // namespace rd::scene
