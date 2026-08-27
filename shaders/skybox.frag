// skybox.frag:视方向 → prefilterCube mip0 采样(=环境原图);LDR 模式 Reinhard(与 pbr 一致)。
// 槽位:texPrefilter=slot5(binding 9);LightUBO=slot2(只用 lightCount.y 判 hdrMode)。
#version 450
layout(location = 0) in vec3 vDir;
layout(binding = 2) uniform LightUBO {
  mat4 lightViewProj;
  mat4 spotViewProj;
  vec4 shadowParams;
  vec4 spotShadowParams;
  vec4 lightCount;  // x=数量 y=hdrMode
  vec4 lights[16];
} lu;
layout(binding = 9) uniform samplerCube texPrefilter;
layout(location = 0) out vec4 outColor;
void main() {
  vec3 c = textureLod(texPrefilter, normalize(vDir), 0.0).rgb;
  if (lu.lightCount.y < 0.5) c = c / (c + 1.0);  // LDR Reinhard
  outColor = vec4(c, 1.0);
}
