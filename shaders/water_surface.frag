// water_surface.frag:Schlick Fresnel + IBL 反射 + 方向光 GGX 高光 + 折射水色(半透明 blend)。
// alpha≈F(垂直透视池底,掠射全反射);hdrMode 与 pbr 一致(线性 or Reinhard+gamma)。
// 槽位:texPrefilter=slot5(binding9);FrameUBO=slot0;ItemUBO=slot1;LightUBO=slot2。
#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(binding = 0) uniform FrameUBO {
  mat4 viewProj;
  vec4 cameraPos;
  vec4 lightDir;
  vec4 lightColor;
  vec4 sh[9];
  vec4 transmissionParams;
  vec4 water[3];
};
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
layout(binding = 2) uniform LightUBO {
  mat4 lightViewProj;
  mat4 spotViewProj;
  vec4 shadowParams;
  vec4 spotShadowParams;
  vec4 lightCount;
  vec4 lights[16];
} lu;
layout(binding = 9) uniform samplerCube texPrefilter;
layout(location = 0) out vec4 outColor;
const float PI = 3.14159265;
float ggxD(float ndh, float a) {
  float dn = ndh * ndh * (a * a - 1.0) + 1.0;
  return (a * a) / (PI * dn * dn + 1e-7);
}
void main() {
  vec3 n = normalize(vNormal);
  vec3 v = normalize(cameraPos.xyz - vWorldPos);
  float ndv = clamp(dot(n, v), 0.0, 1.0);
  float F = 0.02 + 0.98 * pow(1.0 - ndv, 5.0);
  // IBL 反射(rough 0.05 → mip 0.2)
  vec3 refl = textureLod(texPrefilter, reflect(-v, n), 0.2).rgb;
  // 方向光 GGX 高光(首盏方向光;水面不接收阴影)
  vec3 sunSpec = vec3(0.0);
  if (lu.lightCount.x > 0.5 && lu.lights[0].w < 0.5) {
    vec3 L = normalize(lu.lights[0].xyz);
    vec3 h = normalize(L + v);
    float ndl = clamp(dot(n, L), 0.0, 1.0);
    float ndh = clamp(dot(n, h), 0.0, 1.0);
    sunSpec = lu.lights[2].rgb * ndl * F * ggxD(ndh, 0.0025);
  }
  // 折射水色:baseColorFactor 水色 × Beer-Lambert 深度吸收(蓝移)
  vec3 absorb = exp(-water[1].x * vec3(0.35, 0.14, 0.08));
  vec3 refr = baseColorFactor.rgb * absorb;
  float alpha = clamp(0.25 + 0.75 * F, 0.0, 1.0);
  vec3 color = mix(refr, refl, F) + sunSpec;
  if (lu.lightCount.y > 0.5) {
    outColor = vec4(color, alpha);  // hdrMode:线性(composite 做 tone map)
  } else {
    color = color / (color + vec3(1.0));
    outColor = vec4(pow(color, vec3(1.0 / 2.2)), alpha);
  }
}
