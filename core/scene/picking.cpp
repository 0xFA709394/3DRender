// picking 的实现:screenRay(逆 viewProj 反投影)+ Möller–Trumbore 求交。
#include "scene/picking.h"
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace rd::scene {

void screenRay(const Camera& cam, float px, float py, float vpW, float vpH,
               float outOrigin[3], float outDir[3]) {
  const float ndcX = vpW > 0 ? (px / vpW) * 2.0f - 1.0f : 0.0f;
  const float ndcY = vpH > 0 ? 1.0f - (py / vpH) * 2.0f : 0.0f;  // 屏幕 y 翻转
  const math::Mat4 inv = glm::inverse(cam.projMatrix() * cam.viewMatrix());
  math::Vec4 nearP = inv * math::Vec4(ndcX, ndcY, 0.0f, 1.0f);  // NDC z=0(近平面)
  math::Vec4 farP = inv * math::Vec4(ndcX, ndcY, 1.0f, 1.0f);   // z=1(远平面)
  nearP /= nearP.w;
  farP /= farP.w;
  const math::Vec3 d = glm::normalize(math::Vec3(farP - nearP));
  outOrigin[0] = nearP.x;
  outOrigin[1] = nearP.y;
  outOrigin[2] = nearP.z;
  outDir[0] = d.x;
  outDir[1] = d.y;
  outDir[2] = d.z;
}

namespace {
/// Möller–Trumbore;命中返回 true 且 t>0(归一化射线距离)。
bool rayTri(const math::Vec3& o, const math::Vec3& d, const math::Vec3& a,
            const math::Vec3& b, const math::Vec3& c, float& t) {
  const math::Vec3 e1 = b - a, e2 = c - a;
  const math::Vec3 p = glm::cross(d, e2);
  const float det = glm::dot(e1, p);
  if (std::abs(det) < 1e-8f) return false;  // 平行/退化
  const float invDet = 1.0f / det;
  const math::Vec3 tv = o - a;
  const float u = glm::dot(tv, p) * invDet;
  if (u < 0.0f || u > 1.0f) return false;
  const math::Vec3 q = glm::cross(tv, e1);
  const float v = glm::dot(d, q) * invDet;
  if (v < 0.0f || u + v > 1.0f) return false;
  t = glm::dot(e2, q) * invDet;
  return t > 1e-6f;
}
} // namespace

PickResult pickModel(const ModelAsset& model, const math::Mat4& world,
                     const float origin[3], const float dir[3]) {
  PickResult best;
  // 射线入模型空间(world 逆;方向按逆矩阵旋转分量,归一化后距离等价)
  const math::Mat4 invWorld = glm::inverse(world);
  const math::Vec3 o =
      math::Vec3(invWorld * math::Vec4(origin[0], origin[1], origin[2], 1.0f));
  math::Vec3 d = math::Vec3(invWorld * math::Vec4(dir[0], dir[1], dir[2], 0.0f));
  const float dLen = glm::length(d);
  if (dLen < 1e-8f) return best;
  d /= dLen;
  float bestT = 1e30f;
  for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
    const MeshData& mesh = model.meshes[mi];
    const uint32_t strideFloats = mesh.skinned ? 20 : 12;
    // 包围球预筛(随算)
    math::Vec3 c(0);
    const uint32_t nv = uint32_t(mesh.vertices.size()) / strideFloats;
    if (nv == 0) continue;
    for (uint32_t v = 0; v < nv; ++v) {
      const float* p = &mesh.vertices[size_t(v) * strideFloats];
      c += math::Vec3(p[0], p[1], p[2]);
    }
    c /= float(nv);
    float r2 = 0;
    for (uint32_t v = 0; v < nv; ++v) {
      const float* p = &mesh.vertices[size_t(v) * strideFloats];
      const float dx = p[0] - c.x, dy = p[1] - c.y, dz = p[2] - c.z;
      r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
    }
    const float radius = std::sqrt(r2);
    // 射线-球:无交则跳过
    const math::Vec3 oc = c - o;
    const float tca = glm::dot(oc, d);
    const float d2 = glm::dot(oc, oc) - tca * tca;
    if (d2 > radius * radius) continue;
    // 逐三角形
    const uint32_t triCount = mesh.indexCount / 3;
    for (uint32_t t = 0; t < triCount; ++t) {
      uint32_t iv[3];
      for (int k = 0; k < 3; ++k) {
        const size_t byteOff =
            size_t(t * 3 + k) * (mesh.indexType == IndexType::UInt32 ? 4 : 2);
        iv[k] = mesh.indexType == IndexType::UInt32
                    ? *reinterpret_cast<const uint32_t*>(mesh.indices.data() + byteOff)
                    : *reinterpret_cast<const uint16_t*>(mesh.indices.data() + byteOff);
      }
      const float* pa = &mesh.vertices[size_t(iv[0]) * strideFloats];
      const float* pb = &mesh.vertices[size_t(iv[1]) * strideFloats];
      const float* pc = &mesh.vertices[size_t(iv[2]) * strideFloats];
      float hitT = 0;
      if (rayTri(o, d, math::Vec3(pa[0], pa[1], pa[2]), math::Vec3(pb[0], pb[1], pb[2]),
                 math::Vec3(pc[0], pc[1], pc[2]), hitT) &&
          hitT < bestT) {
        bestT = hitT;
        best.hit = true;
        best.meshIndex = int32_t(mi);
      }
    }
  }
  if (best.hit) {
    best.distance = bestT;
    // 命中点回世界空间
    const math::Vec3 lp = o + d * bestT;
    const math::Vec3 wp = math::Vec3(world * math::Vec4(lp, 1.0f));
    best.point[0] = wp.x;
    best.point[1] = wp.y;
    best.point[2] = wp.z;
  }
  return best;
}

} // namespace rd::scene
