// pbr_forward_morph.vert:morph 版 PBR 顶点着色器(增量纹理 texelFetch 累加)。
// texMorph=slot19(binding24,RGBA16F):行 t*2=POSITION 增量,t*2+1=NORMAL 增量;
// ItemUBO ext5/ext6=weights[8],ext4.w=目标数;先 morph 后世界变换(非蒙皮)。
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
  vec4 ext0;  // (vert 不读,GLES 跨阶段声明对齐)
  vec4 ext1;  // (同上)
  vec4 ext2;  // (同上)
  vec4 ext3;  // w=morphTargetCount
  vec4 ext4;  // w=morphTargetCount(同 ext3.w)
  vec4 ext5;  // morph weights[0..3]
  vec4 ext6;  // morph weights[4..7]
};
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
  vWorldPos = (world * vec4(pos, 1.0)).xyz;
  vNormal = (normalMatrix * vec4(nrm, 0.0)).xyz;
  vTangent = vec4((normalMatrix * vec4(aTangent.xyz, 0.0)).xyz, aTangent.w);
  vUV = aUV * uvTransform.zw + uvTransform.xy;
  gl_Position = mvp * vec4(pos, 1.0);
}
