// prefilter.frag：GGX 重要性采样预滤波（64 Hammersley 样本，N=V=R）。
// UBO binding0：face 基向量 + roughness；texEnv samplerCube binding4（texture slot 0）。
#version 450
layout(location = 0) in vec2 vNdc;
layout(binding = 0) uniform UBO {
  vec3 fwd;        // face 中心方向
  vec3 right;      // NDC +x 对应的世界方向
  vec3 upNdc;      // NDC +y 对应的世界方向
  float roughness;
};
layout(binding = 4) uniform samplerCube texEnv;
layout(location = 0) out vec4 outColor;

vec2 hammersley(uint i, uint n) {
  uint bits = i;
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  return vec2(float(i) / float(n), float(bits) * 2.3283064365386963e-10);
}

vec3 importanceSampleGGX(vec2 xi, float roughness, vec3 n) {
  float a = roughness * roughness;
  float phi = 2.0 * 3.14159265 * xi.x;
  float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
  float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
  vec3 h = vec3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
  vec3 up = abs(n.z) < 0.999 ? vec3(0, 0, 1) : vec3(1, 0, 0);
  vec3 tx = normalize(cross(up, n));
  vec3 ty = cross(n, tx);
  return normalize(tx * h.x + ty * h.y + n * h.z);
}

void main() {
  vec3 n = normalize(fwd + vNdc.x * right + vNdc.y * upNdc);
  vec3 v = n;
  vec3 acc = vec3(0.0);
  float totalWeight = 0.0;
  const uint kSamples = 64u;
  for (uint s = 0u; s < kSamples; ++s) {
    vec2 xi = hammersley(s, kSamples);
    vec3 h = importanceSampleGGX(xi, roughness, n);
    vec3 l = normalize(2.0 * dot(v, h) * h - v);
    float ndl = max(0.0, dot(n, l));
    if (ndl > 0.0) {
      acc += textureLod(texEnv, l, 0.0).rgb * ndl;
      totalWeight += ndl;
    }
  }
  outColor = vec4(totalWeight > 0.0 ? acc / totalWeight : vec3(0.0), 1.0);
}
