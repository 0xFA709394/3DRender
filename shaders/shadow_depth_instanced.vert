// shadow_depth_instanced.vert:阴影 pass 实例化;ItemUBO items[] 按 gl_InstanceIndex。
// 宿主 bind ItemUBO(offset=组首槽×256, size=组大小×256);同资源组共享光 VP。
#version 450
layout(location = 0) in vec3 aPos;
layout(binding = 0) uniform ShadowUBO { mat4 lightViewProj; } u;
struct Item {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;
  vec4 uvTransform;
};
layout(binding = 1) uniform ItemUBO { Item items[64]; } iu;
void main() {
  gl_Position = u.lightViewProj * iu.items[gl_InstanceIndex].world * vec4(aPos, 1.0);
}
