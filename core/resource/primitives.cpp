// primitives 的实现:球(UV 经纬)/平面/盒,48B 交错顶点 + 切线计算。
#include "resource/primitives.h"
#include "resource/mesh_utils.h"
#include <cmath>
#include <cstring>

namespace rd::primitives {

MeshData makeSphere(float radius, uint32_t segments, uint32_t rings) {
  MeshData m;
  m.name = "sphere";
  const uint32_t cols = segments + 1, rows = rings + 1;
  m.vertices.resize(size_t(cols) * rows * 12, 0.0f);
  for (uint32_t y = 0; y < rows; ++y) {
    const float v = float(y) / float(rings);
    const float phi = v * 3.14159265f;  // 0..π(顶到底)
    for (uint32_t x = 0; x < cols; ++x) {
      const float u = float(x) / float(segments);
      const float theta = u * 6.2831853f;
      const float sx = std::sin(phi) * std::cos(theta);
      const float sy = std::cos(phi);
      const float sz = std::sin(phi) * std::sin(theta);
      float* d = m.vertices.data() + size_t(y * cols + x) * 12;
      d[0] = sx * radius;
      d[1] = sy * radius;
      d[2] = sz * radius;
      d[3] = sx;
      d[4] = sy;
      d[5] = sz;  // 法线=单位方向
      d[10] = u;
      d[11] = v;
    }
  }
  // 索引(极点重顶点无妨,三角形全部合法)
  m.indices.resize(size_t(rings) * segments * 6 * 2);
  auto* idx = reinterpret_cast<uint16_t*>(m.indices.data());
  uint32_t w = 0;
  for (uint32_t y = 0; y < rings; ++y)
    for (uint32_t x = 0; x < segments; ++x) {
      const uint16_t a = uint16_t(y * cols + x);
      const uint16_t b = uint16_t(y * cols + x + 1);
      const uint16_t c = uint16_t((y + 1) * cols + x);
      const uint16_t dd = uint16_t((y + 1) * cols + x + 1);
      idx[w++] = a; idx[w++] = c; idx[w++] = b;
      idx[w++] = b; idx[w++] = c; idx[w++] = dd;
    }
  m.indexType = IndexType::UInt16;
  m.indexCount = w;
  computeTangents(m.vertices.data(), uint32_t(cols * rows), m.indices.data(), w,
                  IndexType::UInt16, 12);
  return m;
}

MeshData makePlane(float size, float repeat) {
  MeshData m;
  m.name = "plane";
  const float h = size * 0.5f;
  const float v[4][12] = {
      {-h, 0, -h, 0, 1, 0, 1, 0, 0, 1, 0, 0},
      { h, 0, -h, 0, 1, 0, 1, 0, 0, 1, repeat, 0},
      { h, 0,  h, 0, 1, 0, 1, 0, 0, 1, repeat, repeat},
      {-h, 0,  h, 0, 1, 0, 1, 0, 0, 1, 0, repeat},
  };
  m.vertices.assign(&v[0][0], &v[0][0] + 48);
  const uint16_t idx[6] = {0, 2, 1, 0, 3, 2};
  m.indices.resize(12);
  memcpy(m.indices.data(), idx, 12);
  m.indexType = IndexType::UInt16;
  m.indexCount = 6;
  return m;
}

MeshData makeBox(float sx, float sy, float sz) {
  MeshData m;
  m.name = "box";
  const float x = sx * 0.5f, y = sy * 0.5f, z = sz * 0.5f;
  const float n[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
  const float corners[8][3] = {{-x,-y,-z},{x,-y,-z},{x,y,-z},{-x,y,-z},
                               {-x,-y,z},{x,-y,z},{x,y,z},{-x,y,z}};
  const int faceV[6][4] = {{1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},
                           {5,4,7,6},{0,1,2,3}};
  m.vertices.resize(24 * 12, 0.0f);
  for (int f = 0; f < 6; ++f)
    for (int k = 0; k < 4; ++k) {
      float* d = m.vertices.data() + size_t(f * 4 + k) * 12;
      const float* p = corners[faceV[f][k]];
      d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
      d[3] = n[f][0]; d[4] = n[f][1]; d[5] = n[f][2];
      d[10] = (k == 1 || k == 2) ? 1.0f : 0.0f;
      d[11] = (k >= 2) ? 1.0f : 0.0f;
    }
  m.indices.resize(36 * 2);
  auto* idx = reinterpret_cast<uint16_t*>(m.indices.data());
  for (uint16_t f = 0; f < 6; ++f) {
    const uint16_t b = f * 4;
    idx[f * 6 + 0] = b; idx[f * 6 + 1] = uint16_t(b + 1); idx[f * 6 + 2] = uint16_t(b + 2);
    idx[f * 6 + 3] = b; idx[f * 6 + 4] = uint16_t(b + 2); idx[f * 6 + 5] = uint16_t(b + 3);
  }
  m.indexType = IndexType::UInt16;
  m.indexCount = 36;
  computeTangents(m.vertices.data(), 24, m.indices.data(), 36, IndexType::UInt16, 12);
  return m;
}

} // namespace rd::primitives
