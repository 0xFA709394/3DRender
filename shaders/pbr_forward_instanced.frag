// pbr_forward_instanced.frag:pbr_forward 的实例化变体;ItemUBO 按 vItem 索引。
// slot：0=baseColor(b4) 1=MR(b5) 2=normal(b6) 3=emissive(b7) 4=occlusion(b8)
//       5=prefilterCube(b9) 6=brdfLut(b10) 7=shadowMap(b11);
//       FrameUBO(b0) ItemUBO(b1) LightUBO(b2)。
#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUV;
layout(location = 4) flat in uint vItem;

layout(binding = 0) uniform FrameUBO {
  mat4 viewProj;
  vec4 cameraPos;
  vec4 lightDir;      // 占位(多光源后由 LightUBO 接管)
  vec4 lightColor;    // 占位
  vec4 sh[9];         // xyz=SH 系数（Ã 已折叠）
};
struct Item {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;
  vec4 uvTransform;
};
layout(binding = 1) uniform ItemUBO { Item items[64]; } iu;
layout(binding = 2) uniform LightUBO {
  mat4 lightViewProj;
  mat4 spotViewProj;   // 首盏聚光阴影 VP
  vec4 shadowParams;   // x=bias, y=1/shadowMapSize, z=shadowOn, w=vFlip
  vec4 spotShadowParams;  // 聚光同构
  vec4 lightCount;     // x=count, y=hdrMode, z=首盏聚光下标(-1=无)
  vec4 lights[16];     // 4 盏 × 4 vec4(dirType|posRange|color|spot)
};

layout(binding = 4) uniform sampler2D texBaseColor;
layout(binding = 5) uniform sampler2D texMR;
layout(binding = 6) uniform sampler2D texNormal;
layout(binding = 7) uniform sampler2D texEmissive;
layout(binding = 8) uniform sampler2D texOcclusion;
layout(binding = 9) uniform samplerCube texPrefilter;
layout(binding = 10) uniform sampler2D texBrdfLut;
layout(binding = 11) uniform sampler2DShadow texShadow;
layout(binding = 12) uniform sampler2DShadow texShadowSpot;  // slot8:聚光阴影

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
  vec4 baseColor = texture(texBaseColor, vUV) * iu.items[vItem].baseColorFactor;
  // alphaMode=MASK:cutoff(iu.items[vItem].metallicRoughness.w)> 0 时按阈值裁剪
  if (iu.items[vItem].metallicRoughness.w > 0.0 && baseColor.a < iu.items[vItem].metallicRoughness.w) discard;
  vec2 mr = texture(texMR, vUV).bg;   // glTF: G=roughness, B=metallic
  float metallic = clamp(mr.y * iu.items[vItem].metallicRoughness.x, 0.0, 1.0);
  float roughness = clamp(mr.x * iu.items[vItem].metallicRoughness.y, 0.03, 1.0);

  // 法线贴图(TBN)
  vec3 n = normalize(vNormal);
  vec3 t = normalize(vTangent.xyz - n * dot(n, vTangent.xyz));
  vec3 b = cross(n, t) * vTangent.w;
  vec3 nMap = (texture(texNormal, vUV).xyz * 2.0 - 1.0) *
              vec3(iu.items[vItem].metallicRoughness.z, iu.items[vItem].metallicRoughness.z, 1.0);
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

  // 多光源 direct(首盏方向光投影阴影)
  vec3 direct = vec3(0.0);
  const int nLights = min(int(lightCount.x + 0.5), 4);
  for (int i = 0; i < nLights; ++i) {
    vec4 dirType = lights[i * 4 + 0];
    vec4 posRange = lights[i * 4 + 1];
    vec3 lcolor = lights[i * 4 + 2].rgb;
    vec4 spot = lights[i * 4 + 3];
    int type = int(dirType.w + 0.5);
    vec3 L;
    float att = 1.0;
    if (type == 0) {
      L = normalize(dirType.xyz);
    } else {
      vec3 toL = posRange.xyz - vWorldPos;
      float dist = length(toL);
      L = toL / max(dist, 1e-4);
      if (posRange.w > 0.0) {
        float t = clamp(1.0 - dist / posRange.w, 0.0, 1.0);
        att = t * t;
      }
      if (type == 2) {
        float cd = dot(-L, normalize(dirType.xyz));
        float t = clamp((cd - spot.y) / max(spot.x - spot.y, 1e-4), 0.0, 1.0);
        att *= t * t;
      }
    }
    float ndl = clamp(dot(n, L), 0.0, 1.0);
    vec3 term = lcolor * att * ndl *
                (baseColor.rgb * (1.0 - metallic) / PI + ggxSpec(n, L, v, roughness, f0));
    if (i == 0 && type == 0) {
      // 阴影:PCF 3x3(bias 随坡度放大);采样坐标越界视为受光
      float shadow = 1.0;
      if (shadowParams.z > 0.5) {
        vec4 lp = lightViewProj * vec4(vWorldPos, 1.0);
        vec3 ndc = lp.xyz / lp.w;
        vec2 suv;
        suv.x = ndc.x * 0.5 + 0.5;
        suv.y = shadowParams.w > 0.5 ? ndc.y * 0.5 + 0.5 : 0.5 - ndc.y * 0.5;
        float bias = max(shadowParams.x * (1.0 - ndl), shadowParams.x * 0.2);
        float refZ = ndc.z - bias;
        if (suv.x >= 0.0 && suv.x <= 1.0 && suv.y >= 0.0 && suv.y <= 1.0) {
          float sum = 0.0;
          for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
              sum += texture(texShadow,
                             vec3(suv + vec2(float(x), float(y)) * shadowParams.y, refZ));
          shadow = sum / 9.0;
        }
      }
      term *= shadow;
    }
    // 聚光阴影:首盏聚光(lightCount.z)投影,PCF 3x3
    if (int(lightCount.z + 0.5) == i && spotShadowParams.z > 0.5) {
      vec4 lp = spotViewProj * vec4(vWorldPos, 1.0);
      vec3 ndc = lp.xyz / lp.w;
      vec2 suv;
      suv.x = ndc.x * 0.5 + 0.5;
      suv.y = spotShadowParams.w > 0.5 ? ndc.y * 0.5 + 0.5 : 0.5 - ndc.y * 0.5;
      float bias = max(spotShadowParams.x * (1.0 - ndl), spotShadowParams.x * 0.2);
      float refZ = ndc.z - bias;
      if (suv.x >= 0.0 && suv.x <= 1.0 && suv.y >= 0.0 && suv.y <= 1.0 &&
          ndc.z >= 0.0 && ndc.z <= 1.0) {
        float sum = 0.0;
        for (int x = -1; x <= 1; ++x)
          for (int y = -1; y <= 1; ++y)
            sum += texture(texShadowSpot,
                           vec3(suv + vec2(float(x), float(y)) * spotShadowParams.y, refZ));
        term *= sum / 9.0;
      }
    }
    direct += term;
  }

  float ao = mix(1.0, texture(texOcclusion, vUV).r, iu.items[vItem].emissiveOcclusion.a);
  vec3 emissive = texture(texEmissive, vUV).rgb * iu.items[vItem].emissiveOcclusion.rgb;

  vec3 color = (iblDiffuse + iblSpec + direct) * ao + emissive;
  if (lightCount.y > 0.5) {
    outColor = vec4(color, 1.0);  // hdrMode:线性输出,tone mapping 在 composite
  } else {
    color = color / (color + vec3(1.0));          // LDR:Reinhard(现状)
    outColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
  }
}
