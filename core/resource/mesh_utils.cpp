// mesh_utils 的实现:Lengyel 切线计算(逐三角形累积 → Gram-Schmidt 正交化 → 手性)。
#include "resource/mesh_utils.h"
#include <cmath>
#include <vector>

namespace rd {

bool computeTangents(float* vertices, uint32_t vertexCount, const void* indices,
                     uint32_t indexCount, IndexType indexType, uint32_t floatStride) {
  std::vector<float> t1(size_t(vertexCount) * 3, 0.0f);
  std::vector<float> t2(size_t(vertexCount) * 3, 0.0f);
  auto idxAt = [&](uint32_t i) -> uint32_t {
    return indexType == IndexType::UInt16
               ? static_cast<const uint16_t*>(indices)[i]
               : static_cast<const uint32_t*>(indices)[i];
  };
  for (uint32_t t = 0; t + 2 < indexCount; t += 3) {
    uint32_t i0 = idxAt(t), i1 = idxAt(t + 1), i2 = idxAt(t + 2);
    const float* v0 = vertices + size_t(i0) * floatStride;
    const float* v1 = vertices + size_t(i1) * floatStride;
    const float* v2 = vertices + size_t(i2) * floatStride;
    float x1 = v1[0] - v0[0], y1 = v1[1] - v0[1], z1 = v1[2] - v0[2];
    float x2 = v2[0] - v0[0], y2 = v2[1] - v0[1], z2 = v2[2] - v0[2];
    const float* uv0 = v0 + 10;  // uv @ float 偏移 10
    const float* uv1 = v1 + 10;
    const float* uv2 = v2 + 10;
    float s1 = uv1[0] - uv0[0], tc1 = uv1[1] - uv0[1];
    float s2 = uv2[0] - uv0[0], tc2 = uv2[1] - uv0[1];
    float det = s1 * tc2 - s2 * tc1;
    if (std::abs(det) < 1e-12f) continue;  // uv 退化三角形跳过
    float r = 1.0f / det;
    float tan[3] = {r * (tc2 * x1 - tc1 * x2), r * (tc2 * y1 - tc1 * y2),
                    r * (tc2 * z1 - tc1 * z2)};
    float bit[3] = {r * (-s2 * x1 + s1 * x2), r * (-s2 * y1 + s1 * y2),
                    r * (-s2 * z1 + s1 * z2)};
    for (uint32_t i : {i0, i1, i2}) {
      for (int c = 0; c < 3; ++c) {
        t1[size_t(i) * 3 + c] += tan[c];
        t2[size_t(i) * 3 + c] += bit[c];
      }
    }
  }
  // 全部退化 → 失败(调用方降级,如法线贴图近似)
  bool anyValid = false;
  for (uint32_t i = 0; i < vertexCount; ++i) {
    if (t1[size_t(i) * 3] != 0 || t1[size_t(i) * 3 + 1] != 0 || t1[size_t(i) * 3 + 2] != 0)
      anyValid = true;
  }
  if (!anyValid) return false;

  for (uint32_t i = 0; i < vertexCount; ++i) {
    float* v = vertices + size_t(i) * floatStride;
    const float* n = v + 3;
    const float* t = t1.data() + size_t(i) * 3;
    const float* bt = t2.data() + size_t(i) * 3;
    // Gram-Schmidt 正交化
    float ndt = n[0] * t[0] + n[1] * t[1] + n[2] * t[2];
    float tan[3] = {t[0] - n[0] * ndt, t[1] - n[1] * ndt, t[2] - n[2] * ndt};
    float len = std::sqrt(tan[0] * tan[0] + tan[1] * tan[1] + tan[2] * tan[2]);
    if (len < 1e-8f) {
      tan[0] = 1;
      tan[1] = 0;
      tan[2] = 0;
      len = 1.0f;
    }
    // 手性:sign((n × t) · bitangent累积)
    float cx = n[1] * t[2] - n[2] * t[1];
    float cy = n[2] * t[0] - n[0] * t[2];
    float cz = n[0] * t[1] - n[1] * t[0];
    float w = (cx * bt[0] + cy * bt[1] + cz * bt[2]) < 0 ? -1.0f : 1.0f;
    v[6] = tan[0] / len;
    v[7] = tan[1] / len;
    v[8] = tan[2] / len;
    v[9] = w;
  }
  return true;
}

} // namespace rd
