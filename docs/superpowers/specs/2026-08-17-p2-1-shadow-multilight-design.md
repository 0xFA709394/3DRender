# P2-1 设计:阴影 + 多光源

日期:2026-08-17
状态:已确认(用户审阅通过)
前置:总设计 docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md;
阶段二c(Orbit+画质分级+KTX2+上屏链)已完成

## 0. 目标与范围

P2 首个子项目:单方向光阴影 + KHR_lights_punctual 多光源。

1. **阴影**:单方向光 + 单张 shadow map,按模型包围球自动正交取景;
   深度纹理 + 硬件比较采样 + PCF 3x3;画质档控制尺寸/开关。
2. **多光源**:glTF `KHR_lights_punctual` 全类型(dir/point/spot)× 4 盏上限;
   灯光来源双通道:glTF 解析 + C API 手动设置。

非目标:CSM 级联、点光/聚光灯阴影、PCSS、体积光、阴影图集(atlas)、
多光源逐光源阴影。

## 1. RHI 扩展(契约测试门控)

### 1a. 可采样深度纹理 + 比较采样器

- `SamplerDesc` 新增 `bool compareEnable = false`:
  Vulkan `VkSamplerCreateInfo.compareEnable=VK_TRUE, compareOp=LESS`;
  Metal `MTLSamplerDescriptor.compareFunction = Less`;
  GLES `glSamplerParameteri(TEXTURE_COMPARE_MODE, COMPARE_REF_TO_TEXTURE)` +
  `TEXTURE_COMPARE_FUNC = LESS`。
- D32_FLOAT 纹理可作渲染目标:`TextureDesc{format=D32_FLOAT,
  usage=Sampled|RenderTargetAttachment}`;Metal storageMode Private;
  Vulkan image usage 加 `DEPTH_STENCIL_ATTACHMENT`(视图 aspect=DEPTH)。

### 1b. 纯深度渲染目标(depth-only target)

- `OffscreenTargetDesc` 新增 `TextureHandle depthFromTexture`:非空则挂该 D32
  纹理为深度附件、**无颜色附件**;与 `colorFromTexture` 互斥;`sampleCount>1` 拒绝。
- Vulkan:render pass 缓存加 depth-only 变体(单深度附件,storeOp=STORE,
  finalLayout=SHADER_READ_ONLY);pipeline 侧的 pass 键以 colorFormat=
  VK_FORMAT_UNDEFINED 区分。endRenderPass 对 depth-only 目标只做
  DEPTH_ATTACHMENT→SHADER_READ 转换(无 staging 拷贝);readbackTarget 返回 false。
- Metal:render pass 只挂 depthAttachment(loadAction=Clear, storeAction=Store)。
- GLES:FBO 只挂深度纹理,`glDrawBuffer(GL_NONE)/glReadBuffer(GL_NONE)`。
- `targetColorTexture` 对 depth-only 目标返回深度纹理句柄(可采样)。
- `targetSize` 正常返回尺寸。
- 契约测试:渲三角形到 depth-only 目标 → 绑比较采样器采样 → 阴影比较结果
  三后端一致(参考深度内/外两区)。

### 1c. PipelineDesc.depthOnly

`bool depthOnly = false`:depth-only pass 用。Vulkan 按 (UNDEFINED, depth=true,
samples) 键取 pass、颜色混合状态跳过;Metal 不设 colorAttachments[0] 格式;
GLES 无操作。

## 2. 多光源 + 阴影渲染(renderer)

### 2a. LightUBO(slot 2,共 352B)

```
mat4 lightViewProj;      // 阴影光源变换(64B)
vec4 shadowParams;       // x=bias, y=1/shadowMapSize, z=shadowOn, w=pad
vec4 lightCount;         // x=光源数(0..4)
struct Light {           // 64B/盏
  vec4 dirType;          // xyz=方向(point 不用), w=type(0=dir,1=point,2=spot)
  vec4 posRange;         // xyz=位置(dir 不用), w=range(0=无限)
  vec4 color;            // rgb=color × intensity
  vec4 spot;             // x=innerCos, y=outerCos(spot 用)
} lights[4];
```

- GLES 块名表加 `LightUBO→2`;Metal buffer(2);Vulkan set0 binding2。
- 阴影纹理 = **slot 7**(compare 采样器);纹理槽位表追加:7=shadowMap。

### 2b. ShadowPass(endScene 内,场景 pass 之前)

- 触发条件:阴影开(画质档 shadowMapSize>0 且未手动关)且存在方向光。
- shadow map:depth-only 目标,尺寸按画质档(High 2048/Mid 1024);渲染项共享
  ItemUBO 的 world;shadow.vert 计算 `lightViewProj × world × pos`,深度写出。
- bias:shader 端(常量 + slope-scaled),PCF 3x3。
- 取景:调用方传包围球 (center, radius) → 沿光源方向反推 eye,center 为焦点,
  正交包围半径 radius×1.2;`Renderer::setLightFraming(center, radius)`。
- SceneTarget 重建时机同现状;shadow map 在画质档/开关变化时重建。

### 2c. pbr_forward shader 改动

- direct 光照改多光源循环(0..lightCount):dir 无衰减;point 按 range 平滑衰减;
  spot 圆锥内外角平滑衰减。
- 方向光(首盏 dir 型)乘阴影系数:`sampler2DShadow` + PCF 3x3;
  worldPos → light 空间用 lightViewProj。
- IBL/SH 环境光路径不变(不受阴影影响)。
- unlit 管线不接入(无光照语义)。

### 2d. glTF loader

- 解析 `KHR_lights_punctual`:遍历场景节点取 TRS(方向=节点旋转×(0,0,-1),
  位置=平移),读 lights[] 定义(type/color/intensity/range/spot angles)→
  `ModelAsset.lights[]`;超过 4 盏截断并记警告。
- 无灯光的模型:loader 不造灯;渲染走 FrameUBO 默认 1 方向光(现状兼容)。

### 2e. C API + engine

- `rd_engine_add_dir_light(engine, dirX,Y,Z, r,g,b, intensity)`、
  `rd_engine_add_point_light(...pos, range, color, intensity)`、
  `rd_engine_add_spot_light(...pos, dir, innerDeg, outerDeg, ...)`、
  `rd_engine_clear_lights(engine)`。
- `rd_engine_set_shadow_enabled(engine, int)`(默认开;Low 档自动关)。
- 优先级:C API 灯非空 → 用 C API 灯;否则用 glTF 灯;否则默认 1 方向光。
- engine 在 load_gltf 后用模型包围球调 setLightFraming。
- 方向约定:C API 的 dir 与 FrameUBO.lightDir 现行约定一致(指向光源的方向,
  shader 内 dot(N, L) 直接使用);glTF 灯方向按规范取节点旋转 ×(0,0,-1) 后
  取反为该约定。

## 3. 画质联动

- QualityPreset 加 `uint32_t shadowMapSize`:High 2048 / Mid 1024 / Low 0(关)。
- setQuality 变化时重建 shadow map;Low 档跳过 ShadowPass。
- 既有 helmet_low golden 不受影响(Low 无阴影)。

## 4. 错误处理

- 深度纹理创建失败/caps 不满足:阴影自动关 + 记警告,不影响主渲染。
- LightUBO 超 4 盏:截断 + 警告。
- shadow shader 内嵌缺失:renderer init 失败(同现有一致)。

## 5. 测试与验证

| 层 | 内容 |
|---|---|
| 单测 | lights 解析(合成 gltf+KHR_lights_punctual)、包围球→lightViewProj、LightUBO static_assert(352B) |
| RHI 契约 | depth-only 渲染 + 硬件比较采样(三后端;GLES 随 Android 验证) |
| golden | helmet + 程序化地面 quad + 方向光阴影(双后端);Low 档(无阴影)回归不变 |
| 手动 | render_test --interactive 阴影;iOS/Android demo 截图 |

## 6. 实施顺序

1. RHI(1a/1b/1c + 契约测试)
2. loader 灯光解析
3. renderer(LightUBO + ShadowPass + shader)+ 阴影 golden
4. C API + 画质联动 + demo 接线
5. AGENTS.md 收尾
