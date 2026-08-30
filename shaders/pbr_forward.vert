// pbr_forward.vert：PBR 前向顶点着色器。
// location 0=pos,1=normal,2=tangent,3=uv；ItemUBO(binding1)=mvp|world|normalMatrix|factors。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aUV;

layout(binding = 1) uniform ItemUBO {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;   // rgb=emissiveFactor, a=occlusionStrength
  vec4 metallicRoughness;   // x=metallic, y=roughness, z=normalScale
  vec4 uvTransform;         // xy=offset, zw=scale
  vec4 ext0;  // x=clearcoatFactor y=clearcoatRoughness z=clearcoatNormalScale w=specularFactor(vert 不读,GLES 跨阶段声明对齐)
  vec4 ext1;  // xyz=sheenColorFactor w=sheenRoughnessFactor(同上)
  vec4 ext2;  // xyz=specularColorFactor w=ior(同上)
};

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUV;

void main() {
  vWorldPos = (world * vec4(aPos, 1.0)).xyz;
  vNormal = (normalMatrix * vec4(aNormal, 0.0)).xyz;
  vTangent = vec4((normalMatrix * vec4(aTangent.xyz, 0.0)).xyz, aTangent.w);
  vUV = aUV * uvTransform.zw + uvTransform.xy;
  gl_Position = mvp * vec4(aPos, 1.0);
}
