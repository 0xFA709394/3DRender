# 焦散+波动方程动态水波纹 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 引擎级 Water 模块——RGBA16F ping-pong 波动方程仿真 + Jacobian 汇聚焦散 + 顶点位移水面/受水体管线;C API(`rd_engine_load_scene`/`water_disturb`)+ options + render_test `--scene water_pool` + iOS demo 接入。

**Architecture:** `core/renderer/water.{h,cpp}` 承载 `WaterSurface`(仿真/焦散资源与 pass)与 `WaterRenderable`(水面/受水体绘制项);Renderer 持有并在 endScene 场景 pass 前录仿真 pass;水参数走 FrameUBO 尾部扩展(272→336B,RHI uniform slot 仅 0..3);水面/受水体独立 combined 采样器管线族(不占 pbr 槽位,pbr uber-shader 零改动)。

**Tech Stack:** C++17 / GLSL 450(离线三后端编译)/ rd RHI(仅 fragment pass,无 compute)/ gtest / SSIM golden。

**Spec:** `docs/superpowers/specs/2026-09-06-water-caustics-design.md`

**约定(全计划通用):**
- 波场纹理 RGBA16F:R=当前高度 u,G=上一帧 u_prev;UV 域 [0,1]² = 池面俯视(池中心=局部原点,`uv = wp.xz / size + 0.5`)
- 水纹理槽:1=波场 texWave(binding 5)、2=焦散 texCaustics(binding 6)、5=prefilterCube(binding 9)、7=阴影 texShadow(binding 11);0=texBaseColor(binding 4,受水体)
- 仿真固定子步 dt=1/60,每帧至多 2 子步(CFL k=0.42,damp=0.006)
- 命令:`cmake --build build -j` 构建;`ctest --test-dir build` 测试;`./scripts/check.sh` 全量

---

### Task 1: 水系 shader ×6 + 三后端注册 + GLES 语义表

**Files:**
- Create: `shaders/water_step.frag`、`shaders/water_caustics.frag`、`shaders/water_surface.vert`、`shaders/water_surface.frag`、`shaders/water_receiver.vert`、`shaders/water_receiver.frag`
- Modify: `shaders/CMakeLists.txt`(注册编译)、`cmake/ShaderList.cmake`(注册内嵌)、`core/rhi/backends/gles/gles_device.cpp`(kBlockTable/kSamplerTable 增补)

- [ ] **Step 1: 写 water_step.frag**

```glsl
// water_step.frag:波动方程显式有限差分步进(全屏 pass,RGBA16F ping-pong)。
// R=当前高度 u,G=上一帧 u_prev → 输出 (u_new, u) 写入另一张。
// u_new = 2u - u_prev + k·(四邻均值-u) - damp·(u-u_prev) + 注入脉冲。
// 边界=clamp 寻址(池壁全反射);CFL: k≤0.5(常量 0.42)。
// 槽位:texWave=slot1(binding5);WaterStepUBO=slot0。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 0) uniform WaterStepUBO {
  vec4 inject[8];  // xy=uv z=强度(世界高度) w=半径(texel)
  vec4 params;     // x=count y=texel z=damping w=k
} ws;
layout(binding = 5) uniform sampler2D texWave;
layout(location = 0) out vec4 outColor;
void main() {
  vec2 c = texture(texWave, vUV).rg;
  float n = textureOffset(texWave, vUV, ivec2(0, 1)).r;
  float s = textureOffset(texWave, vUV, ivec2(0, -1)).r;
  float e = textureOffset(texWave, vUV, ivec2(1, 0)).r;
  float w = textureOffset(texWave, vUV, ivec2(-1, 0)).r;
  float lap = (n + s + e + w) * 0.25 - c.r;
  float u = 2.0 * c.r - c.g + ws.params.w * lap;
  u -= (c.r - c.g) * ws.params.z;  // 阻尼(速度比例)
  for (int i = 0; i < 8; ++i) {
    if (float(i) >= ws.params.x) break;
    vec2 d = (vUV - ws.inject[i].xy) / max(ws.inject[i].w * ws.params.y, 1e-6);
    u += ws.inject[i].z * exp(-dot(d, d));  // 高斯脉冲
  }
  outColor = vec4(u, c.r, 0.0, 1.0);
}
```

- [ ] **Step 2: 写 water_caustics.frag**

```glsl
// water_caustics.frag:Jacobian 汇聚近似焦散(全屏 pass,与波场同分辨率)。
// 逐 texel:高度梯度→法线→方向光折射→落点 p(uv);邻差分 2×2 Jacobian;
// 强度=clamp(1/|det J|)(汇聚亮/发散暗)。视差近似(焦散图按水面 UV 索引)。
// 槽位:texWave=slot1(binding5);WaterCausticsUBO=slot0。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 0) uniform WaterCausticsUBO {
  vec4 c0;  // x=texel y=eta(1/1.33) z=depth(焦散衰减) w=unused
  vec4 c1;  // x=worldPerTexelX y=worldPerTexelZ z=unused w=unused
  vec4 c2;  // xyz=lightDir(指向光源) w=unused
} wc;
layout(binding = 5) uniform sampler2D texWave;
layout(location = 0) out vec4 outColor;
// 折射后水平落点(uv 域;视差近似:uv + worldOffset/poolSize)
vec2 floorPos(vec2 uv) {
  float t = wc.c0.x;
  float hx = texture(texWave, uv + vec2(t, 0.0)).r - texture(texWave, uv - vec2(t, 0.0)).r;
  float hz = texture(texWave, uv + vec2(0.0, t)).r - texture(texWave, uv - vec2(0.0, t)).r;
  vec3 nrm = normalize(vec3(-hx * wc.c1.x, 2.0, -hz * wc.c1.y));  // 世界尺度梯度
  vec3 d = -normalize(wc.c2.xyz);                                  // 入射(指向下)
  vec3 refr = refract(d, nrm, wc.c0.y);
  if (dot(refr, refr) < 1e-6) refr = d;
  return uv + refr.xz * wc.c0.z / vec2(wc.c1.x, wc.c1.y) / max(wc.c0.x, 1e-6);
}
void main() {
  float t = wc.c0.x;
  vec2 p = floorPos(vUV);
  vec2 px = floorPos(vUV + vec2(t, 0.0));
  vec2 py = floorPos(vUV + vec2(0.0, t));
  vec2 jx = (px - p);  // 已含 /t·t 抵消(同尺度差分)
  vec2 jy = (py - p);
  float det = jx.x * jy.y - jx.y * jy.x;
  float detUv = det / max(t * t, 1e-8);  // uv 域行列式(平态=1)
  float i = clamp(1.0 / max(abs(detUv), 0.05), 0.0, 6.0);
  outColor = vec4(i, 0.0, 0.0, 1.0);
}
```

- [ ] **Step 3: 写 water_surface.vert**

```glsl
// water_surface.vert:水面网格顶点位移(顶点纹理 fetch×4)+ 三点差分法线。
// 世界 xz→池 uv;高度覆盖 Y(planeY+波高);mvp 不适用位移后位置 → 用 FrameUBO.viewProj。
// 槽位:texWave=slot1(binding5,顶点阶段;三后端 vert 采样已支持);
// FrameUBO=slot0(含 water[3] 尾部);ItemUBO=slot1(块声明取前 192B 对齐)。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aUV;
layout(binding = 0) uniform FrameUBO {
  mat4 viewProj;
  vec4 cameraPos;
  vec4 lightDir;
  vec4 lightColor;
  vec4 sh[9];
  vec4 transmissionParams;
  vec4 water[3];  // 0=(sizeX,sizeZ,planeY,waveScale) 1=(depth,causticsI,on,simSize) 2=(texel,0,0,0)
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
layout(binding = 5) uniform sampler2D texWave;
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
void main() {
  vec3 wp = (world * vec4(aPos, 1.0)).xyz;
  vec2 wuv = wp.xz / water[0].xy + 0.5;
  float t = water[2].x;
  float h0 = texture(texWave, wuv).r;
  float hx = texture(texWave, wuv + vec2(t, 0.0)).r - texture(texWave, wuv - vec2(t, 0.0)).r;
  float hz = texture(texWave, wuv + vec2(0.0, t)).r - texture(texWave, wuv - vec2(0.0, t)).r;
  float scale = water[0].w;
  wp.y = water[0].z + h0 * scale;
  // 世界导数:Δh·scale / (2·texel·worldPerTexel)
  float dx = hx * scale / max(2.0 * t * water[0].x, 1e-6);
  float dz = hz * scale / max(2.0 * t * water[0].y, 1e-6);
  vNormal = normalize(vec3(-dx, 1.0, -dz));
  vWorldPos = wp;
  vUV = wuv;
  gl_Position = viewProj * vec4(wp, 1.0);
}
```

- [ ] **Step 4: 写 water_surface.frag**

```glsl
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
```

- [ ] **Step 5: 写 water_receiver.vert**

```glsl
// water_receiver.vert:受水体顶点(池底/池壁/水下物体)= pbr vert 减 morph/切线。
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
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;
  vec4 uvTransform;
  vec4 ext0;
  vec4 ext1;
  vec4 ext2;
};
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
void main() {
  vWorldPos = (world * vec4(aPos, 1.0)).xyz;
  vNormal = (normalMatrix * vec4(aNormal, 0.0)).xyz;
  vUV = aUV * uvTransform.zw + uvTransform.xy;
  gl_Position = mvp * vec4(aPos, 1.0);
}
```

- [ ] **Step 6: 写 water_receiver.frag**

```glsl
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
  // 首盏方向光 + 阴影 PCF 3x3(与 pbr_forward 同款)
  vec3 direct = vec3(0.0);
  float ndl = 0.0;
  if (lu.lightCount.x > 0.5 && lu.lights[0].w < 0.5) {
    vec3 L = normalize(lu.lights[0].xyz);
    ndl = clamp(dot(n, L), 0.0, 1.0);
    // 阴影 PCF 3x3(与 pbr_forward 同款;bias 随坡度放大)
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
```

**注意:上面 water_receiver.frag 的阴影 PCF 段已是最终形态(单一双 for 真采样循环)。**

- [ ] **Step 7: 注册编译与内嵌**

`shaders/CMakeLists.txt` 末尾(第 45 行 `rd_compile_shader(shadow_depth_morph_skinned.vert)` 之后)追加:

```cmake
rd_compile_shader(water_step.frag)
rd_compile_shader(water_caustics.frag)
rd_compile_shader(water_surface.vert)
rd_compile_shader(water_surface.frag)
rd_compile_shader(water_receiver.vert)
rd_compile_shader(water_receiver.frag)
```

`cmake/ShaderList.cmake` 的 `RD_EMBED_SHADERS` 列表末尾追加 ` water_step water_caustics water_surface water_receiver`(一行内空格分隔)。

- [ ] **Step 8: GLES 语义表增补**

`core/rhi/backends/gles/gles_device.cpp` 中 `kBlockTable`(约 819 行)追加两项:

```cpp
      {"LightUBO", 2}, {"JointUBO", 3},
      {"WaterStepUBO", 0}, {"WaterCausticsUBO", 0},
```

`kSamplerTable`(约 833 行)追加:

```cpp
      {"texWave", 1}, {"texCaustics", 2},
```

- [ ] **Step 9: 构建 + 现有测试零回归**

Run: `cmake --build build -j 2>&1 | tail -5 && ctest --test-dir build -R "Embedded|Shader" --output-on-failure`
Expected: 构建成功(6 个 shader 出现在 RD_SHADER_OUT);embedded 相关测试 PASS。

Run: `ctest --test-dir build -E "perf|Interactive" --output-on-failure | tail -3`
Expected: 全 PASS(仅改 GLES 表,host 不受影响)。

- [ ] **Step 10: Commit**

```bash
git add shaders/ cmake/ShaderList.cmake core/rhi/backends/gles/gles_device.cpp
git commit -m "feat(shaders): 水系 shader ×6(波动方程 step/Jacobian 焦散/水面位移/受水体)+ 三后端注册 + GLES 语义表"
```

---

### Task 2: makeGrid 几何 + WaterSurface 仿真模块

**Files:**
- Create: `core/renderer/water.h`、`core/renderer/water.cpp`、`tests/renderer/water_surface_test.cpp`
- Modify: `core/resource/primitives.h`、`core/resource/primitives.cpp`、`core/CMakeLists.txt`、`tests/CMakeLists.txt`、`tests/resource/primitives_test.cpp`

- [ ] **Step 1: 写 makeGrid 失败测试**

`tests/resource/primitives_test.cpp` 追加:

```cpp
TEST(Primitives, MakeGrid) {
  auto m = rd::primitives::makeGrid(4.0f, 8);
  ASSERT_FALSE(m.vertices.empty());
  ASSERT_FALSE(m.indices.empty());
  // 顶点数 =(8+1)²,索引 = 8²×6
  ASSERT_EQ(m.vertices.size() / 12, 81u);
  ASSERT_EQ(m.indices.size(), 8u * 8u * 6u);
  // 四角坐标:±2(y=0),UV [0,1]
  const float* p0 = m.vertices.data();                       // 首 vertex
  const float* pN = m.vertices.data() + (81 - 1) * 12;       // 末 vertex
  EXPECT_NEAR(p0[0], -2.0f, 1e-5);
  EXPECT_NEAR(p0[2], -2.0f, 1e-5);
  EXPECT_NEAR(p0[1], 0.0f, 1e-6);
  EXPECT_NEAR(pN[0], 2.0f, 1e-5);
  EXPECT_NEAR(pN[2], 2.0f, 1e-5);
  // 法线朝上
  EXPECT_NEAR(p0[4], 1.0f, 1e-6);
}
```

Run: `cmake --build build -j --target rd_tests 2>&1 | tail -3 && ctest --test-dir build -R Primitives 2>&1 | tail -3`
Expected: 编译失败(`makeGrid` 未定义)——即为红灯。

- [ ] **Step 2: 实现 makeGrid**

`core/resource/primitives.h` `makePlane` 声明后追加:

```cpp
/// 细分平面网格:y 朝上,中心原点,边长 size,每边 segments 段(≥1);
/// 顶点 (segments+1)²,水面顶点位移用(48B 布局,法线全 +Y,uv=[0,1]²)。
MeshData makeGrid(float size, uint32_t segments);
```

`core/resource/primitives.cpp` 末尾(namespace 内)追加:

```cpp
MeshData makeGrid(float size, uint32_t segments) {
  segments = std::max(segments, 1u);
  MeshData m;
  const float half = size * 0.5f;
  const uint32_t n = segments + 1;
  m.vertices.reserve(size_t(n) * n * 12);
  m.indices.reserve(size_t(segments) * segments * 6);
  for (uint32_t j = 0; j < n; ++j)
    for (uint32_t i = 0; i < n; ++i) {
      const float u = float(i) / float(segments), v = float(j) / float(segments);
      const float x = -half + u * size, z = -half + v * size;
      const float vs[12] = {x, 0.0f, z, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, u, v};
      m.vertices.insert(m.vertices.end(), vs, vs + 12);
    }
  for (uint32_t j = 0; j < segments; ++j)
    for (uint32_t i = 0; i < segments; ++i) {
      const uint32_t a = j * n + i, b = a + 1, c = a + n, d = c + 1;
      const uint32_t idx[6] = {a, c, b, b, c, d};
      m.indices.insert(m.indices.end(), idx, idx + 6);
    }
  return m;
}
```

Run: `cmake --build build -j --target rd_tests && ctest --test-dir build -R Primitives --output-on-failure | tail -2`
Expected: PASS。

- [ ] **Step 3: 写 water.h(WaterSurface 完整接口)**

```cpp
/**
 * @file water.h
 * @brief 波动方程水面:RGBA16F ping-pong 仿真 + Jacobian 焦散 + WaterRenderable。
 * 仿真:固定 1/60 子步(每帧至多 2),注入列表 UBO(高斯脉冲),clamp 边界=池壁反射。
 */
#pragma once
#include "foundation/math.h"
#include "renderer/mesh_renderable.h"
#include "rhi/rhi_device.h"
#include <memory>
#include <string>
#include <vector>

namespace rd {

/// 水面几何与仿真描述。
struct WaterDesc {
  float planeY = 0.0f;      ///< 水面世界高度
  float sizeX = 4.0f;       ///< 池 X 边长(世界单位;UV 域 [0,1]²)
  float sizeZ = 4.0f;       ///< 池 Z 边长
  uint32_t simSize = 256;   ///< 仿真/焦散纹理边长(画质档)
  bool caustics = true;     ///< 焦散 pass(Low 档关)
};

/// 可调参数(选项映射;waveScale=0 零操作)。
struct WaterParams {
  float waveScale = 1.0f;
  float causticsIntensity = 1.0f;
  float depth = 1.0f;
};

class WaterSurface {
public:
  /// 创建 ping-pong/焦散纹理 + step/caustics 管线(全屏 pass,RGBA16F)。
  /// 失败返回 false(资源已清理)。GLES 无 hdr_render_target caps 时失败。
  bool create(Device& dev, const WaterDesc& desc, const std::vector<uint8_t>& blitVsCode,
              const std::vector<uint8_t>& stepFs, const std::vector<uint8_t>& causticsFs,
              const std::string& entry);
  void destroy(Device& dev);
  bool valid() const { return tex_[0].valid() && target_[0].valid(); }

  void setParams(const WaterParams& p) { params_ = p; }
  const WaterParams& params() const { return params_; }
  const WaterDesc& desc() const { return desc_; }
  /// 方向光方向(焦散 pass 用;Renderer 每 step 前设置,指向光源)。
  void setLightDir(float x, float y, float z) { lightDir_[0] = x; lightDir_[1] = y; lightDir_[2] = z; }

  /// 注入涟漪(uv∈[0,1]²;strength 世界高度;radius texel)。帧内累积,下步消费;满 8 丢弃。
  void disturb(float u, float v, float strength, float radius);
  /// 帧时间累计(render_frame 传入;固定 1/60 子步,帧间确定性)。
  void tick(float dt) { simTime_ += dt; }
  /// 仿真步进 + 焦散 pass(endScene 场景 pass 前调用)。
  void step(CommandBuffer* cmd);

  TextureHandle waveTex() const { return tex_[cur_]; }
  TextureHandle causticsTex() const { return causticsOn() ? causticsTex_ : causticsFallbackTex_; }
  SamplerHandle sampler() const { return sampler_; }
  bool causticsOn() const { return desc_.caustics && causticsTex_.valid(); }

private:
  Device* dev_ = nullptr;
  WaterDesc desc_{};
  WaterParams params_{};
  float lightDir_[3] = {0.3f, 1.0f, 0.45f};
  TextureHandle tex_[2];
  TargetHandle target_[2];
  TextureHandle causticsTex_, causticsFallbackTex_;
  TargetHandle causticsTarget_;
  SamplerHandle sampler_;
  PipelineHandle stepPipeline_, causticsPipeline_;
  BufferHandle stepUbo_, causticsUbo_;
  float inject_[8][4] = {};
  uint32_t injectCount_ = 0;
  float simTime_ = 0.0f, stepped_ = 0.0f;
  uint32_t cur_ = 0;
};

/// 水渲染项:Surface=水面(blend 半透明,不投影);Receiver=受水体(opaque,常规深度)。
/// res_ 直持资源(MeshRenderable 的 mesh_ 为 private,子类经自有副本访问)。
class WaterRenderable : public MeshRenderable {
public:
  enum class Mode { Surface, Receiver };
  WaterRenderable(std::shared_ptr<MeshRenderResource> mesh, Mode mode)
      : MeshRenderable(std::move(mesh)), res_(mesh), mode_(mode) {}
  void record(CommandBuffer* cmd, const RenderContext& ctx) override;
  bool waterItem() const override { return true; }
  Mode mode() const { return mode_; }
private:
  std::shared_ptr<MeshRenderResource> res_;
  Mode mode_;
};

} // namespace rd
```

- [ ] **Step 4: 写 water.cpp(WaterSurface 实现)**

```cpp
// WaterSurface:波动方程 ping-pong 步进 + Jacobian 焦散 pass + WaterRenderable 录制。
#include "renderer/water.h"
#include "renderer/environment.h"
#include "renderer/renderable.h"
#include "foundation/log.h"

namespace rd {

namespace {
constexpr float kStepDt = 1.0f / 60.0f;   // 固定子步(确定性)
constexpr float kCflK = 0.42f;            // c²dt²/dx² ≤ 0.5(CFL)
constexpr float kDamping = 0.006f;
} // namespace

bool WaterSurface::create(Device& dev, const WaterDesc& desc,
                          const std::vector<uint8_t>& blitVsCode,
                          const std::vector<uint8_t>& stepFs,
                          const std::vector<uint8_t>& causticsFs,
                          const std::string& entry) {
  if (!dev.caps().supports(Capability::hdr_render_target)) {
    RD_LOGW("renderer.water", "后端无 half-float 渲染目标 caps,水面不可用");
    return false;
  }
  dev_ = &dev;
  desc_ = desc;
  const uint32_t n = std::max(desc_.simSize, 16u);
  desc_.simSize = n;
  // ping-pong 双纹理(texture-backed 目标)
  for (int i = 0; i < 2; ++i) {
    TextureDesc td;
    td.width = n; td.height = n;
    td.format = Format::R16G16B16A16_FLOAT;
    td.usage = TextureUsage::Sampled | TextureUsage::RenderTargetAttachment;
    tex_[i] = dev.createTexture(td);
    if (!tex_[i].valid()) { destroy(dev); return false; }
    OffscreenTargetDesc od;
    od.width = n; od.height = n;
    od.colorFromTexture = tex_[i];
    target_[i] = dev.createOffscreenTarget(od);
    if (!target_[i].valid()) { destroy(dev); return false; }
  }
  if (desc_.caustics) {
    TextureDesc cd = []() { TextureDesc t; t.format = Format::R16G16B16A16_FLOAT;
      t.usage = TextureUsage::Sampled | TextureUsage::RenderTargetAttachment; return t; }();
    cd.width = n; cd.height = n;
    causticsTex_ = dev.createTexture(cd);
    OffscreenTargetDesc cod;
    cod.width = n; cod.height = n;
    cod.colorFromTexture = causticsTex_;
    causticsTarget_ = dev.createOffscreenTarget(cod);
    if (!causticsTex_.valid() || !causticsTarget_.valid()) { destroy(dev); return false; }
  }
  {  // 1×1 黑焦散占位(Low 档/关闭)
    TextureDesc fd;
    fd.width = 1; fd.height = 1;
    fd.format = Format::RGBA8_UNORM;
    fd.usage = TextureUsage::Sampled;
    const uint8_t black[4] = {0, 0, 0, 255};
    fd.data = black; fd.dataSize = 4;
    causticsFallbackTex_ = dev.createTexture(fd);
  }
  SamplerDesc sd;
  sd.wrapU = WrapMode::Clamp;
  sd.wrapV = WrapMode::Clamp;
  sampler_ = dev.createSampler(sd);
  // step 管线(blit vert + water_step.frag,RGBA16F)
  auto mkPass = [&](const std::vector<uint8_t>& fsCode, PipelineHandle& pipe) {
    auto vs = dev.createShaderModule({ShaderStage::Vertex, blitVsCode, entry});
    auto fs = dev.createShaderModule({ShaderStage::Fragment, fsCode, entry});
    PipelineDesc pd;
    pd.vertexShader = vs; pd.fragmentShader = fs;
    pd.cullMode = CullMode::None;
    pd.colorFormat = Format::R16G16B16A16_FLOAT;
    pipe = dev.createPipeline(pd);
    dev.destroyShaderModule(vs);
    dev.destroyShaderModule(fs);
    return pipe.valid();
  };
  if (!mkPass(stepFs, stepPipeline_) ||
      (desc_.caustics && !mkPass(causticsFs, causticsPipeline_))) {
    destroy(dev);
    return false;
  }
  stepUbo_ = dev.createBuffer({144, BufferUsage::Uniform, true, false, nullptr});
  causticsUbo_ = dev.createBuffer({48, BufferUsage::Uniform, true, false, nullptr});
  if (!stepUbo_.valid() || !causticsUbo_.valid()) { destroy(dev); return false; }
  // 初始清零 ping-pong(未定义内容禁止;全屏 clear 一次)
  auto* c = dev.acquireCommandBuffer();
  for (int i = 0; i < 2; ++i) {
    c->beginRenderPass(target_[i], {0, 0, 0, 0, 1.0f});
    c->endRenderPass();
  }
  if (causticsTarget_.valid()) {
    c->beginRenderPass(causticsTarget_, {0, 0, 0, 0, 1.0f});
    c->endRenderPass();
  }
  dev.submit(c);
  dev.waitIdle();
  return true;
}

void WaterSurface::destroy(Device& dev) {
  for (int i = 0; i < 2; ++i) {
    if (target_[i].valid()) dev.destroyTarget(target_[i]);
    if (tex_[i].valid()) dev.destroyTexture(tex_[i]);
    target_[i] = {};
    tex_[i] = {};
  }
  if (causticsTarget_.valid()) dev.destroyTarget(causticsTarget_);
  if (causticsTex_.valid()) dev.destroyTexture(causticsTex_);
  if (causticsFallbackTex_.valid()) dev.destroyTexture(causticsFallbackTex_);
  if (stepPipeline_.valid()) dev.destroyPipeline(stepPipeline_);
  if (causticsPipeline_.valid()) dev.destroyPipeline(causticsPipeline_);
  if (sampler_.valid()) dev.destroySampler(sampler_);
  if (stepUbo_.valid()) dev.destroyBuffer(stepUbo_);
  if (causticsUbo_.valid()) dev.destroyBuffer(causticsUbo_);
  causticsTarget_ = {};
  causticsTex_ = {};
  causticsFallbackTex_ = {};
  stepPipeline_ = {};
  causticsPipeline_ = {};
  sampler_ = {};
  stepUbo_ = {};
  causticsUbo_ = {};
  injectCount_ = 0;
  simTime_ = stepped_ = 0.0f;
  cur_ = 0;
  dev_ = nullptr;
}

void WaterSurface::disturb(float u, float v, float strength, float radius) {
  if (injectCount_ >= 8) return;  // 满 8 丢弃
  inject_[injectCount_][0] = u;
  inject_[injectCount_][1] = v;
  inject_[injectCount_][2] = strength;
  inject_[injectCount_][3] = radius;
  ++injectCount_;
}

void WaterSurface::step(CommandBuffer* cmd) {
  if (!valid()) return;
  // 子步数:累计时间折算,每帧至多 2(120Hz 屏不加速;低帧率至多欠步)
  uint32_t steps = 0;
  while (simTime_ - stepped_ >= kStepDt && steps < 2) {
    stepped_ += kStepDt;
    ++steps;
  }
  if (simTime_ > stepped_ + 4.0f * kStepDt) {  // 长期挂起后重置(防追帧雪崩)
    stepped_ = simTime_;
    steps = 0;
  }
  const float texel = 1.0f / float(desc_.simSize);
  for (uint32_t s = 0; s < steps; ++s) {
    // 注入 UBO(仅首个子步消费;消费后清零)
    struct {
      float inject[8][4];
      float params[4];
    } su{};
    memcpy(su.inject, inject_, sizeof(inject_));
    su.params[0] = float(injectCount_);
    su.params[1] = texel;
    su.params[2] = kDamping;
    su.params[3] = kCflK;
    dev_->updateBuffer(stepUbo_, &su, sizeof(su), 0);
    injectCount_ = 0;
    cmd->beginRenderPass(target_[cur_ ^ 1], {0, 0, 0, 0, 1.0f});
    cmd->bindPipeline(stepPipeline_);
    cmd->bindUniformBuffer(0, stepUbo_, 0, sizeof(su));
    cmd->bindTexture(1, tex_[cur_], sampler_);
    cmd->draw(3, 0);
    cmd->endRenderPass();
    cur_ ^= 1;
  }
  if (causticsOn()) {
    struct {
      float c0[4];  // texel/eta/depth/intensity 占位
      float c1[4];  // worldPerTexel
      float c2[4];  // lightDir
    } cu{};
    cu.c0[0] = texel;
    cu.c0[1] = 1.0f / 1.33f;
    cu.c0[2] = params_.depth;
    cu.c0[3] = params_.causticsIntensity;
    cu.c1[0] = desc_.sizeX * texel;
    cu.c1[1] = desc_.sizeZ * texel;
    cu.c2[0] = lightDir_[0];
    cu.c2[1] = lightDir_[1];
    cu.c2[2] = lightDir_[2];
    dev_->updateBuffer(causticsUbo_, &cu, sizeof(cu), 0);
    cmd->beginRenderPass(causticsTarget_, {0, 0, 0, 0, 1.0f});
    cmd->bindPipeline(causticsPipeline_);
    cmd->bindUniformBuffer(0, causticsUbo_, 0, sizeof(cu));
    cmd->bindTexture(1, tex_[cur_], sampler_);
    cmd->draw(3, 0);
    cmd->endRenderPass();
  }
}

void WaterRenderable::record(CommandBuffer* cmd, const RenderContext& ctx) {
  if (ctx.shadowPass) {
    if (mode_ == Mode::Surface) return;  // 水面不投影
    MeshRenderable::record(cmd, ctx);    // 受水体:常规深度写出
    return;
  }
  const uint64_t kLastSlot = uint64_t(kItemUboMaxSlots - 1) * kItemUboStride;
  uint32_t meshIdx = 0;
  for (const auto& g : res_->meshes()) {
    const uint64_t off = ctx.itemOffset + uint64_t(meshIdx) * kItemUboStride;
    const uint64_t itemOff = off < kLastSlot ? off : kLastSlot;
    if (mode_ == Mode::Surface) {
      cmd->bindPipeline(ctx.waterSurfacePipeline);
      cmd->bindUniformBuffer(0, ctx.frameUbo, 0, 336);
      cmd->bindUniformBuffer(1, ctx.itemUbo, itemOff, kItemUboSize);
      cmd->bindUniformBuffer(2, ctx.lightUbo, 0, 432);
      if (ctx.waterWave.valid()) cmd->bindTexture(1, ctx.waterWave, ctx.waterSampler);
      if (ctx.env && ctx.env->prefilterCube().valid())
        cmd->bindTexture(5, ctx.env->prefilterCube(), ctx.env->cubeSampler());
    } else {
      cmd->bindPipeline(ctx.waterReceiverPipeline);
      cmd->bindUniformBuffer(0, ctx.frameUbo, 0, 336);
      cmd->bindUniformBuffer(1, ctx.itemUbo, itemOff, kItemUboSize);
      cmd->bindUniformBuffer(2, ctx.lightUbo, 0, 432);
      cmd->bindTexture(0, g.baseColorTex, res_->sampler());
      if (ctx.waterCaustics.valid()) cmd->bindTexture(2, ctx.waterCaustics, ctx.waterSampler);
      if (ctx.shadowMap.valid()) cmd->bindTexture(7, ctx.shadowMap, ctx.shadowSampler);
    }
    cmd->bindVertexBuffer(0, g.vbo, 0);
    cmd->bindIndexBuffer(g.ibo, 0, g.indexType);
    cmd->drawIndexed(g.indexCount, 0, 0);
    ++meshIdx;
  }
}

} // namespace rd
```

(`RenderContext` 的 water 字段与 `waterItem()` 虚函数在 Task 3 加入——Task 2 编译时 `water.cpp` 引用了它们,故 **Task 2 与 Task 3 的 Step 1(基类/上下文改造)须同批提交前完成**;或把 Task 3 Step 1 提前并入 Task 2 执行。推荐后者:执行 Task 2 时先做 Task 3 Step 1。)

- [ ] **Step 5: 接入构建**

`core/CMakeLists.txt` `add_library(rd_core STATIC ...)` 的 `renderer/environment.cpp` 行后加:

```cmake
  renderer/water.cpp
```

`tests/CMakeLists.txt` renderer 源列表(`renderer/demo_scenes_test.cpp` 后)加:

```
  renderer/water_surface_test.cpp
```

- [ ] **Step 6: 写 WaterSurface 冒烟测试**

`tests/renderer/water_surface_test.cpp`:

```cpp
// WaterSurface 创建/注入/步进冒烟(不渲染最终图像——语义判据在 Task 3 集成后)。
#include <gtest/gtest.h>
#include "common/shader_code.h"
#include "renderer/water.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

static rd::WaterSurface* makeWater(rd::Device& dev) {
  auto blitVs = rd::test::loadShaderCode(dev.backend(), RD_SHADER_DIR, "blit.vert");
  auto stepFs = rd::test::loadShaderCode(dev.backend(), RD_SHADER_DIR, "water_step.frag");
  auto cauFs = rd::test::loadShaderCode(dev.backend(), RD_SHADER_DIR, "water_caustics.frag");
  auto* w = new rd::WaterSurface();
  rd::WaterDesc d;
  d.simSize = 64;
  if (!w->create(dev, d, blitVs.code, stepFs.code, cauFs.code, blitVs.entry)) {
    delete w;
    return nullptr;
  }
  return w;
}

TEST(WaterSurface, CreateStepSmoke) {
#if defined(__APPLE__)
  rd::DeviceDesc dd;
  dd.backend = rd::Backend::Metal;
  auto dev = rd::createDevice(dd);
  ASSERT_TRUE(dev);
  auto* w = makeWater(*dev);
  ASSERT_NE(w, nullptr);
  EXPECT_TRUE(w->waveTex().valid());
  EXPECT_TRUE(w->causticsTex().valid());
  w->disturb(0.5f, 0.5f, 0.05f, 3.0f);
  w->tick(1.0f / 60.0f);
  dev->beginFrame();
  auto* cmd = dev->acquireCommandBuffer();
  w->step(cmd);
  dev->submit(cmd);
  dev->waitIdle();
  dev->endFrame();
  w->destroy(*dev);
  delete w;
#endif
}
```

Run: `cmake --build build -j --target rd_tests && ctest --test-dir build -R WaterSurface --output-on-failure | tail -2`
Expected: PASS。

- [ ] **Step 7: Commit**

```bash
git add core/renderer/water.h core/renderer/water.cpp core/resource/primitives.{h,cpp} \
        core/CMakeLists.txt tests/CMakeLists.txt tests/renderer/water_surface_test.cpp \
        tests/resource/primitives_test.cpp
git commit -m "feat(renderer): WaterSurface 波动方程仿真/焦散模块 + makeGrid 细分平面 + 冒烟"
```

---

### Task 3: Renderer 集成(FrameUBO 336 + 管线 + endScene 接线)+ 语义测试

**Files:**
- Modify: `core/renderer/renderable.h`(RenderContext 字段)、`core/renderer/mesh_renderable.{h,cpp}`(waterItem 虚 + protected 访问 + FrameUBO 336)、`core/renderer/renderer.{h,cpp}`、`tests/renderer/water_surface_test.cpp`(追加语义测试)

- [ ] **Step 1: RenderContext 字段 + MeshRenderable::waterItem 虚函数**

(本 Step 若提前到 Task 2 执行,顺序更顺——water.cpp 编译依赖。)

`core/renderer/renderable.h` `RenderContext` 末尾(`transSampler` 之后)追加:

```cpp
  /// Water(Renderer 注入;waterSurfacePipeline/waterReceiverPipeline 随
  /// SceneTarget 格式重建;无效=WaterRenderable 跳过绘制)
  PipelineHandle waterSurfacePipeline;
  PipelineHandle waterReceiverPipeline;
  TextureHandle waterWave;         ///< slot1:当前高度场
  TextureHandle waterCaustics;     ///< slot2:焦散(Low 档=1×1 黑占位)
  SamplerHandle waterSampler;
```

`core/renderer/mesh_renderable.h` public 区(`meshSampler()` 后)追加:

```cpp
  /// 水渲染项标记(实例化分组/视锥剔除排除用;WaterRenderable 覆写为 true)。
  virtual bool waterItem() const { return false; }
```

- [ ] **Step 2: RendererShaderDesc + renderer.h 成员/方法**

`core/renderer/renderer.h`:

(a) `RendererShaderDesc` **末尾**(`colorFormat` 之后)追加(位置必须在最后——现有调用点全是位置式聚合初始化):

```cpp
  std::vector<uint8_t> waterStepFs, waterCausticsFs;      // 全屏 pass(空=无水)
  std::vector<uint8_t> waterSurfaceVs, waterSurfaceFs;    // 水面(空=无水)
  std::vector<uint8_t> waterReceiverVs, waterReceiverFs;  // 受水体(空=无水)
```

(b) `Renderer` public 区(`setTransmissionEnabled` 后)追加:

```cpp
  /// ---- Water(波动方程水面;默认未激活)----
  /// 激活水面系统(画质档 simSize/caustics 由 setQuality 联动重建;
  /// shader 未随 init 提供或 caps 缺失返回 false)。
  bool enableWater(const WaterDesc& desc);
  void disableWater();
  bool waterActive() const { return water_ && water_->valid(); }
  void setWaterParams(const WaterParams& p);
  /// 注入涟漪(uv∈[0,1]² 池面俯视域)。
  void disturbWater(float u, float v, float strength, float radius);
  /// 帧时间累计(render_frame/submitDemoScene 传入;固定子步)。
  void tick(float dt) { if (water_) water_->tick(dt); }
  void submitWaterSurface(const std::shared_ptr<MeshRenderResource>& mesh,
                          const math::Mat4& world);
  void submitWaterReceiver(const std::shared_ptr<MeshRenderResource>& mesh,
                           const math::Mat4& world);
```

(c) private 成员区(transmission 块后)追加:

```cpp
  // ---- Water(波动方程水面)----
  std::unique_ptr<WaterSurface> water_;
  WaterDesc waterDesc_{};             ///< 最近一次 enableWater 描述(setQuality 重建用)
  WaterParams waterParams_{};
  uint32_t waterSimSize_ = 0;         ///< 画质档联动(0=未设置)
  uint32_t waterCaustics_ = 1;
  std::vector<uint8_t> waterStepFs_, waterCausticsFs_, waterSvCode_, waterSfCode_,
      waterRvCode_, waterRfCode_;     ///< enableWater 暂存(init 期拷贝)
  ShaderModuleHandle waterSvs_, waterSfs_, waterRvs_, waterRfs_;
  PipelineHandle waterSurfacePipeline_, waterReceiverPipeline_;
  /// submit 共用尾部(queue/world/joint/meshCount 登记与容量检查)。
  void pushWaterItem(std::unique_ptr<WaterRenderable> r,
                     const std::shared_ptr<MeshRenderResource>& mesh,
                     const math::Mat4& world);
```

头文件 include 区加 `#include "renderer/water.h"`。

- [ ] **Step 3: renderer.cpp——init/setQuality/shutdown/beginScene/submit/endScene 接线**

(a) `init` 内(`transSampler_` 创建块之后、PostChain 之前)追加:

```cpp
  // Water shader 模块与产物暂存(空码=不支持;enableWater 时用)
  waterStepFs_ = desc.waterStepFs;
  waterCausticsFs_ = desc.waterCausticsFs;
  waterSvCode_ = desc.waterSurfaceVs;
  waterSfCode_ = desc.waterSurfaceFs;
  waterRvCode_ = desc.waterReceiverVs;
  waterRfCode_ = desc.waterReceiverFs;
  if (!waterSvCode_.empty() && !waterSfCode_.empty()) {
    waterSvs_ = dev.createShaderModule({ShaderStage::Vertex, waterSvCode_, desc.entry});
    waterSfs_ = dev.createShaderModule({ShaderStage::Fragment, waterSfCode_, desc.entry});
  }
  if (!waterRvCode_.empty() && !waterRfCode_.empty()) {
    waterRvs_ = dev.createShaderModule({ShaderStage::Vertex, waterRvCode_, desc.entry});
    waterRfs_ = dev.createShaderModule({ShaderStage::Fragment, waterRfCode_, desc.entry});
  }
```

(b) `shutdown` 内(`transSampler_` 释放块后)追加:

```cpp
  if (water_) water_->destroy(*dev_);
  water_.reset();
  if (waterSurfacePipeline_.valid()) dev_->destroyPipeline(waterSurfacePipeline_);
  if (waterReceiverPipeline_.valid()) dev_->destroyPipeline(waterReceiverPipeline_);
  for (ShaderModuleHandle m : {waterSvs_, waterSfs_, waterRvs_, waterRfs_})
    if (m.valid()) dev_->destroyShaderModule(m);
  waterSurfacePipeline_ = {};
  waterReceiverPipeline_ = {};
  waterSvs_ = waterSfs_ = waterRvs_ = waterRfs_ = {};
```

(c) `setQuality` 末尾追加(画质档联动重建):

```cpp
  const uint32_t wantSim = q.waterSimSize ? q.waterSimSize : 128;
  const uint32_t wantCaustics = q.waterCaustics ? 1u : 0u;
  if (water_ && water_->valid() &&
      (wantSim != waterSimSize_ || wantCaustics != waterCaustics_)) {
    WaterDesc d = waterDesc_;
    d.simSize = wantSim;
    d.caustics = wantCaustics != 0;
    WaterParams p = water_->params();
    disableWater();
    if (enableWater(d)) water_->setParams(p);
  }
  waterSimSize_ = wantSim;
  waterCaustics_ = wantCaustics;
```

(d) `beginScene` FrameUBO 结构体扩 336B:局部 `fu` 结构体加一组成员 + static_assert 改:

```cpp
  struct {
    math::Mat4 viewProj;
    math::Vec4 cameraPos;
    math::Vec4 lightDir;
    math::Vec4 lightColor;
    float sh[9][4];
    float transmissionParams[4];
    float waterParams[3][4];  // 水尾部:0=(sizeX,sizeZ,planeY,waveScale)
                              // 1=(depth,causticsI,on,simSize) 2=(texel,0,0,0)
  } fu;
  static_assert(sizeof(fu) == 336, "FrameUBO 必须 336B(272+water 48B)");
```

填充(在 `fu.transmissionParams` 清零块后):

```cpp
  for (int i = 0; i < 3; ++i)
    fu.waterParams[i][0] = fu.waterParams[i][1] = fu.waterParams[i][2] =
        fu.waterParams[i][3] = 0.0f;
  if (water_ && water_->valid()) {
    const auto& d = water_->desc();
    const auto& p = water_->params();
    fu.waterParams[0][0] = d.sizeX;
    fu.waterParams[0][1] = d.sizeZ;
    fu.waterParams[0][2] = d.planeY;
    fu.waterParams[0][3] = p.waveScale;
    fu.waterParams[1][0] = p.depth;
    fu.waterParams[1][1] = p.causticsIntensity;
    fu.waterParams[1][2] = water_->causticsOn() ? 1.0f : 0.0f;
    fu.waterParams[1][3] = float(d.simSize);
    fu.waterParams[2][0] = 1.0f / float(d.simSize);
  }
```

`frameUbo_` 创建尺寸 272→336(`init` 中 `dev.createBuffer({272, ...})` 改 `{336, ...}`)。

**FrameUBO 绑定点 272→336 全量替换**(4 处):`renderer.cpp` 实例化路径 `bindUniformBuffer(0, frameUbo_, 0, 272)`;`mesh_renderable.cpp` 三处 `bindUniformBuffer(0, ctx.frameUbo, 0, 272)`。

(e) water 公有方法实现(文件尾部 transmission 函数后):

```cpp
bool Renderer::enableWater(const WaterDesc& desc) {
  if (!dev_) return false;
  if (waterStepFs_.empty() || waterCausticsFs_.empty() || waterSvCode_.empty() ||
      waterSfCode_.empty() || waterRvCode_.empty() || waterRfCode_.empty()) {
    RD_LOGW("renderer", "water shader 未随 init 提供,水面不可用");
    return false;
  }
  disableWater();
  water_ = std::make_unique<WaterSurface>();
  if (!water_->create(*dev_, desc, blitVsCode_, waterStepFs_, waterCausticsFs_, entry_)) {
    water_.reset();
    RD_LOGW("renderer", "WaterSurface 创建失败");
    return false;
  }
  waterDesc_ = desc;
  water_->setParams(waterParams_);
  // 画质档当前值即刻生效(setQuality 已记录)
  if (waterSimSize_) {
    if (waterSimSize_ != desc.simSize || waterCaustics_ != (desc.caustics ? 1u : 0u)) {
      WaterDesc d = waterDesc_;
      d.simSize = waterSimSize_;
      d.caustics = waterCaustics_ != 0;
      WaterParams p = waterParams_;
      disableWater();
      water_ = std::make_unique<WaterSurface>();
      if (water_->create(*dev_, d, blitVsCode_, waterStepFs_, waterCausticsFs_, entry_)) {
        waterDesc_ = d;
        water_->setParams(p);
      } else {
        water_.reset();
        return false;
      }
    }
  }
  pipeSamples_ = 0;  // 强制下帧 ensureScenePipelines 重建 water 管线
  return true;
}

void Renderer::disableWater() {
  if (water_) water_->destroy(*dev_);
  water_.reset();
}

void Renderer::setWaterParams(const WaterParams& p) {
  waterParams_ = p;
  if (water_) water_->setParams(p);
}

void Renderer::disturbWater(float u, float v, float strength, float radius) {
  if (water_) water_->disturb(u, v, strength, radius);
}

void Renderer::pushWaterItem(std::unique_ptr<WaterRenderable> r,
                             const std::shared_ptr<MeshRenderResource>& mesh,
                             const math::Mat4& world) {
  if (queue_.size() >= kMaxItems) {
    RD_LOGW("renderer", "渲染项超出 %u,截断", kMaxItems);
    return;
  }
  queue_.push_back(std::move(r));
  worldStack_.push_back(world);
  jointSlot_.push_back(-1);
  morphOverride_.emplace_back();
  const auto& md = mesh ? mesh->meshes() : std::vector<MeshGpuData>();
  meshCount_.push_back(uint32_t(std::max<size_t>(1, md.size())));
}

void Renderer::submitWaterSurface(const std::shared_ptr<MeshRenderResource>& mesh,
                                  const math::Mat4& world) {
  if (!waterActive()) return;
  pushWaterItem(std::make_unique<WaterRenderable>(mesh, WaterRenderable::Mode::Surface),
                mesh, world);
}

void Renderer::submitWaterReceiver(const std::shared_ptr<MeshRenderResource>& mesh,
                                   const math::Mat4& world) {
  if (!waterActive()) return;
  pushWaterItem(std::make_unique<WaterRenderable>(mesh, WaterRenderable::Mode::Receiver),
                mesh, world);
}
```

(f) `endScene` 三处接线:

1. 聚光 ShadowPass 块之后、`// 上屏链` 注释之前插 water step:

```cpp
  // ---- Water 仿真步进 + 焦散 pass(场景 pass 前;方向光喂焦散)----
  if (water_ && water_->valid()) {
    if (dirLight) water_->setLightDir(dirLight->direction[0], dirLight->direction[1],
                                      dirLight->direction[2]);
    else water_->setLightDir(-0.5f, 0.8f, 0.3f);
    water_->step(cmd);
  }
```

2. 场景 pass `RenderContext ctx` 填充块末尾追加:

```cpp
  ctx.waterSurfacePipeline = waterSurfacePipeline_;
  ctx.waterReceiverPipeline = waterReceiverPipeline_;
  if (water_ && water_->valid()) {
    ctx.waterWave = water_->waveTex();
    ctx.waterCaustics = water_->causticsTex();
    ctx.waterSampler = water_->sampler();
  }
```

3. 视锥剔除 `culledBy` 的 `dynamic0` 条件追加 water 排除:

```cpp
    const bool dynamic0 =
        jointSlot_[idx] >= 0 || r0->waterItem() ||
        (!r0->meshData().empty() && r0->meshData()[0].morph);  // 蒙皮/morph/水 不剔除
```

4. 实例化分组排除(场景 pass `rid` 条件与阴影 pass `rid` 条件各加一项):

场景:`!r->meshData().empty() && !r->meshData()[0].skinned && !r->waterItem() && ...`
阴影:`r->meshData().size() == 1 && jointSlot_[idx] < 0 && !r->waterItem() && ...`

(g) `ensureScenePipelines` 末尾(`pipeFmt_ = fmt;` 之前)追加:

```cpp
  // ---- Water 管线(独立 combined 族;Surface=blend+depthTest 关写,Receiver=opaque)----
  if (waterSvs_.valid() && waterSfs_.valid()) {
    PipelineDesc wpd;
    wpd.vertexShader = waterSvs_;
    wpd.fragmentShader = waterSfs_;
    fillVertexLayout(wpd);
    wpd.cullMode = CullMode::None;
    wpd.depthTest = true;
    wpd.depthWrite = false;
    wpd.blend.enable = true;
    wpd.blend.srcColor = BlendFactor::SrcAlpha;
    wpd.blend.dstColor = BlendFactor::OneMinusSrcAlpha;
    wpd.blend.srcAlpha = BlendFactor::One;
    wpd.blend.dstAlpha = BlendFactor::OneMinusSrcAlpha;
    wpd.colorFormat = fmt;
    wpd.sampleCount = samples;
    if (waterSurfacePipeline_.valid()) dev_->destroyPipeline(waterSurfacePipeline_);
    waterSurfacePipeline_ = dev_->createPipeline(wpd);
  }
  if (waterRvs_.valid() && waterRfs_.valid()) {
    PipelineDesc rpd;
    rpd.vertexShader = waterRvs_;
    rpd.fragmentShader = waterRfs_;
    fillVertexLayout(rpd);
    rpd.cullMode = CullMode::None;
    rpd.depthTest = true;
    rpd.depthWrite = true;
    rpd.colorFormat = fmt;
    rpd.sampleCount = samples;
    if (waterReceiverPipeline_.valid()) dev_->destroyPipeline(waterReceiverPipeline_);
    waterReceiverPipeline_ = dev_->createPipeline(rpd);
  }
```

注意:水面材质需走 blend 分区——`tierOf` 读 `meshData()[0].material.alphaBlend`,水面网格的 MaterialData 构建时置 `alphaBlend=true`(Task 5 场景构建);`WaterRenderable` 不参与 transmission 拆分语义(表面按 tier2 在 pass B 画,池底 opaque 在 pass A,行为正确)。

- [ ] **Step 4: 语义测试(先写,跑通接线)**

`tests/renderer/water_surface_test.cpp` 追加(文件头部补 include):

```cpp
#include "common/image.h"
#include "renderer/renderer.h"
#include "resource/mesh_render_resource.h"
#include "resource/primitives.h"
#include "scene/camera.h"
#include <glm/glm.hpp>

namespace {
constexpr uint32_t kW = 256, kH = 256;

struct WaterRig {
  std::unique_ptr<rd::Device> dev;
  rd::Renderer renderer;
  rd::TargetHandle target;
  std::shared_ptr<rd::MeshRenderResource> floor, surf;
  bool ok = false;

  WaterRig(rd::Backend b) {
    rd::DeviceDesc d;
    d.backend = b;
    dev = rd::createDevice(d);
    if (!dev) return;
    auto load = [&](const char* n) {
      return rd::test::loadShaderCode(b, RD_SHADER_DIR, n);
    };
    auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
    auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
    auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
    auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
    auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
    auto exFs = load("bloom_extract.frag"), bbFs = load("bloom_blur.frag");
    auto cpFs = load("composite.frag"), fxFs = load("fxaa.frag");
    rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                              pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                              sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                              cpFs.code,   fxFs.code,   {}, {}, {}, {}, {}, {}, {}, {}, {},
                              unlitVs.entry, rd::Format::RGBA8_UNORM,
                              load("water_step.frag").code, load("water_caustics.frag").code,
                              load("water_surface.vert").code,
                              load("water_surface.frag").code,
                              load("water_receiver.vert").code,
                              load("water_receiver.frag").code};
    rd::OffscreenTargetDesc td;
    td.width = kW; td.height = kH; td.depth = true;
    target = dev->createOffscreenTarget(td);
    if (!target.valid() || !renderer.init(*dev, sd)) return;
    rd::WaterDesc wd;
    wd.simSize = 128;
    if (!renderer.enableWater(wd)) return;
    // 池底 + 水面
    auto f = rd::primitives::makePlane(4.0f, 2.0f);
    f.material.roughnessFactor = 0.9f;
    rd::ModelAsset fm;
    fm.meshes.push_back(std::move(f));
    fm.boundingRadius = 3.0f;
    floor = rd::MeshRenderResource::upload(*dev, fm);
    auto s = rd::primitives::makeGrid(4.0f, 64);
    s.material.alphaBlend = true;
    s.material.baseColorFactor[0] = 0.15f;
    s.material.baseColorFactor[1] = 0.35f;
    s.material.baseColorFactor[2] = 0.4f;
    rd::ModelAsset sm;
    sm.meshes.push_back(std::move(s));
    sm.boundingRadius = 3.0f;
    surf = rd::MeshRenderResource::upload(*dev, sm);
    ok = floor && surf;
  }

  std::vector<uint8_t> frame(uint32_t i) {
    rd::scene::Camera cam;
    cam.lookAt({2.2f, 2.0f, 2.4f}, {0, -0.2f, 0}, {0, 1, 0});
    cam.setPerspective(0.78539816f, 1.0f, 0.1f, 50.0f);
    dev->beginFrame();
    renderer.tick(1.0f / 60.0f);
    renderer.beginScene(cam, {0.05f, 0.05f, 0.06f, 1.0f});
    renderer.submitWaterReceiver(floor, glm::translate(rd::math::Mat4(1.0f),
                                                       rd::math::Vec3(0, -1.0f, 0)));
    renderer.submitWaterSurface(surf, rd::math::Mat4(1.0f));
    auto* cmd = dev->acquireCommandBuffer();
    renderer.endScene(cmd, target);
    dev->submit(cmd);
    dev->waitIdle();
    dev->endFrame();
    (void)i;
    std::vector<uint8_t> px(size_t(kW) * kH * 4);
    dev->readbackTarget(target, px.data(), px.size());
    return px;
  }
};

size_t maxDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  size_t m = 0;
  for (size_t i = 0; i < a.size(); ++i) m = std::max(m, size_t(std::abs(int(a[i]) - int(b[i]))));
  return m;
}
} // namespace

TEST(Water, StillFramesIdentical) {
#if defined(__APPLE__)
  WaterRig rig(rd::Backend::Metal);
  ASSERT_TRUE(rig.ok);
  auto a = rig.frame(0);
  auto b = rig.frame(1);
  auto c = rig.frame(2);
  EXPECT_EQ(maxDiff(a, b), 0u);  // 平态启动:逐帧零漂移
  EXPECT_EQ(maxDiff(b, c), 0u);
#endif
}

TEST(Water, InjectChangesThenDecays) {
#if defined(__APPLE__)
  WaterRig rig(rd::Backend::Metal);
  ASSERT_TRUE(rig.ok);
  auto still = rig.frame(0);
  rig.renderer.disturbWater(0.5f, 0.5f, 0.05f, 3.0f);
  auto ripple = rig.frame(1);
  EXPECT_GT(maxDiff(still, ripple), 4u);  // 注入必可见
  for (int i = 0; i < 900; ++i) rig.frame(2);  // 15s @60fps 阻尼耗尽
  auto settled = rig.frame(3);
  EXPECT_LE(maxDiff(still, settled), 2u);  // 回到平态(±量化噪声)
#endif
}

TEST(Water, GateZeroOp) {
#if defined(__APPLE__)
  WaterRig rig(rd::Backend::Metal);
  ASSERT_TRUE(rig.ok);
  rig.renderer.setWaterParams({0.0f, 3.0f, 1.0f});  // waveScale=0 静态场
  rig.renderer.disturbWater(0.5f, 0.5f, 0.05f, 3.0f);
  auto a = rig.frame(0);
  rig.renderer.setWaterParams({0.0f, 0.0f, 1.0f});  // causticsIntensity 0 vs 3
  auto b = rig.frame(1);
  EXPECT_EQ(maxDiff(a, b), 0u);  // 静态场下焦散强度零操作
#endif
}
```

- [ ] **Step 5: 跑语义测试**

Run: `cmake --build build -j --target rd_tests && ctest --test-dir build -R "Water" --output-on-failure | tail -6`
Expected: WaterSurface.CreateStepSmoke / Water.StillFramesIdentical / Water.InjectChangesThenDecays / Water.GateZeroOp 全 PASS。
若 StillFramesIdentical 失败:检查 ping-pong 初始清零与 step 未注入时数值守恒(2u-u_prev+0=恒等);若 Inject 失败:检查注入 UBO 布局与 consume 后清零。

- [ ] **Step 6: 全量回归**

Run: `ctest --test-dir build -E "perf|Interactive" --output-on-failure | tail -3`
Expected: 全 PASS(FrameUBO 336 扩展对 pbr 族透明——绑定区间大于 shader 声明块合法;golden 零回归)。
若 golden 失败:排查是否有绑定 size 漏改(rg "272" core/renderer)。

- [ ] **Step 7: Commit**

```bash
git add core/renderer/ tests/renderer/water_surface_test.cpp
git commit -m "feat(renderer): Renderer 水系统集成(FrameUBO 336 尾部/water 管线族/endScene 仿真 pass)+ 语义测试四件"
```

---

### Task 4: QualityPreset + options + C API(load_scene/water_disturb)+ 引擎场景/雨滴

**Files:**
- Modify: `core/renderer/quality.h`、`core/renderer/quality.cpp`、`core/api/options.json`、`core/api/rd_api.h`、`core/api/rd_api.cpp`

- [ ] **Step 1: QualityPreset 扩展**

`core/renderer/quality.h` `QualityPreset` **末尾**追加:

```cpp
  uint32_t waterSimSize;      ///< 水仿真/焦散纹理边长(0=128 兜底)
  uint32_t waterCaustics;     ///< 焦散 pass(0=关)
```

`core/renderer/quality.cpp` 三档末尾追加两个初始化值(High 512/1,Mid 256/1,Low 128/0,兜底行 128/0):

```cpp
  case QualityTier::High: return {1.0f, 4, 256, 6, 4096, 2048, 1, 0, 1, 1, 512, 1};
  case QualityTier::Mid:  return {0.75f, 2, 128, 5, 2048, 1024, 1, 0, 1, 1, 256, 1};
  case QualityTier::Low:  return {0.5f, 1, 64, 4, 1024, 0, 0, 1, 0, 0, 128, 0};
```

(兜底 return 行同步追加 `128, 0`。)

Run: `cmake --build build -j --target rd_tests && ctest --test-dir build -R Quality --output-on-failure | tail -2`
Expected: quality_test 可能对预设字段做全字段比较——如有 FAIL 按测试期望更新(High/Mid/Low 各加 water 字段断言 512/256/128)。

- [ ] **Step 2: options.json 四项**

`core/api/options.json` 追加:

```json
  , "water.rain": {"type": "bool", "default": true, "doc": "自动雨滴(引擎 water_pool 场景)"},
  "water.wave_scale": {
    "type": "float", "default": 1.0,
    "domain": {"style": "range", "min": 0.0, "max": 2.0, "step": 0.05},
    "doc": "波幅缩放(0=静止)"
  },
  "water.caustics_intensity": {
    "type": "float", "default": 1.0,
    "domain": {"style": "range", "min": 0.0, "max": 3.0, "step": 0.05},
    "doc": "焦散亮度"
  },
  "water.depth": {
    "type": "float", "default": 1.0,
    "domain": {"style": "range", "min": 0.2, "max": 3.0, "step": 0.05},
    "doc": "水深(吸收/焦散衰减)"
  }
```

(注意 JSON 语法:并入现有对象,前一项补逗号;构建时 GenOptions 自动重生成 `options_generated.h`,`o.water.rain` 等字段即可用。)

Run: `cmake --build build -j 2>&1 | tail -3`
Expected: 重新配置生成成功;`build/generated/options_generated.h` 含 `struct water`。

- [ ] **Step 3: C API 声明**

`core/api/rd_api.h`(`rd_engine_load_gltf` 声明附近)追加:

```c
/// 加载程序场景(当前支持 "water_pool":波动方程水面+焦散池)。
/// 需 surface 就绪(renderer init 后);未知名返回 RD_ERROR_ASSET。
rd_result_t rd_engine_load_scene(rd_engine* engine, const char* name);

/// 屏幕像素坐标点按注入涟漪(射线∩水面;未命中忽略)。water_pool 场景外 no-op。
void rd_engine_water_disturb(rd_engine* engine, float x, float y);
```

- [ ] **Step 4: 引擎实现(rd_api.cpp)**

(a) `rd_engine` 结构追加成员(`animator` 块后):

```cpp
  // ---- water_pool 程序场景 ----
  struct WaterScene {
    bool active = false;
    std::shared_ptr<rd::MeshRenderResource> surface;
    std::vector<std::shared_ptr<rd::MeshRenderResource>> receivers;
    std::vector<rd::math::Mat4> worlds;
    float rainTimer = 0.4f;
    uint32_t rainSeed = 0x9E3779B9u;  // LCG(确定性)
  } waterScene;
```

(b) 匿名 namespace 追加(`installModel` 后):

```cpp
/// water_pool 池场景:池底/四壁/两球一柱(受水体)+ 水面网格。
/// 常量与 tools/render_test/scenes.cpp 的 buildWaterPool 保持一致。
bool buildWaterPoolScene(rd_engine* e) {
  if (!e->rendererReady) {
    setError(e, "load_scene 需 surface 就绪后调用");
    return false;
  }
  // 卸载模型路径状态
  if (e->model) {
    e->device->waitIdle();
    e->model->destroy(*e->device);
    e->model = nullptr;
  }
  e->scene = std::make_unique<rd::scene::Scene>();
  e->modelAsset = rd::ModelAsset();
  e->hasAnimation = false;
  e->animator = rd::scene::Animator();
  auto& ws = e->waterScene;
  ws.receivers.clear();
  ws.worlds.clear();
  auto put = [&](rd::MeshData&& m, const rd::math::Mat4& w) {
    rd::ModelAsset a;
    a.meshes.push_back(std::move(m));
    a.boundingRadius = 3.0f;
    auto res = rd::MeshRenderResource::upload(*e->device, a);
    if (!res) return false;
    ws.receivers.push_back(res);
    ws.worlds.push_back(w);
    return true;
  };
  const rd::math::Mat4 I(1.0f);
  // 池底(y=-1)
  auto floor = rd::primitives::makePlane(4.0f, 4.0f);
  floor.material.roughnessFactor = 0.9f;
  floor.material.baseColorFactor[0] = floor.material.baseColorFactor[1] =
      floor.material.baseColorFactor[2] = 0.72f;
  if (!put(std::move(floor), glm::translate(I, rd::math::Vec3(0, -1.0f, 0)))) return false;
  // 四壁(薄盒 4×1.25×0.1,y -1..0.25;左右壁绕 Y 转 90°)
  const rd::math::Vec3 wallPos[4] = {{0, -0.375f, -2.0f}, {0, -0.375f, 2.0f},
                                     {-2.0f, -0.375f, 0}, {2.0f, -0.375f, 0}};
  for (int i = 0; i < 4; ++i) {
    auto wm = rd::primitives::makeBox(4.0f, 1.25f, 0.1f);
    wm.material.roughnessFactor = 0.85f;
    wm.material.baseColorFactor[0] = 0.8f;
    wm.material.baseColorFactor[1] = 0.78f;
    wm.material.baseColorFactor[2] = 0.74f;
    rd::math::Mat4 w = glm::translate(I, wallPos[i]);
    if (i >= 2) w = w * glm::rotate(I, 1.5707963f, rd::math::Vec3(0, 1, 0));
    if (!put(std::move(wm), w)) return false;
  }
  // 两球 + 一柱(水下物体)
  auto sph = rd::primitives::makeSphere(0.4f, 32, 16);
  sph.material.roughnessFactor = 0.4f;
  sph.material.baseColorFactor[2] = 0.9f;
  if (!put(std::move(sph), glm::translate(I, rd::math::Vec3(-0.9f, -0.6f, -0.6f))))
    return false;
  auto sph2 = rd::primitives::makeSphere(0.4f, 32, 16);
  sph2.material.roughnessFactor = 0.4f;
  sph2.material.baseColorFactor[0] = 0.9f;
  if (!put(std::move(sph2), glm::translate(I, rd::math::Vec3(0.9f, -0.55f, 0.7f))))
    return false;
  auto col = rd::primitives::makeBox(0.5f, 1.6f, 0.5f);
  col.material.roughnessFactor = 0.7f;
  if (!put(std::move(col), glm::translate(I, rd::math::Vec3(0.1f, -0.2f, -1.2f))))
    return false;
  // 水面网格
  auto surf = rd::primitives::makeGrid(4.0f, 128);
  surf.material.alphaBlend = true;
  surf.material.roughnessFactor = 0.05f;
  surf.material.baseColorFactor[0] = 0.15f;
  surf.material.baseColorFactor[1] = 0.35f;
  surf.material.baseColorFactor[2] = 0.4f;
  rd::ModelAsset sm;
  sm.meshes.push_back(std::move(surf));
  sm.boundingRadius = 3.0f;
  ws.surface = rd::MeshRenderResource::upload(*e->device, sm);
  if (!ws.surface) return false;
  // 水系统
  rd::WaterDesc wd;  // 默认 planeY=0/size 4×4;simSize/caustics 走画质档(setQuality 联动)
  if (!e->renderer.enableWater(wd)) {
    setError(e, "水面不可用(caps/shader 缺失)");
    return false;
  }
  ws.active = true;
  // 取景 + 灯光(暖方向光)
  e->orbit.frameModel((const float[]){0, -0.3f, 0}, 3.2f);
  e->manualLights.clear();
  rd::LightData dl;
  dl.type = rd::LightType::Directional;
  const float n = std::sqrt(0.3f * 0.3f + 1.0f + 0.45f * 0.45f);
  dl.direction[0] = 0.3f / n;
  dl.direction[1] = 1.0f / n;
  dl.direction[2] = 0.45f / n;
  dl.color[0] = 3.2f; dl.color[1] = 3.0f; dl.color[2] = 2.7f;
  e->manualLights.push_back(dl);
  e->gltfLights.clear();
  e->lightsDirty = true;
  e->renderer.setLightFraming((const float[]){0, -0.5f, 0}, 2.8f);
  e->renderDirty = true;
  return true;
}
```

(四壁循环直接如上——每壁重建 makeBox 并设置材质,无需独立 `wall` 变量与冗余分支。)

(c) `applyOptions` 末尾追加:

```cpp
  rd::WaterParams wp;
  wp.waveScale = o.water.wave_scale;
  wp.causticsIntensity = o.water.caustics_intensity;
  wp.depth = o.water.depth;
  e->renderer.setWaterParams(wp);
```

(d) `rd_engine_load_scene`/`rd_engine_water_disturb` 实现(文件尾部选项函数前):

```cpp
rd_result_t rd_engine_load_scene(rd_engine* e, const char* name) {
  if (!e || !name) return RD_ERROR_INVALID_ARG;
  if (std::string(name) != "water_pool") {
    setError(e, (std::string("未知场景: ") + name).c_str());
    return RD_ERROR_ASSET;
  }
  if (!buildWaterPoolScene(e)) return RD_ERROR_SCENE;
  return RD_OK;
}

void rd_engine_water_disturb(rd_engine* e, float x, float y) {
  if (!e || !e->waterScene.active || !e->rendererReady) return;
  const float vw = e->width ? float(e->width) : 512.0f;
  const float vh = e->height ? float(e->height) : 512.0f;
  applyCamera(e, vw, vh);
  float o[3], d[3];
  rd::scene::screenRay(e->camera, x, y, vw, vh, o, d);
  const float planeY = 0.0f;  // water_pool 水面高度(WaterDesc 默认)
  if (std::fabs(d[1]) < 1e-5f) return;
  const float t = (planeY - o[1]) / d[1];
  if (t <= 0.0f) return;
  const float px = o[0] + d[0] * t, pz = o[2] + d[2] * t;
  const float u = px / 4.0f + 0.5f, v = pz / 4.0f + 0.5f;  // 池 4×4
  if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return;
  e->renderer.disturbWater(u, v, 0.045f, 3.0f);
  e->renderDirty = true;
}
```

头部补 include:`#include "renderer/water.h"`、`#include "resource/primitives.h"`、`#include <glm/glm.hpp>`(若未有)。

(e) `render_frame` 三处改:

1. 按需渲染跳过条件加 water(水活动=持续动画):

```cpp
  if (!e->renderDirty && !e->animator.playing() && !e->orbit.isMoving() &&
      !e->waterScene.active)
    return;
```

2. 雨滴(在 `applyOptions(e);` 之后):

```cpp
  if (e->waterScene.active && e->options.water.rain) {
    e->waterScene.rainTimer -= dt;
    if (e->waterScene.rainTimer <= 0.0f) {
      e->waterScene.rainTimer = 0.8f;
      uint32_t& r = e->waterScene.rainSeed;
      r = r * 1664525u + 1013904223u;
      const float u = float((r >> 16) & 0xFFFF) / 65535.0f;
      r = r * 1664525u + 1013904223u;
      const float v = float((r >> 16) & 0xFFFF) / 65535.0f;
      e->renderer.disturbWater(0.12f + u * 0.76f, 0.12f + v * 0.76f, 0.03f, 2.0f);
    }
  }
```

3. 场景提交(`if (e->hasAnimation...) else { e->scene->collect... }` 前插):

```cpp
  if (e->waterScene.active) {
    e->renderer.tick(dt);
    for (size_t i = 0; i < e->waterScene.receivers.size(); ++i)
      e->renderer.submitWaterReceiver(e->waterScene.receivers[i],
                                      e->waterScene.worlds[i]);
    e->renderer.submitWaterSurface(e->waterScene.surface, rd::math::Mat4(1.0f));
  } else if (e->hasAnimation && e->model) {
```

(即原 `if (e->hasAnimation ...)` 链前加 waterScene 分支。)

(f) `installModel` 开头卸载水场景(load_gltf 替换场景时):

```cpp
  if (e->waterScene.active) {
    e->waterScene.active = false;
    e->waterScene.receivers.clear();
    e->waterScene.worlds.clear();
    e->waterScene.surface = nullptr;
    e->renderer.disableWater();
  }
```

(g) `registerCommands` 引擎族追加:

```cpp
  bus.add("load_scene", [e](const std::string& a, std::string&) {
    return rd_engine_load_scene(e, a.c_str()) == RD_OK;
  });
  bus.add("water_disturb", [e](const std::string& a, std::string&) {
    const auto sp = a.find(' ');
    if (sp == std::string::npos) return false;
    rd_engine_water_disturb(e, float(atof(a.substr(0, sp).c_str())),
                            float(atof(a.substr(sp + 1).c_str())));
    return true;
  });
```

- [ ] **Step 5: 构建 + C API 冒烟**

Run: `cmake --build build -j 2>&1 | tail -3 && ctest --test-dir build -E "perf|Interactive" --output-on-failure | tail -3`
Expected: 全 PASS(新增 API 未破坏现有;options 重生成后 api 测试含新选项名)。

- [ ] **Step 6: Commit**

```bash
git add core/renderer/quality.{h,cpp} core/api/options.json core/api/rd_api.{h,cpp}
git commit -m "feat(api): rd_engine_load_scene(water_pool)/water_disturb + water.* 选项 + 画质档 simSize/caustics + 确定性雨滴"
```

---

### Task 5: render_test --scene water_pool + golden 双后端

**Files:**
- Modify: `tools/render_test/scenes.h`、`tools/render_test/scenes.cpp`、`tools/render_test/main.cpp`、`tools/render_test/interactive.mm`、`tests/renderer/demo_scenes_test.cpp`
- Create: `tests/renderer/water_golden_test.cpp`
- Create: `tests/golden/water_pool_metal.png`、`tests/golden/water_pool_vulkan.png`(RD_UPDATE_GOLDENS 生成)

- [ ] **Step 1: DemoScene 扩展 + buildWaterPool(host 直驱版)**

`tools/render_test/scenes.h` `DemoScene` 追加成员(`skinnedRes` 后):

```cpp
  std::shared_ptr<MeshRenderResource> waterSurface;  // water_pool 水面(空=非水场景)
```

`scenes.cpp`:

(a) `kNames` 数组 `"material_ext_gallery"` 后追加 `"water_pool"`(顺序任意,追加到 transmission/morph 之后保持就近):

```cpp
const char* const kNames[] = {"material_balls", "cornell_box", "light_playground",
                              "skinned_demo",   "instanced_field",
                              "sponza",         "cesium_man",
                              "emissive_bloom", "normal_map_wall",
                              "shadow_gallery", "ktx2_gallery", "alpha_blend",
                              "fox_anim",       "material_ext_gallery",
                              "transmission_gallery", "morph_demo",
                              "water_pool"};
```

(b) 匿名 namespace 追加构建函数(几何与 rd_api.cpp 的 buildWaterPoolScene 同构——**维持两边布局常量一致**:池 4×4、底 y=-1、暖光、两球一柱):

```cpp
// 水波纹池:受水体(底/壁/两球一柱)+ 水面网格;波动方程+焦散(Renderer water)
void buildWaterPool(Device& dev, Renderer& renderer, DemoScene& out) {
  const math::Mat4 I(1.0f);
  auto put = [&](MeshData&& m, const math::Vec3& pos, bool rotY = false) {
    math::Mat4 w = glm::translate(I, pos);
    if (rotY) w = w * glm::rotate(I, 1.5707963f, math::Vec3(0, 1, 0));
    uploadInto(dev, wrapMesh(std::move(m)), out, w);
  };
  auto floor = primitives::makePlane(4.0f, 4.0f);
  floor.material.roughnessFactor = 0.9f;
  floor.material.baseColorFactor[0] = floor.material.baseColorFactor[1] =
      floor.material.baseColorFactor[2] = 0.72f;
  put(std::move(floor), math::Vec3(0, -1.0f, 0));
  const math::Vec3 wallPos[4] = {{0, -0.375f, -2.0f}, {0, -0.375f, 2.0f},
                                 {-2.0f, -0.375f, 0}, {2.0f, -0.375f, 0}};
  for (int i = 0; i < 4; ++i) {
    auto wm = primitives::makeBox(4.0f, 1.25f, 0.1f);
    wm.material.roughnessFactor = 0.85f;
    wm.material.baseColorFactor[0] = 0.8f;
    wm.material.baseColorFactor[1] = 0.78f;
    wm.material.baseColorFactor[2] = 0.74f;
    put(std::move(wm), wallPos[i], i >= 2);
  }
  auto sph = primitives::makeSphere(0.4f, 32, 16);
  sph.material.roughnessFactor = 0.4f;
  sph.material.baseColorFactor[2] = 0.9f;
  put(std::move(sph), math::Vec3(-0.9f, -0.6f, -0.6f));
  auto sph2 = primitives::makeSphere(0.4f, 32, 16);
  sph2.material.roughnessFactor = 0.4f;
  sph2.material.baseColorFactor[0] = 0.9f;
  put(std::move(sph2), math::Vec3(0.9f, -0.55f, 0.7f));
  auto col = primitives::makeBox(0.5f, 1.6f, 0.5f);
  col.material.roughnessFactor = 0.7f;
  put(std::move(col), math::Vec3(0.1f, -0.2f, -1.2f));
  auto surf = primitives::makeGrid(4.0f, 128);
  surf.material.alphaBlend = true;
  surf.material.roughnessFactor = 0.05f;
  surf.material.baseColorFactor[0] = 0.15f;
  surf.material.baseColorFactor[1] = 0.35f;
  surf.material.baseColorFactor[2] = 0.4f;
  ModelAsset sm;
  sm.meshes.push_back(std::move(surf));
  sm.boundingRadius = 3.0f;
  out.waterSurface = MeshRenderResource::upload(dev, sm);
  // 水系统(默认 desc;画质档联动由 quality 指定)
  rd::WaterDesc wd;
  renderer.enableWater(wd);
  // 灯光/取景(与 engine 版一致)
  LightData dl;
  dl.type = LightType::Directional;
  const float n = std::sqrt(0.3f * 0.3f + 1.0f + 0.45f * 0.45f);
  dl.direction[0] = 0.3f / n;
  dl.direction[1] = 1.0f / n;
  dl.direction[2] = 0.45f / n;
  dl.color[0] = 3.2f; dl.color[1] = 3.0f; dl.color[2] = 2.7f;
  out.lights.push_back(dl);
  out.camera.lookAt({2.6f, 2.2f, 2.8f}, {0, -0.3f, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 50.0f);
  out.framingCenter[1] = -0.5f;
  out.framingRadius = 2.8f;
  static const rd::QualityPreset kHigh = rd::qualityPreset(rd::QualityTier::High);
  out.quality = &kHigh;
}
```

(头部补 `#include "renderer/water.h"`。)

(c) `buildDemoScene` 分派表追加(else if 链):

```cpp
  } else if (n == "water_pool") {
    buildWaterPool(dev, renderer, out);
```

(d) `submitDemoScene` 开头(`s.animTime += dt;` 后)追加:

```cpp
  if (s.waterSurface) {  // water_pool:受水体 + 水面(固定描述;tick 驱动仿真)
    renderer.tick(dt);
    for (size_t i = 0; i < s.resources.size(); ++i)
      renderer.submitWaterReceiver(s.resources[i], s.worlds[i]);
    renderer.submitWaterSurface(s.waterSurface, math::Mat4(1.0f));
    return;
  }
```

(e) `destroyDemoScene` 追加水面资源释放(`s.skinnedRes` 释放块后):

```cpp
  if (s.waterSurface) s.waterSurface->destroy(dev);
  s.waterSurface = nullptr;
```

- [ ] **Step 2: 三处 RendererShaderDesc 加载点补 water 六件**

`tools/render_test/main.cpp`(--scene 分支)、`tools/render_test/interactive.mm`(runSceneInteractive)、`tests/renderer/demo_scenes_test.cpp`(renderScene)各自:

1. shader 加载行追加(在 morph 加载之后):

```cpp
    auto wStepFs = load("water_step.frag");
    auto wCauFs = load("water_caustics.frag");
    auto wSv = load("water_surface.vert");
    auto wSf = load("water_surface.frag");
    auto wRv = load("water_receiver.vert");
    auto wRf = load("water_receiver.frag");
```

2. `RendererShaderDesc` 聚合初始化**末尾**(`unlitVs.entry, rd::Format::RGBA8_UNORM`/`scFmt` 之后)追加:

```cpp
    , wStepFs.code, wCauFs.code, wSv.code, wSf.code, wRv.code, wRf.code
```

(demo_scenes_test 的 desc 用位置式且显式列了 `{}` 占位——同样在末尾 colorFormat 后追加六个 .code。)

- [ ] **Step 3: 手动冒烟**

Run: `cmake --build build -j --target render_test 2>&1 | tail -2 && ./build/tools/render_test/render_test --backend metal --scene water_pool --out /tmp/water.png`
Expected: exit 0;`已保存 /tmp/water.png (512x512,场景 water_pool)`。
Run: `./build/tools/img_check/img_check /tmp/water.png --min-coverage 0.03`
Expected: PASS。
目视核对:俯视水池,水面半透明蓝绿,池底可见两球一柱与四壁;此帧无注入=静水(平态),波动需交互/golden 注入驱动。

- [ ] **Step 4: golden 测试文件**

`tests/renderer/water_golden_test.cpp`:

```cpp
// water_pool golden:90 帧确定性仿真 + 两次脚本注入(雨滴关=host 场景无雨)。
#include <gtest/gtest.h>
#include "common/golden_test.h"
#include "common/shader_code.h"
#include "tools/render_test/scenes.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 512, kH = 512;

rd::test::Image renderWaterPool(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto load = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
  auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
  auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
  auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
  auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
  auto exFs = load("bloom_extract.frag"), bbFs = load("bloom_blur.frag");
  auto cpFs = load("composite.frag"), fxFs = load("fxaa.frag");
  auto wStepFs = load("water_step.frag"), wCauFs = load("water_caustics.frag");
  auto wSv = load("water_surface.vert"), wSf = load("water_surface.frag");
  auto wRv = load("water_receiver.vert"), wRf = load("water_receiver.frag");
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   {}, {}, {}, {}, {}, {}, {}, {}, {},
                            unlitVs.entry, rd::Format::RGBA8_UNORM,
                            wStepFs.code, wCauFs.code, wSv.code, wSf.code, wRv.code,
                            wRf.code};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  rd::Renderer renderer;
  if (!target.valid() || !renderer.init(*device, sd)) return {};
  rd::ModelAsset storage;
  rd::tool::DemoScene scene;
  if (!rd::tool::buildDemoScene("water_pool", *device, renderer, scene, storage)) return {};
  // 90 帧 @1/60;帧 10/34 注入两滴(确定性)
  for (int f = 0; f < 90; ++f) {
    if (f == 10) renderer.disturbWater(0.35f, 0.42f, 0.05f, 3.0f);
    if (f == 34) renderer.disturbWater(0.62f, 0.55f, 0.04f, 3.0f);
    device->beginFrame();
    renderer.beginScene(scene.camera, {0.05f, 0.05f, 0.06f, 1.0f});
    rd::tool::submitDemoScene(scene, renderer, 1.0f / 60.0f);
    auto* cmd = device->acquireCommandBuffer();
    renderer.endScene(cmd, target);
    device->submit(cmd);
    device->waitIdle();
    device->endFrame();
  }
  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(size_t(kW) * kH * 4);
  device->readbackTarget(target, img.pixels.data(), img.pixels.size());
  rd::tool::destroyDemoScene(scene, *device);
  renderer.shutdown();
  return img;
}
} // namespace

RD_GOLDEN_TEST(Water, Pool, "water_pool", 0.05, renderWaterPool)
```

`tests/CMakeLists.txt` renderer 源列表追加 ` renderer/water_golden_test.cpp`。

- [ ] **Step 5: 生成 golden + 全量回归**

Run: `cmake --build build -j --target rd_tests && RD_UPDATE_GOLDENS=1 ctest --test-dir build -R "Water.Pool" --output-on-failure | tail -4`
Expected: 生成 `tests/golden/water_pool_{metal,vulkan}.png`,测试 SKIP(golden 已更新)。
**目视核对两张 PNG**:水面涟漪环纹清晰、池底焦散网纹、两球一柱投影——确认后提交。

Run: `ctest --test-dir build -R "Water.Pool" --output-on-failure | tail -2`
Expected: Metal/Vulkan 双 PASS(SSIM)。

Run: `ctest --test-dir build -E "perf|Interactive" --output-on-failure | tail -3`
Expected: 全 PASS(demo_scenes SmokeAll 自动纳入 water_pool,覆盖率断言过)。

- [ ] **Step 6: interactive 冒烟(单击涟漪)**

`tools/render_test/interactive.mm` `runSceneInteractive` 的 GLFW 回放路径外,`Ctx` 加 `double downX = 0, downY = 0;` + `bool waterScene = false;`;`onMouseButton` PRESS 记录 `c->downX = x; c->downY = y;`,RELEASE 时判 tap(位移 < 6px)且 `c->waterScene` → 计算相机 ray ∩ y=0 平面注入 `renderer`——**直驱路径拿 renderer 不便,简化:tap 注入走 DemoScene 水域中心**——更简单可靠的做法:场景路径 tap 直接按屏幕归一化映射池 UV(俯视近似)误差大。
**最终采用:interactive 场景路径 tap 不实现精确求交,用固定两点之一?否。** 维持一致性:在 `runSceneInteractive` 内注册窗口级 tap 处理——该函数持有 `renderer/scene/orbit`,在循环里消费 `ctx.tapPending`:

```cpp
// Ctx 追加:
  bool waterScene = false;
  bool tapPending = false;
  double tapX = 0, tapY = 0, downX = 0, downY = 0;
// onMouseButton RELEASE 分支追加(位移 < 6px 判 tap):
    if (c->waterScene && std::fabs(x - c->downX) < 6 && std::fabs(y - c->downY) < 6) {
      c->tapPending = true;
      c->tapX = x; c->tapY = y;
    }
// runSceneInteractive 帧循环开头(orbit.update 之前):
    if (ctx.tapPending) {
      ctx.tapPending = false;
      // 相机 ray ∩ y=0 平面 → 池 uv
      float sx, sy;
      glfwGetWindowContentScale(win, &sx, &sy);
      scene.camera.setPerspective(0.78539816f, float(fbw) / float(fbh),
                                  std::max(0.01f, orbit.distance() * 0.02f),
                                  orbit.distance() * 20.0f);
      float o[3], dirc[3];
      rd::scene::screenRay(scene.camera, float(ctx.tapX * sx), float(ctx.tapY * sy),
                           float(fbw * sx), float(fbh * sy), o, dirc);
      if (std::fabs(dirc[1]) > 1e-5f) {
        const float t = -o[1] / dirc[1];
        const float px = o[0] + dirc[0] * t, pz = o[2] + dirc[2] * t;
        const float u = px / 4.0f + 0.5f, v = pz / 4.0f + 0.5f;
        if (u >= 0 && u <= 1 && v >= 0 && v <= 1)
          renderer.disturbWater(u, v, 0.045f, 3.0f);
      }
    }
```

(`ctx.waterScene = scene.waterSurface != nullptr;` 在构建场景后设置;头部 `#include "scene/picking.h"` 取 `screenRay`。)

Run: `RD_INTERACTIVE_FRAMES=90 ./build/tools/render_test/render_test --interactive --scene water_pool`
Expected: exit 0(90 帧冒烟;本地人工拖拽/点按看涟漪)。

- [ ] **Step 7: Commit**

```bash
git add tools/render_test/ tests/renderer/water_golden_test.cpp tests/CMakeLists.txt tests/golden/water_pool_*.png
git commit -m "feat(tools+test): water_pool 场景(host 直驱)+ golden 双后端(90 帧注入确定性)+ interactive 单击涟漪"
```

---

### Task 6: iOS demo 接入(场景菜单 + 单击涟漪)

**Files:**
- Modify: `platform/ios/RenderView.swift`、`samples/ios/RdDemo/AppDelegate.swift`

- [ ] **Step 1: RenderView.loadScene + 单击涟漪**

`platform/ios/RenderView.swift`:

(a) `loadModel` 后追加:

```swift
    /// 加载程序场景(如 water_pool;主线程)。成功返回 true。
    @discardableResult
    public func loadScene(_ name: String) -> Bool {
        guard let engine else { return false }
        let r = rd_engine_load_scene(engine, name)
        print("RD: load_scene \(name) -> \(r)")
        return r == RD_OK
    }
```

(b) `startEngine` 双击手势注册后追加单击(tap 须等双击失败才触发,避免抢双击重置):

```swift
        // 单击涟漪(需等双击失败;water_pool 场景外 engine 内部 no-op)
        let singleTap = UITapGestureRecognizer(target: self, action: #selector(onSingleTap(_:)))
        singleTap.require(toFail: doubleTap)
        addGestureRecognizer(singleTap)
```

(c) 手势响应(`onDoubleTap` 旁):

```swift
    @objc private func onSingleTap(_ g: UITapGestureRecognizer) {
        guard let engine else { return }
        let p = g.location(in: self)
        rd_engine_water_disturb(engine, Float(p.x * contentScaleFactor),
                                Float(p.y * contentScaleFactor))
    }
```

- [ ] **Step 2: AppDelegate 场景型 state**

`samples/ios/RdDemo/AppDelegate.swift`:

(a) `DemoState` 加字段与新增水场景状态:

```swift
    private struct DemoState {
        let quality: Int
        let model: String
        let label: String
        let scene: String?   // 非空=程序场景(如 water_pool),model 字段忽略
    }
```

`states()` 中 helmet 三态与 extras 之间插入:

```swift
        s.append(DemoState(quality: 1, model: "water_pool", label: "水波纹",
                           scene: "water_pool"))
```

(b) `applyCurrentState` 分支:

```swift
    private func applyCurrentState() {
        guard let renderView else { return }
        let st = states()[stateIndex]
        renderView.setQuality(st.quality)
        if let scene = st.scene {
            renderView.loadScene(scene)
        } else if let path = Bundle.main.path(forResource: st.model, ofType: "glb") {
            renderView.loadModel(path)
        }
        print("RD: demo 状态 -> \(st.label)/\(st.model)")
    }
```

(c) `onSwitchTap` 菜单标题沿用 `st.label / st.model`(water_pool 显示 `水波纹 / water_pool`,无需改)。

- [ ] **Step 3: iOS 构建**

Run: `cmake -S . -B build-ios -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake -DRD_IOS_SDK=iphonesimulator >/dev/null && cmake --build build-ios --config Debug 2>&1 | tail -3`
Expected: BUILD SUCCEEDED(rd_core 含 water.cpp + 6 个 shader 内嵌,`water_step` 等已入 ShaderList)。

- [ ] **Step 4: 模拟器人工验证(记录到 PR/提交说明)**

xcodebuild/模拟器运行 RdDemo(现有流程):场景菜单出现「水波纹」项;进入后 60fps 波动水面 + 自动雨滴;轻点水面出涟漪环;长按录制通道不受影响。
(自动化截图校验沿用既有 ios_metal.png 流程的,可选执行 `RD_UPDATE_GOLDENS` 不涉及。)

- [ ] **Step 5: Commit**

```bash
git add platform/ios/RenderView.swift samples/ios/RdDemo/AppDelegate.swift
git commit -m "feat(ios): demo 接入 water_pool 场景菜单 + 单击涟漪(water_disturb)"
```

---

### Task 7: 全量回归 + AGENTS.md 收尾

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: 全量检查**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 配置+构建+全部测试 PASS(golden 零回归;perf 基线不受影响——水场景未入 perf 套件)。

- [ ] **Step 2: AGENTS.md 更新**

「当前状态」列表追加一行:

```markdown
- 水波纹完成:波动方程水面(RGBA16F ping-pong,固定 1/60 子步×≤2,clamp 边界=池壁
  反射,注入 UBO 高斯脉冲×8)+ Jacobian 汇聚焦散(视差近似,Low 档关绑 1×1 黑)+
  水面管线(顶点 fetch 位移+三点差分法线+Fresnel/IBL/太阳 GGX+Beer-Lambert 水色,
  blend 半透明)+ 受水体管线(漫反射+阴影 PCF+SH+焦散调制,水线上方无焦散);
  独立 combined 管线族(槽 1=texWave/2=texCaustics/5=prefilter/7=shadow,GLES 语义
  名表+块名表增补);FrameUBO 272→336B(water[3] 尾部,未激活全零);
  WaterRenderable(surface 不投影/receiver 常规深度;不参与实例化分组与视锥剔除);
  画质档 simSize 512/256/128+caustics 1/1/0 联动重建;选项 water.rain/wave_scale/
  caustics_intensity/depth;C API rd_engine_load_scene("water_pool")/water_disturb
  (屏幕 ray∩水面)+ 命令 load_scene/water_disturb + 确定性 LCG 雨滴(0.8s;
  render_frame 水活动时持续渲);render_test --scene water_pool + golden 双后端
  (90 帧两注入确定性)+ interactive 单击涟漪;iOS demo 场景菜单+单击涟漪
  (singleTap require(toFail: doubleTap));已知限制:焦散单次折射视差近似/
  水面不投影不接收阴影/不反射场景几何(仅 IBL)/GLES 无 half-float caps 整体禁用
```

「构建与测试」段追加两行:

```markdown
- 水波纹:`./build/tools/render_test/render_test --scene water_pool --out x.png`;
  交互 `--interactive --scene water_pool`(单击涟漪/拖拽 orbit);
  golden water_pool 双后端(90 帧 2 注入);语义测试 Water.*(平态零漂移/注入可见/
  阻尼归零/静态场零操作)
```

- [ ] **Step 3: Commit**

```bash
git add AGENTS.md
git commit -m "docs: 水波纹(波动方程+焦散)记入 AGENTS.md"
```

---

## 自审记录(写完计划后核对)

1. **Spec 覆盖**:§1 架构=Task 3;§2 仿真=Task 1 step shader + Task 2 WaterSurface(固定子步/注入/边界/雨滴 Task 4);§3 焦散=Task 1 caustics + Task 2 pass;§4 着色=Task 1 surface/receiver + Task 3 管线;§5 画质=Task 4 Step 1 + Task 3 setQuality 联动;§6 C API/options=Task 4;§7 iOS=Task 6;§8 测试=Task 3 语义 + Task 5 golden/冒烟/SmokeAll;§9 限制已入 AGENTS 文案;§10 顺序与 Task 1-7 对应。✓
2. **占位符**:无 TBD/TODO;所有代码块为最终形态(water_receiver.frag 阴影段单一 PCF 循环;rd_api.cpp 四壁循环直接重建 makeBox);跨 Task 依赖已注明(Task 3 Step 1 可提前并入 Task 2)。✓
3. **类型一致性**:WaterSurface 方法名 create/destroy/disturb/tick/step/waveTex/causticsTex/sampler/causticsOn/setParams/setLightDir 全计划一致;Renderer 方法 enableWater/disableWater/waterActive/setWaterParams/disturbWater/tick/submitWaterSurface/submitWaterReceiver 一致;RenderContext 字段 waterSurfacePipeline/waterReceiverPipeline/waterWave/waterCaustics/waterSampler 与 water.cpp record 一致;FrameUBO 336 与全部绑定点一致。✓
4. **风险点**:RendererShaderDesc 追加在**末尾**(三处位置式聚合初始化不破坏);quality.cpp 位置式初始化同; GLES kSamplerTable/kBlockTable 增补为唯一 GLES 改动;Vulkan sampler binding 全阶段标志已有(vert fetch 可行);Metal bindTexture 已双阶段(P4-C)。✓
