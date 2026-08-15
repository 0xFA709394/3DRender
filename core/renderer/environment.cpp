// environment 的实现(CPU 部分):程序化摄影棚环境、SH9 投影、BRDF LUT 积分。
// 方向约定:GL/Khronos cubemap 约定(与 GPU 采样一致;旧 CPU 自洽约定已废弃)。
#include "renderer/environment.h"
#include <algorithm>
#include <cmath>
#include <glm/glm.hpp>

namespace rd::renderer {
namespace {

// GPU(Khronos)cubemap 约定:u 右向、v 顶向下,均 ∈[-1,1]
glm::vec3 faceDir(uint32_t face, float u, float v) {
  switch (face) {
    case 0: return glm::normalize(glm::vec3(1, -v, -u));   // +X
    case 1: return glm::normalize(glm::vec3(-1, -v, u));   // -X
    case 2: return glm::normalize(glm::vec3(u, 1, v));     // +Y
    case 3: return glm::normalize(glm::vec3(u, -1, -v));   // -Y
    case 4: return glm::normalize(glm::vec3(u, -v, 1));    // +Z
    case 5: return glm::normalize(glm::vec3(-u, -v, -1));  // -Z
  }
  return glm::vec3(0, 0, 1);
}

glm::vec3 envColor(const glm::vec3& d) {
  // 摄影棚:地平渐变 + 两个柔光箱(亮度 ≤1.0)
  float t = glm::clamp(d.y * 0.5f + 0.5f, 0.0f, 1.0f);
  glm::vec3 base = glm::mix(glm::vec3(0.04f, 0.04f, 0.05f), glm::vec3(0.35f, 0.36f, 0.40f), t);
  float s1 = glm::smoothstep(0.90f, 0.98f,
                             glm::dot(d, glm::normalize(glm::vec3(-0.5f, 0.8f, 0.3f))));
  base += glm::vec3(0.65f, 0.60f, 0.52f) * s1;
  float s2 = glm::smoothstep(0.93f, 0.985f,
                             glm::dot(d, glm::normalize(glm::vec3(0.7f, 0.4f, -0.5f))));
  base += glm::vec3(0.40f, 0.48f, 0.60f) * s2;
  return glm::min(base, glm::vec3(1.0f));
}

uint8_t toByte(float v) { return uint8_t(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

glm::vec2 hammersley(uint32_t i, uint32_t n) {
  uint32_t bits = i;
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  return glm::vec2(float(i) / float(n), float(bits) * 2.3283064365386963e-10f);
}

glm::vec3 importanceSampleGGX(glm::vec2 xi, float roughness, const glm::vec3& n) {
  float a = roughness * roughness;
  float phi = 2.0f * 3.14159265f * xi.x;
  float cosTheta = sqrtf((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
  float sinTheta = sqrtf(1.0f - cosTheta * cosTheta);
  glm::vec3 h(sinTheta * cosf(phi), sinTheta * sinf(phi), cosTheta);
  glm::vec3 up = fabsf(n.z) < 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
  glm::vec3 tx = glm::normalize(glm::cross(up, n));
  glm::vec3 ty = glm::cross(n, tx);
  return glm::normalize(tx * h.x + ty * h.y + n * h.z);
}

} // namespace

EnvCubemap buildEnvCubemap(uint32_t size) {
  EnvCubemap env;
  env.size = size;
  env.faces.resize(6);
  for (uint32_t f = 0; f < 6; ++f) {
    auto& face = env.faces[f];
    face.resize(size_t(size) * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
      for (uint32_t x = 0; x < size; ++x) {
        float u = (float(x) + 0.5f) / float(size) * 2.0f - 1.0f;
        float v = (float(y) + 0.5f) / float(size) * 2.0f - 1.0f;
        glm::vec3 c = envColor(faceDir(f, u, v));
        uint8_t* p = &face[(y * size + x) * 4];
        p[0] = toByte(c.r);
        p[1] = toByte(c.g);
        p[2] = toByte(c.b);
        p[3] = 255;
      }
    }
  }
  return env;
}

std::array<float, 3> sampleEnv(const EnvCubemap& env, float dx, float dy, float dz) {
  // 主轴选面(GPU 约定的逆映射)
  glm::vec3 d = glm::normalize(glm::vec3(dx, dy, dz));
  glm::vec3 a = glm::abs(d);
  uint32_t face;
  float u, v;
  if (a.x >= a.y && a.x >= a.z) {
    if (d.x > 0) { face = 0; u = -d.z / a.x; v = -d.y / a.x; }
    else         { face = 1; u =  d.z / a.x; v = -d.y / a.x; }
  } else if (a.y >= a.z) {
    if (d.y > 0) { face = 2; u =  d.x / a.y; v =  d.z / a.y; }
    else         { face = 3; u =  d.x / a.y; v = -d.z / a.y; }
  } else {
    if (d.z > 0) { face = 4; u =  d.x / a.z; v = -d.y / a.z; }
    else         { face = 5; u = -d.x / a.z; v = -d.y / a.z; }
  }
  uint32_t x = std::min(env.size - 1, uint32_t((u * 0.5f + 0.5f) * env.size));
  uint32_t y = std::min(env.size - 1, uint32_t((v * 0.5f + 0.5f) * env.size));
  const uint8_t* p = &env.faces[face][(y * env.size + x) * 4];
  return {p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f};
}

std::vector<std::array<float, 3>> projectToSH(
    const std::vector<std::vector<uint8_t>>& faces, uint32_t size) {
  float coeff[9][3] = {};
  const float weightPerTexel = 4.0f * 3.14159265f / (6.0f * size * size);
  for (uint32_t f = 0; f < 6; ++f) {
    for (uint32_t y = 0; y < size; ++y) {
      for (uint32_t x = 0; x < size; ++x) {
        float u = (float(x) + 0.5f) / float(size) * 2.0f - 1.0f;
        float v = (float(y) + 0.5f) / float(size) * 2.0f - 1.0f;
        glm::vec3 d = faceDir(f, u, v);
        const uint8_t* p = &faces[f][(y * size + x) * 4];
        glm::vec3 L(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f);
        float yb[9] = {0.282095f,
                       0.488603f * d.y,
                       0.488603f * d.z,
                       0.488603f * d.x,
                       1.092548f * d.x * d.y,
                       1.092548f * d.y * d.z,
                       1.092548f * d.x * d.z,
                       0.546274f * (d.x * d.x - d.y * d.y),
                       0.315392f * (3.0f * d.z * d.z - 1.0f)};
        for (int i = 0; i < 9; ++i)
          for (int c = 0; c < 3; ++c) coeff[i][c] += L[c] * yb[i] * weightPerTexel;
      }
    }
  }
  // 折叠 Ã(辐照度卷积系数)
  const float A[9] = {3.141593f, 2.094395f, 2.094395f, 2.094395f,
                      0.785398f, 0.785398f, 0.785398f, 0.785398f, 0.785398f};
  std::vector<std::array<float, 3>> out(9);
  for (int i = 0; i < 9; ++i)
    for (int c = 0; c < 3; ++c) out[i][c] = coeff[i][c] * A[i];
  return out;
}

float evalSH(const std::vector<std::array<float, 3>>& sh, float nx, float ny, float nz) {
  float yb[9] = {0.282095f,
                 0.488603f * ny,
                 0.488603f * nz,
                 0.488603f * nx,
                 1.092548f * nx * ny,
                 1.092548f * ny * nz,
                 1.092548f * nx * nz,
                 0.546274f * (nx * nx - ny * ny),
                 0.315392f * (3.0f * nz * nz - 1.0f)};
  float e = 0;
  for (int i = 0; i < 9; ++i) e += sh[i][0] * yb[i];
  return e;
}

std::vector<float> integrateBrdfLut(uint32_t size) {
  std::vector<float> lut(size_t(size) * size * 2);
  const uint32_t kSamples = 64;
  for (uint32_t y = 0; y < size; ++y) {     // roughness
    for (uint32_t x = 0; x < size; ++x) {   // NdotV
      float roughness = (float(y) + 0.5f) / float(size);
      float ndotv = (float(x) + 0.5f) / float(size);
      glm::vec3 n(0, 0, 1);
      glm::vec3 v(sqrtf(1.0f - ndotv * ndotv), 0, ndotv);
      float a = 0, b = 0;
      for (uint32_t s = 0; s < kSamples; ++s) {
        glm::vec2 xi = hammersley(s, kSamples);
        glm::vec3 h = importanceSampleGGX(xi, roughness, n);
        glm::vec3 l = glm::normalize(2.0f * glm::dot(v, h) * h - v);
        float ndotl = glm::max(0.0f, l.z);
        if (ndotl <= 0.0f) continue;
        // Epic/Karis 形式:Vis = G·VdotH/(NdotH·NdotV),Smith IBL k = a/2
        float ndoth = glm::max(0.0f, h.z);
        float vdoth = glm::max(0.0f, glm::dot(v, h));
        float rough2 = roughness * roughness;  // a
        float k = rough2 / 2.0f;
        float gv = ndotv / (ndotv * (1.0f - k) + k);
        float gl = ndotl / (ndotl * (1.0f - k) + k);
        float vis = gv * gl * vdoth / (ndoth * ndotv + 1e-7f);
        float fc = powf(1.0f - vdoth, 5.0f);
        a += (1.0f - fc) * vis;
        b += fc * vis;
      }
      lut[(y * size + x) * 2] = a / float(kSamples);
      lut[(y * size + x) * 2 + 1] = b / float(kSamples);
    }
  }
  return lut;
}

} // namespace rd::renderer
