// pbr_forward_instanced.frag:pbr_forward 的实例化变体;ItemUBO 按 vItem 索引。
//   KHR 扩展四件套(clearcoat/sheen/specular/ior)与 pbr_forward.frag 同构。
// slot：0=baseColor(b4) 1=MR(b5) 2=normal(b6) 3=emissive(b7) 4=occlusion(b8)
//       5=prefilterCube(b9) 6=brdfLut(b10) 7=shadowMap(b11) 8=spotShadow(b12)
//       9=clearcoat(b13) 10=clearcoatRough(b14) 11=clearcoatNormal(b15)
//       12=sheenColor(b16) 13=sheenRough(b17) 14=specularColor(b18) 15=specular(b19);
//       FrameUBO(b0) ItemUBO(b1,元素 512B) LightUBO(b2)。
// 分离采样器模型同 pbr_forward.frag(15 texture2D + 共享 smpMat@23;cube/lut/shadow combined)。
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
  vec4 transmissionParams;  // x=1/transW y=1/transH z=maxLod w=0
};
struct Item {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;   // rgb=emissiveFactor, a=occlusionStrength
  vec4 metallicRoughness;   // x=metallic, y=roughness, z=normalScale, w=alphaCutoff
  vec4 uvTransform;         // xy=offset, zw=scale
  vec4 ext0;  // x=clearcoatFactor y=clearcoatRoughness z=clearcoatNormalScale w=specularFactor
  vec4 ext1;  // xyz=sheenColorFactor w=sheenRoughnessFactor
  vec4 ext2;  // xyz=specularColorFactor w=ior
  vec4 ext3;  // x=transmissionFactor y=thicknessFactor z=attenuationDistance(0=∞) w=morphTargetCount
  vec4 ext4;  // xyz=attenuationColor w=morphTargetCount(同 ext3.w)
  vec4 ext5;  // morph weights[0..3](instanced 不消费;块对齐)
  vec4 ext6;  // morph weights[4..7]
  vec4 _pad[9];  // std140 数组元素 stride 对齐 CPU 槽距 512B(368+144)
};
layout(binding = 1) uniform ItemUBO { Item items[32]; } iu;  // 组上限 32(16KB 线)
layout(binding = 2) uniform LightUBO {
  mat4 lightViewProj;
  mat4 spotViewProj;   // 首盏聚光阴影 VP
  vec4 shadowParams;   // x=bias, y=1/shadowMapSize, z=shadowOn, w=vFlip
  vec4 spotShadowParams;  // 聚光同构
  vec4 lightCount;     // x=count, y=hdrMode, z=首盏聚光下标(-1=无)
  vec4 lights[16];     // 4 盏 × 4 vec4(dirType|posRange|color|spot)
};

layout(binding = 4) uniform texture2D texBaseColor;       // slot0
layout(binding = 5) uniform texture2D texMR;              // slot1
layout(binding = 6) uniform texture2D texNormal;          // slot2
layout(binding = 7) uniform texture2D texEmissive;        // slot3
layout(binding = 8) uniform texture2D texOcclusion;       // slot4
layout(binding = 9) uniform samplerCube texPrefilter;     // slot5(combined)
layout(binding = 10) uniform sampler2D texBrdfLut;        // slot6(combined,nearest)
layout(binding = 11) uniform sampler2DShadow texShadow;   // slot7(combined,比较采样)
layout(binding = 12) uniform sampler2DShadow texShadowSpot; // slot8(combined,比较采样)
layout(binding = 13) uniform texture2D texClearcoat;       // slot9:R=清漆强度
layout(binding = 14) uniform texture2D texClearcoatRough;  // slot10:G=清漆粗糙度
layout(binding = 15) uniform texture2D texClearcoatNormal; // slot11:清漆法线(缺省平面法线占位)
layout(binding = 16) uniform texture2D texSheenColor;      // slot12:RGB
layout(binding = 17) uniform texture2D texSheenRough;      // slot13:A=粗糙度
layout(binding = 18) uniform texture2D texSpecularColor;   // slot14:RGB
layout(binding = 19) uniform texture2D texSpecular;        // slot15:A=specular 因子
layout(binding = 20) uniform texture2D texTransmissionScene;  // slot16:场景色拷贝(mip 链)
layout(binding = 21) uniform texture2D texTransmission;       // slot17:R=透射强度
layout(binding = 22) uniform texture2D texThickness;          // slot18:G=厚度
layout(binding = 23) uniform sampler smpMat;  // 共享材质采样器(线性+repeat;UV 差异 shader 内 clamp)

layout(location = 0) out vec4 outColor;

const float PREFILTER_MIPS = 5.0;
const float PI = 3.14159265;
// Charlie 方向反照率无 LUT 解析拟合(three.js 惯例;与 Khronos viewer 的 LUT 版
// 有微小数值差异,golden 自生成自洽)
const float kSheenAlbedo = 0.157;

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

// 方向光 GGX 高光(D·G·F/(4·ndl·ndv)),fres 输出供分层衰减/漫反射能量扣复用
vec3 ggxSpec(vec3 n, vec3 l, vec3 v, float roughness, vec3 f0, out vec3 fres) {
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
  fres = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
  return fres * (d * gv * gl / (4.0 * ndv * ndl + 1e-7));
}

// Charlie 分布(KHR_materials_sheen 附录;alpha = roughness²)
float sheenD(float roughness, float ndh) {
  float a = roughness * roughness;
  float invA = 1.0 / max(a, 1e-4);
  float sin2h = max(1.0 - ndh * ndh, 0.0078125);
  return (2.0 + invA) * pow(sin2h, invA * 0.5) / (2.0 * PI);
}

// Neubelt 可见性(解析;整体能量守恒由 kSheenAlbedo 拟合承担)
float sheenV(float ndl, float ndv) {
  return clamp(1.0 / (4.0 * (ndl + ndv - ndl * ndv)), 0.0, 1.0);
}

void main() {
  vec4 baseColor = texture(sampler2D(texBaseColor, smpMat), vUV) * iu.items[vItem].baseColorFactor;
  // alphaMode=MASK:cutoff(iu.items[vItem].metallicRoughness.w)> 0 时按阈值裁剪
  if (iu.items[vItem].metallicRoughness.w > 0.0 &&
      baseColor.a < iu.items[vItem].metallicRoughness.w) discard;
  vec2 mr = texture(sampler2D(texMR, smpMat), vUV).bg;   // glTF: G=roughness, B=metallic
  float metallic = clamp(mr.y * iu.items[vItem].metallicRoughness.x, 0.0, 1.0);
  float roughness = clamp(mr.x * iu.items[vItem].metallicRoughness.y, 0.03, 1.0);

  // 法线贴图(TBN;清漆法线共享此切线空间)
  vec3 n = normalize(vNormal);
  vec3 t = normalize(vTangent.xyz - n * dot(n, vTangent.xyz));
  vec3 b = cross(n, t) * vTangent.w;
  vec3 nMap = (texture(sampler2D(texNormal, smpMat), vUV).xyz * 2.0 - 1.0) *
              vec3(iu.items[vItem].metallicRoughness.z,
                   iu.items[vItem].metallicRoughness.z, 1.0);
  n = normalize(t * nMap.x + b * nMap.y + n * nMap.z);

  vec3 v = normalize(cameraPos.xyz - vWorldPos);
  vec3 r = reflect(-v, n);
  float ndv = clamp(dot(n, v), 0.0, 1.0);

  // ---- KHR_materials_specular + ior:介质 f0 改造(全默认 → 跳过,零回归)----
  float specWeight = 1.0;
  vec3 f0d = vec3(0.04);
  const bool specIor = iu.items[vItem].ext0.w != 1.0 || iu.items[vItem].ext2.w != 1.5 ||
                       iu.items[vItem].ext2.x != 1.0 || iu.items[vItem].ext2.y != 1.0 ||
                       iu.items[vItem].ext2.z != 1.0;
  if (specIor) {
    specWeight = clamp(iu.items[vItem].ext0.w * texture(sampler2D(texSpecular, smpMat), vUV).a, 0.0, 1.0);
    vec3 specColor = clamp(
        iu.items[vItem].ext2.xyz * texture(sampler2D(texSpecularColor, smpMat), vUV).rgb, vec3(0.0),
        vec3(1.0));
    float k = (1.0 - iu.items[vItem].ext2.w) / (1.0 + iu.items[vItem].ext2.w);
    f0d = min(k * k * specColor, vec3(1.0));
  }
  vec3 f0 = mix(f0d, baseColor.rgb, metallic);

  // ---- KHR_materials_sheen(默认 sheenColorFactor=0 → 跳过)----
  vec3 sheenColor = vec3(0.0);
  float sheenRough = 0.0;
  const bool sheenOn = max(max(iu.items[vItem].ext1.x, iu.items[vItem].ext1.y),
                           iu.items[vItem].ext1.z) > 0.0;
  if (sheenOn) {
    sheenColor = iu.items[vItem].ext1.xyz * texture(sampler2D(texSheenColor, smpMat), vUV).rgb;
    sheenRough = clamp(iu.items[vItem].ext1.w * texture(sampler2D(texSheenRough, smpMat), vUV).a, 0.03, 1.0);
  }

  // ---- KHR_materials_clearcoat(默认 factor=0 → 跳过)----
  float ccFactor = 0.0, ccRough = 0.0;
  vec3 ncc = n;
  const bool ccOn = iu.items[vItem].ext0.x > 0.0;
  if (ccOn) {
    ccFactor = clamp(iu.items[vItem].ext0.x * texture(sampler2D(texClearcoat, smpMat), vUV).r, 0.0, 1.0);
    ccRough = clamp(iu.items[vItem].ext0.y * texture(sampler2D(texClearcoatRough, smpMat), vUV).g, 0.03, 1.0);
    vec3 nMapCc = (texture(sampler2D(texClearcoatNormal, smpMat), vUV).xyz * 2.0 - 1.0) *
                  vec3(iu.items[vItem].ext0.z, iu.items[vItem].ext0.z, 1.0);
    ncc = normalize(t * nMapCc.x + b * nMapCc.y + n * nMapCc.z);
  }

  // ---- KHR transmission + volume(默认 factor=0 → 跳过;transmission 项不参与
  //      实例化分组,此路径实际不触发,保持与 pbr_forward.frag 同构)----
  float transFactor = 0.0;
  vec3 transmitted = vec3(0.0);
  const bool transOn = iu.items[vItem].ext3.x > 0.0;
  if (transOn) {
    transFactor =
        clamp(iu.items[vItem].ext3.x * texture(sampler2D(texTransmission, smpMat), vUV).r,
              0.0, 1.0);
    float thickness =
        iu.items[vItem].ext3.y * texture(sampler2D(texThickness, smpMat), vUV).g;
    vec2 suv = gl_FragCoord.xy * transmissionParams.xy;
    vec3 refr = refract(-v, n, 1.0 / clamp(iu.items[vItem].ext2.w, 1.001, 3.0));
    suv += refr.xy * max(thickness, 0.05) * transmissionParams.xy * 4.0;
    float lod = roughness * transmissionParams.z;
    transmitted = textureLod(sampler2D(texTransmissionScene, smpMat),
                             clamp(suv, vec2(0.0), vec2(1.0)), lod).rgb;
    if (iu.items[vItem].ext3.z > 0.0 && thickness > 0.0) {
      vec3 atten = clamp(iu.items[vItem].ext4.xyz, vec3(1e-4), vec3(1.0));
      transmitted *= exp(-log(atten) / iu.items[vItem].ext3.z * thickness);
    }
  }

  // IBL(基层):SH diffuse + prefilter specular(split-sum)
  vec3 irradiance = evalIrradiance(n);
  vec3 iblDiffuse = irradiance * baseColor.rgb * (1.0 - metallic);
  vec3 prefiltered = textureLod(texPrefilter, r, roughness * (PREFILTER_MIPS - 1.0)).rgb;
  vec2 brdf = texture(texBrdfLut, vec2(ndv, roughness)).rg;
  vec3 Fenv = f0 * brdf.x + brdf.y;
  vec3 iblSpec = prefiltered * Fenv;
  if (specIor) iblDiffuse *= (vec3(1.0) - specWeight * Fenv);  // 介质漫反射能量扣
  if (transOn) iblDiffuse *= (1.0 - transFactor);  // 透射替换漫反射位

  // 多光源 direct(首盏方向光 + 首盏聚光投影阴影;阴影因子同施于扩展层)
  vec3 direct = vec3(0.0);
  vec3 ccDirect = vec3(0.0);  // 清漆层独立累积(分层衰减不衰减清漆自身)
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
    vec3 fres;
    vec3 spec = ggxSpec(n, L, v, roughness, f0, fres);
    vec3 diffuse = baseColor.rgb * (1.0 - metallic) / PI;
    if (specIor) diffuse *= (vec3(1.0) - specWeight * fres);
    if (transOn) diffuse *= (1.0 - transFactor);  // 透射替换漫反射位
    vec3 term = lcolor * att * ndl * (diffuse + spec);
    if (sheenOn) {  // sheen 瓣:Charlie D × Neubelt V
      vec3 h = normalize(L + v);
      float ndh = clamp(dot(n, h), 0.0, 1.0);
      term += lcolor * att * ndl * sheenColor * sheenD(sheenRough, ndh) * sheenV(ndl, ndv);
    }
    vec3 ccTerm = vec3(0.0);
    if (ccOn) {  // 清漆瓣:GGX(f0=0.04,独立法线/粗糙度)
      float ndlCc = clamp(dot(ncc, L), 0.0, 1.0);
      vec3 fresCc;
      ccTerm = lcolor * att * ndlCc * ggxSpec(ncc, L, v, ccRough, vec3(0.04), fresCc);
    }
    float shadowF = 1.0;
    if (i == 0 && type == 0) {
      // 阴影:PCF 3x3(bias 随坡度放大);采样坐标越界视为受光
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
          shadowF = sum / 9.0;
        }
      }
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
        shadowF *= sum / 9.0;
      }
    }
    direct += term * shadowF;
    ccDirect += ccTerm * shadowF;
  }

  float ao = mix(1.0, texture(sampler2D(texOcclusion, smpMat), vUV).r, iu.items[vItem].emissiveOcclusion.a);
  vec3 emissive =
      texture(sampler2D(texEmissive, smpMat), vUV).rgb * iu.items[vItem].emissiveOcclusion.rgb;

  vec3 base = iblDiffuse + iblSpec + direct;
  if (sheenOn) {  // sheen 层:基层能量扣(kSheenAlbedo 拟合)+ sheen IBL
    float scale = 1.0 - kSheenAlbedo * max(max(sheenColor.r, sheenColor.g), sheenColor.b);
    vec3 sheenIbl = textureLod(texPrefilter, r, sheenRough * (PREFILTER_MIPS - 1.0)).rgb *
                    sheenColor * kSheenAlbedo;
    base = base * scale + sheenIbl;
  }
  if (ccOn) {  // 清漆层:IBL(复用 brdfLut,f0=0.04 近似)+ 菲涅尔分层混合
    vec3 rcc = reflect(-v, ncc);
    float ndvCc = clamp(dot(ncc, v), 0.0, 1.0);
    vec3 preCc = textureLod(texPrefilter, rcc, ccRough * (PREFILTER_MIPS - 1.0)).rgb;
    vec2 brdfCc = texture(texBrdfLut, vec2(ndvCc, ccRough)).rg;
    vec3 ccIbl = preCc * (0.04 * brdfCc.x + brdfCc.y);
    float Fcc = 0.04 + 0.96 * pow(1.0 - ndvCc, 5.0);
    base = base * (1.0 - ccFactor * Fcc) + (ccDirect + ccIbl) * ccFactor;
  }
  if (transOn) {  // 透射:入射面菲涅尔权重(高光保留在 base 中)
    float fTrans = f0d.x + (1.0 - f0d.x) * pow(1.0 - ndv, 5.0);
    base += transmitted * transFactor * (1.0 - fTrans);
  }
  vec3 color = base * ao + emissive;
  if (lightCount.y > 0.5) {
    outColor = vec4(color, 1.0);  // hdrMode:线性输出,tone mapping 在 composite
  } else {
    color = color / (color + vec3(1.0));          // LDR:Reinhard(现状)
    outColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
  }
}
