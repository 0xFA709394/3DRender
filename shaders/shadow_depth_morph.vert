// shadow_depth_morph.vert:morph 版深度写出(texMorph 行 t*2=POSITION 增量)。
// ShadowUBO=lightViewProj;ItemUBO 取 ext4.w/ext5/ext6(权重);normal 不需要。
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
  vec4 ext0;
  vec4 ext1;
  vec4 ext2;
  vec4 ext3;  // w=morphTargetCount
  vec4 ext4;  // w=morphTargetCount(同 ext3.w)
  vec4 ext5;  // morph weights[0..3]
  vec4 ext6;  // morph weights[4..7]
} item;
layout(binding = 24) uniform texture2D texMorph;
layout(binding = 23) uniform sampler smpMat;
void main() {
  vec3 pos = aPos;
  const int mc = int(item.ext4.w + 0.5);
  if (mc > 0) {
    float wts[8] = float[8](item.ext5.x, item.ext5.y, item.ext5.z, item.ext5.w,
                            item.ext6.x, item.ext6.y, item.ext6.z, item.ext6.w);
    vec3 dP = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
      if (i >= mc) break;
      dP += texelFetch(sampler2D(texMorph, smpMat),
                       ivec2(int(gl_VertexIndex), i * 2), 0).xyz * wts[i];
    }
    pos += dP;
  }
  gl_Position = u.lightViewProj * item.world * vec4(pos, 1.0);
}
