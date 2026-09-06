// water_receiver.frag:简化 pbr 受水体——漫反射 + 方向光阴影 PCF + SH 环境 +
// 焦散调制(世界 xz→池 uv;水下按深度衰减,水线上方无焦散)。
// 槽位:texBaseColor=slot0(binding4) texCaustics=slot2(binding6)
//       texShadow=slot7(binding11,比较采样);FrameUBO=slot0;ItemUBO=slot1;LightUBO=slot2。
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
layout(binding = 4) uniform sampler2D texBaseColor;
layout(binding = 6) uniform sampler2D texCaustics;
layout(binding = 11) uniform sampler2DShadow texShadow;
layout(location = 0) out vec4 outColor;
const float PI = 3.14159265;
vec3 evalIrradiance(vec3 n) {
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
void main() {
  vec3 albedo = texture(texBaseColor, vUV).rgb * baseColorFactor.rgb;
  vec3 n = normalize(vNormal);
  // 首盏方向光 + 阴影 PCF 3x3(与 pbr_forward 同款;bias 随坡度放大)
  vec3 direct = vec3(0.0);
  float ndl = 0.0;
  if (lu.lightCount.x > 0.5 && lu.lights[0].w < 0.5) {
    vec3 L = normalize(lu.lights[0].xyz);
    ndl = clamp(dot(n, L), 0.0, 1.0);
    float shadowF = 1.0;
    if (lu.shadowParams.z > 0.5) {
      vec4 lp = lu.lightViewProj * vec4(vWorldPos, 1.0);
      vec3 ndc = lp.xyz / lp.w;
      vec2 suv;
      suv.x = ndc.x * 0.5 + 0.5;
      suv.y = lu.shadowParams.w > 0.5 ? ndc.y * 0.5 + 0.5 : 0.5 - ndc.y * 0.5;
      float bias = max(lu.shadowParams.x * (1.0 - ndl), lu.shadowParams.x * 0.2);
      float refZ = ndc.z - bias;
      if (suv.x >= 0.0 && suv.x <= 1.0 && suv.y >= 0.0 && suv.y <= 1.0) {
        float sum = 0.0;
        for (int x = -1; x <= 1; ++x)
          for (int y = -1; y <= 1; ++y)
            sum += texture(texShadow,
                           vec3(suv + vec2(float(x), float(y)) * lu.shadowParams.y, refZ));
        shadowF = sum / 9.0;
      }
    }
    // 焦散:水下(低于水面)按吸收衰减;水线上方为 0
    float caust = 0.0;
    if (water[1].z > 0.5) {
      vec2 wuv = vWorldPos.xz / water[0].xy + 0.5;
      float underWater = clamp((water[0].z - vWorldPos.y) / max(water[1].x, 1e-3), 0.0, 1.0);
      caust = texture(texCaustics, clamp(wuv, vec2(0.0), vec2(1.0))).r;
      caust *= exp(-2.0 * (1.0 - underWater) - 0.4) * underWater;  // 近水面亮,深处/线上衰减
    }
    direct = lu.lights[2].rgb * ndl * shadowF * (1.0 + caust * water[1].y);
  }
  vec3 ambient = evalIrradiance(n) * albedo * 0.45;
  vec3 color = albedo / PI * direct + ambient;
  if (lu.lightCount.y > 0.5) {
    outColor = vec4(color, 1.0);
  } else {
    color = color / (color + vec3(1.0));
    outColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
  }
}
