// shadow_depth_morph_skinned.vert:morph+蒙皮版深度写出。
// locations 4/5=joints/weights(stride 80);先 morph(绑定姿态)后 skin。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 4) in vec4 aJoints;
layout(location = 5) in vec4 aWeights;
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
layout(binding = 3) uniform JointUBO { mat4 joints[128]; } jubo;
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
  mat4 skin = aWeights.x * jubo.joints[int(aJoints.x + 0.5)] +
              aWeights.y * jubo.joints[int(aJoints.y + 0.5)] +
              aWeights.z * jubo.joints[int(aJoints.z + 0.5)] +
              aWeights.w * jubo.joints[int(aJoints.w + 0.5)];
  gl_Position = u.lightViewProj * item.world * skin * vec4(pos, 1.0);
}
