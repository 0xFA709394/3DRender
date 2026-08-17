// shadow_depth.vert:顶点 → 光源空间(ShadowUBO=lightViewProj,ItemUBO=world)。
#version 450
layout(location = 0) in vec3 aPos;
layout(binding = 0) uniform ShadowUBO { mat4 lightViewProj; } u;
layout(binding = 1) uniform ItemUBO {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;
  vec4 uvTransform;
} item;
void main() { gl_Position = u.lightViewProj * item.world * vec4(aPos, 1.0); }
