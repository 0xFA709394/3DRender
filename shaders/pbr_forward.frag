// pbr_forward.frag：glTF metallic-roughness + 1 方向光 + IBL（SH diffuse + prefilter specular）。
// slot：0=baseColor(b4) 1=MR(b5) 2=normal(b6) 3=emissive(b7) 4=occlusion(b8)
//       5=prefilterCube(b9) 6=brdfLut(b10)；FrameUBO(b0) ItemUBO(b1)。
#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUV;

layout(binding = 0) uniform FrameUBO {
  mat4 viewProj;
  vec4 cameraPos;
  vec4 lightDir;      // 指向光源的方向
  vec4 lightColor;
  vec4 sh[9];         // xyz=SH 系数（Ã 已折叠）
};
layout(binding = 1) uniform ItemUBO {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;
  vec4 uvTransform;
};

layout(binding = 4) uniform sampler2D texBaseColor;
layout(binding = 5) uniform sampler2D texMR;
layout(binding = 6) uniform sampler2D texNormal;
layout(binding = 7) uniform sampler2D texEmissive;
layout(binding = 8) uniform sampler2D texOcclusion;
layout(binding = 9) uniform samplerCube texPrefilter;
layout(binding = 10) uniform sampler2D texBrdfLut;

layout(location = 0) out vec4 outColor;

const float PREFILTER_MIPS = 5.0;
const float PI = 3.14159265;

vec3 evalIrradiance(vec3 n) {
  // 与 environment.cpp 同一组正交归一 SH 基常量
  float yb[9];
  yb[0] = 0.282095;
  yb[1] = 0.488603 * n.y;
  yb[2] = 0.488603 * n.z;
  yb[3] = 0.488603 * n.x;
  yb[4] = 1.092548 * n.x * n.y;
  yb[5] = 1.092548 * n.y * n.z;
  yb[6] = 1.092548 * n.x * n.z;
  yb[7] = 0.546274 * (n.x * n.x - n.y * n.y);
  yb[8] = 0.315392 * (3.0 * n.z * n.z - 1.0);
  vec3 e = vec3(0.0);
  for (int i = 0; i < 9; ++i) e += sh[i].xyz * yb[i];
  return max(e, vec3(0.0));
}

// 方向光 GGX 高光(D·G·F/(4·ndl·ndv)),fres 输出供 Fresnel 复用
vec3 ggxSpec(vec3 n, vec3 l, vec3 v, float roughness, vec3 f0) {
  vec3 h = normalize(l + v);
  float ndh = clamp(dot(n, h), 0.0, 1.0);
  float ndl = clamp(dot(n, l), 0.0, 1.0);
  float ndv = clamp(dot(n, v), 0.0, 1.0);
  float vdh = clamp(dot(v, h), 0.0, 1.0);
  float a = roughness * roughness;
  float dDen = ndh * ndh * (a * a - 1.0) + 1.0;
  float d = (a * a) / (PI * dDen * dDen + 1e-7);
  float k = a / 2.0;
  float gv = ndv / (ndv * (1.0 - k) + k + 1e-7);
  float gl = ndl / (ndl * (1.0 - k) + k + 1e-7);
  vec3 fres = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
  return fres * (d * gv * gl / (4.0 * ndv * ndl + 1e-7));
}

void main() {
  vec4 baseColor = texture(texBaseColor, vUV) * baseColorFactor;
  vec2 mr = texture(texMR, vUV).bg;   // glTF: G=roughness, B=metallic
  float metallic = clamp(mr.y * metallicRoughness.x, 0.0, 1.0);
  float roughness = clamp(mr.x * metallicRoughness.y, 0.03, 1.0);

  // 法线贴图(TBN)
  vec3 n = normalize(vNormal);
  vec3 t = normalize(vTangent.xyz - n * dot(n, vTangent.xyz));
  vec3 b = cross(n, t) * vTangent.w;
  vec3 nMap = (texture(texNormal, vUV).xyz * 2.0 - 1.0) *
              vec3(metallicRoughness.z, metallicRoughness.z, 1.0);
  n = normalize(t * nMap.x + b * nMap.y + n * nMap.z);

  vec3 v = normalize(cameraPos.xyz - vWorldPos);
  vec3 r = reflect(-v, n);
  vec3 f0 = mix(vec3(0.04), baseColor.rgb, metallic);

  // IBL:SH diffuse + prefilter specular(split-sum)
  vec3 irradiance = evalIrradiance(n);
  vec3 iblDiffuse = irradiance * baseColor.rgb * (1.0 - metallic);
  vec3 prefiltered = textureLod(texPrefilter, r, roughness * (PREFILTER_MIPS - 1.0)).rgb;
  vec2 brdf = texture(texBrdfLut, vec2(clamp(dot(n, v), 0.0, 1.0), roughness)).rg;
  vec3 iblSpec = prefiltered * (f0 * brdf.x + brdf.y);

  // 1 方向光
  vec3 l = normalize(lightDir.xyz);
  float ndl = clamp(dot(n, l), 0.0, 1.0);
  vec3 direct = lightColor.rgb * ndl *
                (baseColor.rgb * (1.0 - metallic) / PI + ggxSpec(n, l, v, roughness, f0));

  float ao = mix(1.0, texture(texOcclusion, vUV).r, emissiveOcclusion.a);
  vec3 emissive = texture(texEmissive, vUV).rgb * emissiveOcclusion.rgb;

  vec3 color = (iblDiffuse + iblSpec + direct) * ao + emissive;
  color = color / (color + vec3(1.0));          // Reinhard
  outColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
}
