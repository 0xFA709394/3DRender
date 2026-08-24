// shadow_depth_mask.frag:cutout 阴影;采样 baseColor alpha 低于阈值 discard。
// depth-only 无颜色输出;alphaCutoff 取 ItemUBO.metallicRoughness.w(0=非 MASK)。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 1) uniform ItemUBO {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;   // w=alphaCutoff
  vec4 uvTransform;
} item;
layout(binding = 4) uniform sampler2D texBaseColor;  // 纹理槽 0 ↔ binding 4
void main() {
  float a = texture(texBaseColor, vUV).a * item.baseColorFactor.a;
  if (item.metallicRoughness.w > 0.0 && a < item.metallicRoughness.w) discard;
}
