// equirect_to_cube.frag:方向(face basis,与 prefilter 同 UBO 布局)→ equirect uv 采样。
// 用途:HDR equirect 环境 → cubemap 转换(逐面全屏 pass);HDR 模式专用。
#version 450
layout(location = 0) in vec2 vNdc;
layout(binding = 0) uniform UBO {
  vec3 fwd;        // face 中心方向
  vec3 right;      // NDC +x 对应的世界方向
  vec3 upNdc;      // NDC +y 对应的世界方向
  float roughness; // 未用(与 prefilter UBO 布局一致)
};
layout(binding = 4) uniform sampler2D texEquirect;  // 纹理槽 0 ↔ binding 4
layout(location = 0) out vec4 outColor;
void main() {
  vec3 dir = normalize(fwd + vNdc.x * right + vNdc.y * upNdc);
  // 等距柱状:u = atan2(z,x)/(2π)+0.5,v = acos(y)/π(顶=v0)
  float u = atan(dir.z, dir.x) * 0.15915494 + 0.5;
  float v = acos(clamp(dir.y, -1.0, 1.0)) * 0.31830989;
  outColor = vec4(texture(texEquirect, vec2(u, v)).rgb, 1.0);
}
