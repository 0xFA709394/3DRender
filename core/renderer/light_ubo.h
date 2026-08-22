/**
 * @file light_ubo.h
 * @brief LightUBO(slot2,352B)布局与填充:多光源数组 + 阴影参数 + 光源变换。
 */
#pragma once
#include "resource/gltf_loader.h"  // LightData/LightType
#include "foundation/math.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>

namespace rd {

/// LightUBO 布局(352B):lightViewProj|shadowParams|lightCount|lights[4×64B]
struct LightUBOData {
  math::Mat4 lightViewProj;
  float shadowParams[4];   // x=bias, y=1/shadowMapSize, z=shadowOn, w=vFlip(GLES=1)
  float lightCount[4];     // x=count
  struct LightSlot {
    float dirType[4];      // xyz=指向光源方向, w=type(0=dir,1=point,2=spot)
    float posRange[4];     // xyz=位置, w=range(0=无限)
    float color[4];        // rgb=color×intensity
    float spot[4];         // x=innerCos, y=outerCos
  } lights[4];
};
static_assert(sizeof(LightUBOData) == 352, "LightUBO 必须 352B");

/// 填充 LightUBO:lights 超 4 截断;首盏方向光为阴影投射者(见 shader)。
inline void fillLightUBO(LightUBOData& out, const std::vector<LightData>& lights,
                         const math::Mat4& lightViewProj, float shadowTexel,
                         bool shadowOn, bool vFlip, float bias = 0.0015f) {
  out.lightViewProj = lightViewProj;
  out.shadowParams[0] = bias;
  out.shadowParams[1] = shadowTexel;
  out.shadowParams[2] = shadowOn ? 1.0f : 0.0f;
  out.shadowParams[3] = vFlip ? 1.0f : 0.0f;
  const uint32_t n = std::min<uint32_t>(uint32_t(lights.size()), 4);
  out.lightCount[0] = float(n);
  out.lightCount[1] = out.lightCount[2] = out.lightCount[3] = 0.0f;
  for (uint32_t i = 0; i < 4; ++i) {
    auto& s = out.lights[i];
    memset(&s, 0, sizeof(s));
    if (i >= n) continue;
    const LightData& l = lights[i];
    s.dirType[0] = l.direction[0];
    s.dirType[1] = l.direction[1];
    s.dirType[2] = l.direction[2];
    s.dirType[3] = float(l.type == LightType::Directional ? 0
                         : l.type == LightType::Point   ? 1
                                                        : 2);
    s.posRange[0] = l.position[0];
    s.posRange[1] = l.position[1];
    s.posRange[2] = l.position[2];
    s.posRange[3] = l.range;
    s.color[0] = l.color[0];
    s.color[1] = l.color[1];
    s.color[2] = l.color[2];
    s.spot[0] = std::cos(l.innerCone);
    s.spot[1] = std::cos(l.outerCone);
  }
}

/// 包围球 + 方向光 → lightViewProj(正交,DEPTH_ZERO_TO_ONE)。
inline math::Mat4 makeLightViewProj(const LightData& dirLight, const float center[3],
                                    float radius) {
  math::Vec3 L(dirLight.direction[0], dirLight.direction[1], dirLight.direction[2]);
  if (glm::dot(L, L) < 1e-6f) L = math::Vec3(0, 1, 0);
  L = glm::normalize(L);
  const math::Vec3 c(center[0], center[1], center[2]);
  const math::Vec3 eye = c + L * (radius * 2.0f);
  const math::Mat4 view = math::lookAt(eye, c, math::Vec3(0, 1, 0));
  const float r = radius * 1.2f;
  const math::Mat4 proj = glm::ortho(-r, r, -r, r, radius * 0.1f, radius * 4.0f);
  return proj * view;
}

} // namespace rd
