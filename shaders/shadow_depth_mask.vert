// shadow_depth_mask.vert:cutout 阴影(输出 vUV 供 frag 采样 alpha)。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec2 aUV;
layout(binding = 0) uniform ShadowUBO { mat4 lightViewProj; } u;
layout(binding = 1) uniform ItemUBO {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;   // w=alphaCutoff
  vec4 uvTransform;
} item;
layout(location = 0) out vec2 vUV;
void main() {
  vUV = aUV * item.uvTransform.zw + item.uvTransform.xy;
  gl_Position = u.lightViewProj * item.world * vec4(aPos, 1.0);
}
