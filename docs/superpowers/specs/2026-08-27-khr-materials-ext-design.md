# P4-A 设计:KHR 材质扩展四件套(clearcoat / sheen / specular / ior)

日期:2026-08-27
状态:已确认(用户审阅通过)
前置:总设计 docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md;
P2 全部完成(阴影/后处理/骨骼动画/拾取/性能基准),P4 已落地视锥剔除/自动实例化/包体积

## 0. 目标与范围

里程碑 A(P4 渲染能力扩充第一批):glTF 材质模型扩展四件套。

1. **KHR_materials_clearcoat**:清漆层(车漆/亮面包装),独立 GGX 瓣 +
   独立法线/粗糙度纹理,基层按菲涅尔衰减。
2. **KHR_materials_sheen**:织物绒面(服装电商),Charlie 分布 + Neubelt
   可见性(纯解析),能量守恒用无 LUT 解析拟合。
3. **KHR_materials_specular**:介质高光强度/颜色控制,改造 f0 与漫反射能量。
4. **KHR_materials_ior**:折射率 → 介质 f0(独立于 specular,亦可单独生效)。

非目标:transmission/volume(里程碑 B,需场景颜色缓冲折射)、
morph targets/Draco(里程碑 C,几何管线)、clearcoat 的 IBL 专用 LUT
(复用现有 brdfLut,f0=0.04 近似)、sheen LUT(用解析拟合替代)。

**零回归硬约束**:材质不含扩展时(全部因子默认值)渲染结果与现状
逐像素一致;现有 golden 全量不动。

## 1. 现状盘点(扩展点)

- 纹理槽 9/9 已满(slot0..8:baseColor/MR/normal/emissive/occlusion/
  prefilterCube/brdfLut/shadow/spotShadow),caps `max_texture_slots=9`。
- uniform 槽 4/4 已满(slot0=Frame/slot1=Item/slot2=Light/slot3=Joint);
  ItemUBO 256B 已排满(3×mat4 + 4×vec4)。
- cgltf 原生解析四扩展(has_clearcoat/has_sheen/has_specular/has_ior),
  loader 侧为纯字段读取;纹理走现有 decodeImage(KTX2/PNG/JPEG 通吃)。
- `pbr_forward.frag` 被 pbr/blend/skinned 三管线复用;实例化走独立的
  `pbr_forward_instanced.frag`(ItemUBO 为 items[64] 数组变体)。
  **frag 改动面 = 这 2 个文件**。

## 2. Loader(resource/gltf_loader)

`MaterialData` 新增字段(默认值=零操作语义):

```cpp
// KHR_materials_clearcoat
ImageData clearcoat, clearcoatRough, clearcoatNormal;
float clearcoatFactor = 0.0f, clearcoatRoughnessFactor = 0.0f;
float clearcoatNormalScale = 1.0f;
// KHR_materials_sheen
ImageData sheenColor, sheenRough;
float sheenColorFactor[3] = {0, 0, 0};
float sheenRoughnessFactor = 0.0f;
// KHR_materials_specular
ImageData specularColorTex, specularTex;   // specular 因子在 A 通道
float specularFactor = 1.0f;
float specularColorFactor[3] = {1, 1, 1};
// KHR_materials_ior(独立于 specular 生效)
float ior = 1.5f;
```

`readMaterial` 按 cgltf 字段直读;纹理全部经 `decodeImage`(内嵌
buffer_view / 外链 URI / KTX2 转码路径不变)。

默认值论证(零回归依据):clearcoatFactor=0 → 清漆瓣乘以 0;
sheenColorFactor=(0,0,0) → sheen 瓣为黑;specularFactor=1 /
specularColorFactor=(1,1,1) / ior=1.5 → f0=0.04 与现状一致。

## 3. GPU 资源(resource/mesh_render_resource)

`MeshGpuData` 新增 7 个 TextureHandle(clearcoat/clearcoatRough/
clearcoatNormal/sheenColor/sheenRough/specularColor/specular);
缺省统一绑现有 `fallbackWhite_`(specular 因子默认 1 × 白图 = 1,符合
spec 默认;其余因子默认 0 压制贡献)。

## 4. ItemUBO 扩 304B(stride 512B)

uniform 槽已满 → 扩 ItemUBO,追加 3 个 vec4(48B):

```cpp
float ext0[4];  // x=clearcoatFactor y=clearcoatRoughness z=clearcoatNormalScale w=specularFactor
float ext1[4];  // xyz=sheenColorFactor w=sheenRoughnessFactor
float ext2[4];  // xyz=specularColorFactor w=ior
```

- shader 块 256B → 304B(GLES 16KB 块上限内);**stride 256 → 512**
  保持 256B 动态偏移对齐(GLES `GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT`
  与 Vulkan `minUniformBufferOffsetAlignment` 真机普遍 256);
  UBO 缓冲 128 槽 × 512B = 64KB(纯缓冲,无块大小问题)。
- **实例化变体**:`pbr_forward_instanced.frag` 的 `Item` 结构体补 padding
  到 512B(std140 数组元素 stride 须匹配 CPU 槽距),`items[64] → items[32]`
  (块 = 32×512 = 16KB,恰在 GLES 保证线,与现状同量级);
  自动实例化分组上限 64 → 32(超组拆分,逻辑不变)。
- `kUboStride`/`kMaxItemSlots`(renderer.h)同步;`bindUniformBuffer` 的
  range 参数 256 → sizeof(ItemUBOData)。

## 5. 纹理槽 9 → 16(caps 同步)

slot 分配(恒绑定占位,同 shadow map 模式):

| slot | 纹理 | Vulkan binding | 内容通道 |
|---|---|---|---|
| 9 | clearcoat | 13 | R=clearcoat 强度 |
| 10 | clearcoatRough | 14 | G=clearcoat 粗糙度 |
| 11 | clearcoatNormal | 15 | RGB 法线 |
| 12 | sheenColor | 16 | RGB 颜色 |
| 13 | sheenRough | 17 | A 通道粗糙度 |
| 14 | specularColor | 18 | RGB |
| 15 | specular | 19 | A=specular 因子 |

- Vulkan:set0 binding 13..19 加 7 个 sampler,描述符布局/池按 16 纹理槽扩。
- GLES:sampler 名字表 +7 条(texClearcoat→9 等);绑定点 9..15。
- Metal:texture index 9..15 直给(参数缓冲上限富余)。
- `caps.max_texture_slots` 9 → 16(三后端 + rhi_constants.inc.h 注释)。
- **风险**:恰压 GLES 3.0 保证的 16 纹理单元线(`GL_MAX_TEXTURE_IMAGE_UNITS`
  ≥16);真实设备普遍 32+,接受并在 AGENTS.md 注明。
- `MeshRenderable::record` 三条 PBR 路径(pbr/skinned/blend)各 +7 次
  bindTexture(恒绑定占位,与 env/shadow 同模式)。

## 6. Shader 分层 BRDF(2 个 frag)

按 Khronos glTF-Sample-Viewer 分层模型重排光照组合(uber shader,
无变体;**均匀分支**:扩展因子全默认时跳过对应纹理采样与瓣计算):

1. **ior+specular 改造 f0**(直接光与 IBL 共用):
   `f0_介质 = ((1-ior)/(1+ior))² × specularColorFactor×tex.rgb`;
   `specularWeight = specularFactor×tex.a` 缩放介质高光,
   漫反射能量扣 `(1 - specularWeight × F)`;金属部 f0=baseColor 不变。
2. **直接光循环内**逐光源叠加:
   - sheen 瓣:Charlie 分布 D + Neubelt 可见性 V(解析数值拟合公式,
     按 KHR_materials_sheen 附录),`sheenColor × NdotL`。
   - clearcoat 瓣:GGX(F0=0.04,独立 clearcoatNormal/clearcoatRoughness),
     `ccFactor × NdotL_cc`。
3. **IBL**:base 不变(f0 改造自动生效);
   sheen IBL = prefilter@sheenRough × sheenColor × 解析能量拟合;
   clearcoat IBL = prefilter@coatNormal/coatRough × (0.04×lut.x+lut.y) × ccFactor。
4. **分层衰减**(spec 规定顺序):
   `base *= (1 - sheenScaling × max3(sheenColor))`;
   最终 `color = mix(基层, clearcoat层, ccFactor × Fcc)`。
5. sheen 能量守恒为**无 LUT 解析拟合**,与参考 viewer 的 LUT 版存在微小
   数值差异;golden 自生成自洽,差异记入文档。

`pbr_forward.frag` 覆盖 pbr/blend/skinned 三路径;`pbr_forward_instanced.frag`
同步相同改动(经 `iu.items[vItem].extN` 访问)。

## 7. 画质/选项门控(与 shadow 同模式)

- `options.json` 新增 `render.ext_materials`(bool,默认 true)——
  命令总线自动获得 set/toggle。
- `QualityPreset` 新增 `bool extMaterials`(High/Mid=true,Low=false)。
- 生效语义 = **与关系**:`effective = option && qualityPreset.extMaterials`。
- 实现零变体:门控发生在 ItemUBO 填充处——关闭时强制写默认因子,
  shader 走均匀分支跳过路径;纹理槽仍恒绑定占位(描述符布局静态)。
  关闭后每 draw 成本 ≈ 几次 uniform 比较。

## 8. 测试

- `scripts/fetch_assets.sh` 增加 Khronos 官方模型:
  ClearCoatTest、SheenClothPillow、SpecularTest(assets/ 不入库)。
- golden(`RD_GOLDEN_TEST` 宏 + SSIM 双后端):
  `clear_coat_test` / `sheen_pillow` / `specular_test` 各一场景。
- loader 单测:最小 gltf JSON 构造四扩展字段,验证因子/纹理路径解析
  (含缺省默认)。
- **回归门槛:现有 golden 全量不变**(默认值=零操作,SSIM 应无损级)。
- 演示场景 `material_ext_gallery`(3 模型并排,render_test --scene /
  interactive 直驱,交互查验用)。

## 9. 文档

AGENTS.md 更新:纹理槽约定 9→16(逐槽清单)、ItemUBO 512B stride/304B 块、
实例化分组上限 32、分层 BRDF 结构、四扩展清单与默认值零操作语义、
`render.ext_materials` 选项与画质档与关系。

## 10. 里程碑规划(A→B→C)

| 批次 | 内容 | 关键改动面 |
|---|---|---|
| **A(本 spec)** | clearcoat/sheen/specular/ior | loader + ItemUBO + 纹理槽 + 2 frag |
| B | transmission/volume | SceneTarget 颜色缓冲折射采样 + volume 吸收 |
| C | morph targets + Draco | 顶点管线(targets 混合)+ draco 解码依赖 |

每批次独立 spec → plan → 实现 → 验证循环。
