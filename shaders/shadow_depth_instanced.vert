// shadow_depth_instanced.vert:阴影 pass 实例化;ItemUBO items[] 按 gl_InstanceIndex。
// 宿主 bind ItemUBO(offset=组首槽×512, size=组大小×512);同资源组共享光 VP。
// Item 布局与 pbr_forward_instanced 一致(ext0..2 + padding 到 512B 槽距)。
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
  vec4 ext0;
  vec4 ext1;
  vec4 ext2;
  vec4 _pad[13];
};
layout(binding = 1) uniform ItemUBO { Item items[32]; } iu;
void main() {
  gl_Position = u.lightViewProj * iu.items[gl_InstanceIndex].world * vec4(aPos, 1.0);
}
