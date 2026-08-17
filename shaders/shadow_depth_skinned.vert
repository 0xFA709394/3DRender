// shadow_depth_skinned.vert:蒙皮版深度写出(ShadowUBO=lightViewProj,ItemUBO=world,
// JointUBO(b3)=joints)。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 4) in vec4 aJoints;
layout(location = 5) in vec4 aWeights;
layout(binding = 0) uniform ShadowUBO { mat4 lightViewProj; } u;
layout(binding = 1) uniform ItemUBO {
  mat4 mvp; mat4 world; mat4 normalMatrix;
  vec4 baseColorFactor; vec4 emissiveOcclusion; vec4 metallicRoughness; vec4 uvTransform;
} item;
layout(binding = 3) uniform JointUBO { mat4 joints[128]; } jubo;
void main() {
  mat4 skin = aWeights.x * jubo.joints[int(aJoints.x + 0.5)] +
              aWeights.y * jubo.joints[int(aJoints.y + 0.5)] +
              aWeights.z * jubo.joints[int(aJoints.z + 0.5)] +
              aWeights.w * jubo.joints[int(aJoints.w + 0.5)];
  gl_Position = u.lightViewProj * item.world * skin * vec4(aPos, 1.0);
}
