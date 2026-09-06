// pbr_forward_morph_skinned.vert:morph+蒙皮版 PBR 顶点着色器。
// location 4=joints4f@48,5=weights4f@64(stride 80);JointUBO(b3)=joints[128]。
// 先 morph(绑定姿态空间)后 skin(glTF 语义);法线用 mat3(skin) 近似。
// texMorph=slot19(binding24,RGBA16F):行 t*2=POSITION,t*2+1=NORMAL。
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
  vec4 ext0;  // (vert 不读,GLES 跨阶段声明对齐)
  vec4 ext1;  // (同上)
  vec4 ext2;  // (同上)
  vec4 ext3;  // w=morphTargetCount
  vec4 ext4;  // w=morphTargetCount(同 ext3.w)
  vec4 ext5;  // morph weights[0..3]
  vec4 ext6;  // morph weights[4..7]
};
layout(binding = 3) uniform JointUBO { mat4 joints[128]; } jubo;
layout(binding = 24) uniform texture2D texMorph;   // slot19(binding 例外)
layout(binding = 23) uniform sampler smpMat;       // 共享(texelFetch 不滤波)

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUV;

void main() {
  vec3 pos = aPos;
  vec3 nrm = aNormal;
  const int mc = int(ext4.w + 0.5);
  if (mc > 0) {
    float wts[8] = float[8](ext5.x, ext5.y, ext5.z, ext5.w,
                            ext6.x, ext6.y, ext6.z, ext6.w);
    vec3 dP = vec3(0.0);
    vec3 dN = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
      if (i >= mc) break;
      dP += texelFetch(sampler2D(texMorph, smpMat),
                       ivec2(int(gl_VertexIndex), i * 2), 0).xyz * wts[i];
      dN += texelFetch(sampler2D(texMorph, smpMat),
                       ivec2(int(gl_VertexIndex), i * 2 + 1), 0).xyz * wts[i];
    }
    pos += dP;
    nrm += dN;
  }
  mat4 skin = aWeights.x * jubo.joints[int(aJoints.x + 0.5)] +
              aWeights.y * jubo.joints[int(aJoints.y + 0.5)] +
              aWeights.z * jubo.joints[int(aJoints.z + 0.5)] +
              aWeights.w * jubo.joints[int(aJoints.w + 0.5)];
  vec4 p = skin * vec4(pos, 1.0);   // 先 morph 后 skin
  vWorldPos = (world * p).xyz;
  vNormal = (normalMatrix * vec4(mat3(skin) * nrm, 0.0)).xyz;
  vTangent = vec4((normalMatrix * vec4(mat3(skin) * aTangent.xyz, 0.0)).xyz, aTangent.w);
  vUV = aUV * uvTransform.zw + uvTransform.xy;
  gl_Position = mvp * p;
}
