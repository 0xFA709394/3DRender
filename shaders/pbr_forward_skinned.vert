// pbr_forward_skinned.vert:蒙皮版 PBR 顶点着色器。
// location 4=joints4f@48,5=weights4f@64(stride 80);JointUBO(b3)=joints[128]。
// 蒙皮在模型空间进行(skin·pos 之后乘 world);法线用 mat3(skin) 近似。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aUV;
layout(location = 4) in vec4 aJoints;
layout(location = 5) in vec4 aWeights;

layout(binding = 1) uniform ItemUBO {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;
  vec4 uvTransform;
  vec4 ext0;  // x=clearcoatFactor y=clearcoatRoughness z=clearcoatNormalScale w=specularFactor(vert 不读,GLES 跨阶段声明对齐)
  vec4 ext1;  // xyz=sheenColorFactor w=sheenRoughnessFactor(同上)
  vec4 ext2;  // xyz=specularColorFactor w=ior(同上)
  vec4 ext3;  // transmission/morph 计数(vert 不读,对齐)
  vec4 ext4;  // 同上
  vec4 ext5;  // morph weights[0..3](同上)
  vec4 ext6;  // morph weights[4..7](同上)
};
layout(binding = 3) uniform JointUBO { mat4 joints[128]; } jubo;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUV;

void main() {
  mat4 skin = aWeights.x * jubo.joints[int(aJoints.x + 0.5)] +
              aWeights.y * jubo.joints[int(aJoints.y + 0.5)] +
              aWeights.z * jubo.joints[int(aJoints.z + 0.5)] +
              aWeights.w * jubo.joints[int(aJoints.w + 0.5)];
  vec4 p = skin * vec4(aPos, 1.0);
  vWorldPos = (world * p).xyz;
  vNormal = (normalMatrix * vec4(mat3(skin) * aNormal, 0.0)).xyz;
  vTangent = vec4((normalMatrix * vec4(mat3(skin) * aTangent.xyz, 0.0)).xyz, aTangent.w);
  vUV = aUV * uvTransform.zw + uvTransform.xy;
  gl_Position = mvp * p;
}
