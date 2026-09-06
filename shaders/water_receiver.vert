// water_receiver.vert:受水体顶点(池底/池壁/水下物体)= pbr vert 减 morph/切线。
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
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;
  vec4 uvTransform;
  vec4 ext0;
  vec4 ext1;
  vec4 ext2;
};
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
void main() {
  vWorldPos = (world * vec4(aPos, 1.0)).xyz;
  vNormal = (normalMatrix * vec4(aNormal, 0.0)).xyz;
  vUV = aUV * uvTransform.zw + uvTransform.xy;
  gl_Position = mvp * vec4(aPos, 1.0);
}
