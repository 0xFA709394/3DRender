// pbr_forward_instanced.vert:同资源相邻项合并实例化;ItemUBO 按 gl_InstanceIndex 索引。
// 宿主侧:bind ItemUBO(offset=组首槽×256, size=组大小×256);逐实例 mvp/world/normalMatrix。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aUV;

struct Item {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;   // rgb=emissiveFactor, a=occlusionStrength
  vec4 metallicRoughness;   // x=metallic, y=roughness, z=normalScale, w=alphaCutoff
  vec4 uvTransform;         // xy=offset, zw=scale
};
layout(binding = 1) uniform ItemUBO { Item items[64]; } iu;  // kMaxItems

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUV;
layout(location = 4) flat out uint vItem;  // frag 索引同 UBO

void main() {
  const uint i = gl_InstanceIndex;  // 宿主以 bind offset 对齐组首槽
  const Item it = iu.items[i];
  vWorldPos = (it.world * vec4(aPos, 1.0)).xyz;
  vNormal = (it.normalMatrix * vec4(aNormal, 0.0)).xyz;
  vTangent = vec4((it.normalMatrix * vec4(aTangent.xyz, 0.0)).xyz, aTangent.w);
  vUV = aUV * it.uvTransform.zw + it.uvTransform.xy;
  vItem = i;
  gl_Position = it.mvp * vec4(aPos, 1.0);
}
