# P4-B 设计:KHR transmission / volume(透射 + 体积吸收)

日期:2026-08-31
状态:已确认(用户逐节审阅通过)
前置:总设计 docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md;
P4-A 完成(docs/superpowers/specs/2026-08-27-khr-materials-ext-design.md)

## 0. 目标与范围

里程碑 B(P4 渲染能力扩充第二批):glTF 透射/体积材质两件套。

1. **KHR_materials_transmission**:玻璃/液体类透射材质。透射光来自**场景颜色
   缓冲的折射采样**(屏幕空间),粗糙度驱动 mip 级模糊(毛玻璃)。
2. **KHR_materials_volume**:体积吸收——Beer-Lambert 衰减
   (attenuationColor/attenuationDistance)+ 厚度(thicknessFactor/thicknessTexture)
   驱动折射偏移量与吸收深度。

非目标:KHR_materials_dispersion(色散,较新扩展,另批)、morph/Draco(里程碑 C)、
互相折射/自折射(见 §8 已知限制)、真实多次折射事件(屏幕空间近似)。

**零回归硬约束**(同 P4-A):材质无 transmission 扩展(transmissionFactor=0)时
渲染结果与现状逐像素一致;现有 golden 全量不动;门控关闭或无 transmission 项时
不发生 pass 拆分。

## 1. 核心流程:场景 pass 拆两段

当前 `endScene`:阴影 pass → 单段场景 pass(opaque 先、blend 远→近)→ 后处理链。
改为(**仅当存在可见 transmission 项且门控开**):

```
阴影 pass(不变)
场景 pass A: beginRenderPass(scene, clear)
             → 天空盒 + 全部 opaque 项(含实例化分组)
             → endRenderPass(MSAA resolve 完成)
拷贝链:     blit 全屏 pass 把 scene 颜色拷入 transmissionTex mip0
             → generateMipmaps(transmissionTex)   [复用现有三后端实现]
场景 pass B: beginRenderPass(scene, load)         [RHI 新增 load 语义, §4]
             → transmission 项(远→近, 采样 transmissionTex 折射)
             → blend 项(远→近, 从 pass A 挪到 B)
             → endRenderPass
后处理链(不变)
```

关键次序论证:

- **拷贝时机在 opaque 后、transmission 前**:transmission 纹理只含 opaque 内容
  (天空盒已画,算背景),这是 three.js/Filament 的标准近似。
- **blend 项挪到 pass B 末尾**:blend 内容不进折射纹理(接受为限制),但必须
  最后画才能正确叠在玻璃前/后。
- **pass B 必须 load 颜色+深度**:pass A 的 opaque 结果与深度都要保留(深度保证
  玻璃被 opaque 墙正确遮挡)→ 触发 RHI load 语义扩展(§4)。
- **transmission 项之间**远→近排序;彼此折射不可见(纹理里没有对方)——与
  three.js/Filament v1 同限制。
- **门控关闭/无 transmission 项**:单段 pass 原样走(零回归)。

transmission 项**排除自动实例化分组**(同 blend 项现有处理);蒙皮 transmission
走 skinned 管线进 pass B;阴影 pass 中 transmission 项按 opaque 正常投影。
Unlit 材质不参与 transmission(分类时按 opaque 处理,spec 语义亦如此)。

## 2. Loader(resource/gltf_loader)

cgltf 原生支持两扩展(has_transmission/has_volume),`MaterialData` 追加
(默认值=零操作语义):

```cpp
// KHR_materials_transmission
ImageData transmissionTex;              // R 通道=透射强度
float transmissionFactor = 0.0f;        // 0=无透射 → 零操作
// KHR_materials_volume
ImageData thicknessTex;                 // G 通道=厚度
float thicknessFactor = 0.0f;           // 0=薄壁近似(无体积)
float attenuationColor[3] = {1, 1, 1};
float attenuationDistance = 0.0f;       // 0 哨兵 = spec 默认 +∞(无吸收)
```

纹理全走现有 decodeImage(内嵌 buffer_view / 外链 URI / KTX2 转码路径不变)。

默认值论证(零回归依据):transmissionFactor=0 → 透射瓣不生效、pass 不拆分;
thicknessFactor=0 → 薄壁;attenuationColor=(1,1,1) 且 distance=0(∞) → 无吸收。

## 3. GPU 资源与 UBO 扩展

### 3.1 MeshGpuData(resource/mesh_render_resource)

+2 纹理句柄:transmissionTex(slot17, R)、thicknessTex(slot18, G);
缺省统一绑现有 `fallbackWhite_`(因子 0 压制贡献,符合 spec 默认)。

### 3.2 ItemUBO 扩 336B(stride 512B 不变)

追加 2 个 vec4(48B→64B 增量,304→336B):

```cpp
float ext3[4];  // x=transmissionFactor y=thicknessFactor z=attenuationDistance(0=∞) w=0
float ext4[4];  // xyz=attenuationColor w=0
```

- 块 304B→336B,槽距 512B 不变(padding 富余吸收);kItemUboSize 同步,
  renderer.cpp 的 static_assert 联动。
- 实例化变体:Item 结构体数据段 304→336B,`_pad[13]→_pad[11]`(总 512B 不变),
  `items[32]` 不变。
- 读取 ext3/ext4 的只有 2 个 frag;vert 系声明块不变(读子集合法,绑定位
  range 已按槽距覆盖)。

### 3.3 FrameUBO 扩 272B

256B 已满 → 追加 1 个 vec4:

```cpp
float transmissionParams[4];  // x=1/transmissionTexW y=1/transmissionTexH
                              // z=maxLod(mip 链级数-1) w=0
```

屏幕 UV 与 roughness→mip 映射所需,per-frame 全局量。声明 FrameUBO 的只有
`pbr_forward.frag`/`pbr_forward_instanced.frag` 两文件;bind range 256→272 同步
(renderer.cpp 含 skybox 等全部 FrameUBO 绑定点核查)。

### 3.4 纹理槽 16→19 与分离采样器模型

新槽位:**16=transmissionScene**(全局场景拷贝,Renderer 按 pass 绑定;pass A 绑
1×1 占位,pass B 绑真图)、**17=transmission**(R)、**18=thickness**(G)。

**硬约束(实测)**:MoltenVK `maxPerStageDescriptorSamplers=16`(Apple M2 Pro
实测;Metal iOS sampler 状态上限同为 16,P4-A 已靠折返 hack 恰好蹭满)。
直接 +3 combined sampler 会让 Vulkan 后端超限。纹理(image)维度宽裕
( sampled images=256 / Metal 纹理 31+)。

**方案(已确认:全量分离改造)**:仅 `pbr_forward.frag` +
`pbr_forward_instanced.frag` 两文件,15 张普通 2D 纹理(baseColor/MR/normal/
emissive/occlusion/clearcoat×3/sheen×2/specular×2 + transmission/thickness/
transmissionScene)全部声明为 `texture2D`,共享 1 个 `smpMat`(与现有 mesh
sampler 同状态:线性+mipmap);prefilterCube/brdfLut/shadow×2 采样器状态特殊
且量少,**保持 combined**。

改造后 sampler 描述符 **16→5**(smpMat + cube + lut + shadow×2):

- **Vulkan**:描述符布局改混合类型——15 个 SAMPLED_IMAGE + 1 个 SAMPLER +
  4 个 combined(cube/lut/shadow×2)+ UBO;池/布局按新形状。bindTexture(slot,
  tex, sampler) 对分离槽只写 image 描述符(sampler 参数被共享语义取代)。
- **Metal**:spirv-cross 对分离声明天然输出独立 texture index + 少量 sampler
  index(5 个,全部 ≤15);**折返 hack(fixup_msl_samplers.cmake + bindTexture
  映射)可删除**;纹理索引 = binding 号(4..22),≤31 上限内。
- **GLES**:GLSL ES 无分离类型,spirv-cross 自动把 `sampler2D(tex, smp)` 合并回
  combined sampler2D;名字表 +3(texTransmission→16 等);19 纹理单元超 ES3.0
  保证的 16 线(真机普遍 32+,与 P4-A 同一接受逻辑,记入文档)。
- **验证前置**:spirv-cross 的 GLSL ES 合并行为 = plan 阶段第一个任务(编译
  冒烟 + 三后端 golden 零回归),不通过则回落最小腾位方案。

`caps.max_texture_slots` 16→19(三后端 + rhi_constants.inc.h 注释)。

## 4. RHI 扩展:load 语义

`beginRenderPass(target, clear)` 恒清屏 → 增加 load 语义:
`beginRenderPass(target, clear, loadContent=false)`(默认 false,三后端零回归)。

- **Metal**:loadAction = Load/Clear;scene target 深度 storeAction 由
  DontCare→Store(pass A→B 深度要保留;ensureSceneTarget 目标加 preserve 标记,
  仅 scene target 设置,其余 pass 不受影响)。
- **Vulkan**:attachment loadOp 变体(render pass 对象按 loadOp 键扩充缓存;
  storeOp 本已 Store)。
- **GLES**:load 时跳过 glClear(FBO 附着天然持久)。

**transmissionTex 目标管理**:`ensureTransmissionTarget(w, h, format)`——
格式/尺寸跟随 SceneTarget(HDR=R16F / LDR=RGBA8,非 MSAA,全 mip 链);
尺寸/画质档变化时重建(与 ensurePostTargets 同生命周期模式)。

拷贝实现:复用现有 blit 全屏 pass(scene resolve 纹理 → transmissionTex mip0,
GLES v 翻转由 BlitUBO.params.x 吸收)→ `generateMipmaps()`(三后端已实现;
GLES RGBA16F 的过滤性由 hdr_render_target caps 连带保证,plan 阶段实测验证,
异常则回落 blit 降采样链)。

**屏幕 UV 翻转**:pass B 采样 transmissionTex 的 fragCoord 原点差异(GLES
左下 vs Metal/Vulkan 左上)复用 post 链 vFlip 约定统一吸收;写入端(blit 拷贝)
已按 composite 同款约定,采样端在 plan 阶段于 GLES 后端核对一次方向。

## 5. Shader:透射瓣(2 个 frag)

`pbr_forward.frag` + `pbr_forward_instanced.frag` 按 Khronos
glTF-Sample-Viewer 公式移植,**均匀分支**(transmissionFactor > 0 才走;
默认因子全跳,零纹理采样零回归):

```glsl
// ① 屏幕折射 UV:薄壁(无 volume)小偏移;有厚度时按 refract(-V,N,1/ior) 投影
vec2 screenUV = gl_FragCoord.xy * transmissionTexel;
float thickness = thicknessFactor * texture(sampler2D(texThickness, smpMat), uv).g;
vec3 refr = refract(-V, N, 1.0/ior);
vec2 offset = refr.xy * thickness * transmissionTexel.xy * 投影系数;
// ② mip 级 = roughness 映射(毛玻璃模糊)
float lod = perceptualRoughness * maxLod;
vec3 transmitted = textureLod(sampler2D(texTransmissionScene, smpMat),
                              clamp(screenUV + offset, 0.0, 1.0), lod).rgb;
// ③ volume 吸收:Beer-Lambert(attenuationDistance>0 时)
vec3 sigma = -log(attenuationColor) / attenuationDistance;
transmitted *= exp(-sigma * thickness);
// ④ 组合:透射替换介电层漫反射位(菲涅尔权重下),金属瓣不变
diffuse *= (1.0 - transmissionFactor);
color = mix(color, transmitted * (1.0 - F0介电), transmissionFactor)
        + 高光/金属瓣(现有分层结构不变);
```

- HDR 模式采样 tonemap 前线性场景色(拷贝发生在 post 链之前,天然正确)。
- `smpMat` 共享采样器(§3.4),shader 内 clamp UV 抵消其 repeat wrap 差异。
- 与 P4-A 分层 BRDF 的叠加顺序:透射作用在介电基层(diffuse 位),clearcoat/
  sheen 瓣不受影响;specular/ior 改造后的 f0 继续生效(透射入口菲涅尔用同 f0)。
- 具体公式细节(log 基、投影系数、F 权重)plan 阶段对照 glTF-Sample-Viewer
  源码逐项移植,以 golden 自生成自洽为准。

## 6. 分类/排序/门控

### 6.1 分类与排序(endScene)

- **transmission 项判定**:`transmissionFactor > 0 && 门控开`(纹理乘因子,
  因子 0 即压制;门控关时 ItemUBO 强制写默认因子——同 ext_materials 模式,
  此时按 opaque 分类,pass 不拆分)。
- 三分区排序:**opaque(含实例化组)→ transmission(远→近,排除实例化分组)→
  blend(远→近)**;pass A 渲 opaque,pass B 依次渲 transmission 与 blend。
- transmission + alphaBlend 组合:进 pass B,按材质选 blend 管线(现有
  pbr/blend 两管线复用,仅 pass 归属变化)。
- transmission + skinned:skinned 管线进 pass B;阴影 pass 一切如常。
- transmission 项不参与视锥剔除豁免(与 opaque 同规则剔除)。

### 6.2 选项/画质门控(与 ext_materials 同模式)

- `options.json`:`render.transmission`(bool,默认 true)——命令总线自动获得
  set/toggle。
- `QualityPreset.transmission`:High/Mid=1,Low=0。
- 生效 = **与关系**(option && preset);关闭时 ItemUBO 填默认因子 + pass 不拆分,
  零拆分开销。

## 7. 测试

| 类别 | 内容 |
|---|---|
| golden 双后端 | `ext_transmission`(TransmissionTest)、`ext_transmission_roughness`(TransmissionRoughnessTest)、`ext_volume`(AttenuationTest)、`ext_amber`(MosquitoInAmber,transmission+volume 综合)——缺失自动 skip |
| RHI 契约 | LoadOp:渲三角 → load 重开 → 再画 → 两层共存(三后端);scene 深度跨 pass 保留 |
| loader 单测 | 最小 gltf JSON 构造两扩展字段:因子/纹理/默认值(含 ∞ 哨兵) |
| 门控语义 | `set render.transmission false` → 与关闭基线逐像素一致(零操作) |
| 分离采样器 | spirv-cross GLSL ES 合并实测(plan 首任务)+ 三后端 golden 零回归 |
| 演示场景 | `transmission_gallery`(棋盘格上玻璃球阵列:清玻璃/毛玻璃/彩色吸收)+ interactive 直驱 |

fetch_assets.sh 收录 TransmissionTest / TransmissionRoughnessTest /
AttenuationTest / MosquitoInAmber(assets/ 不入库)。

## 8. 已知限制(记入文档)

1. transmission 纹理只含 opaque 内容:transmission/blend 物体不互相折射
   (three.js/Filament v1 同限)。
2. 折射偏移为屏幕空间近似(单次折射事件)。
3. Low 档退化为 opaque 渲染(参考实现同语义降级)。
4. GLES 19 纹理单元超 ES3.0 保证线(真机普遍 32+)。

## 9. 文档

AGENTS.md 更新:槽位 16..18 清单、分离采样器模型与 sampler 数(16→5)、
ItemUBO 336B/FrameUBO 272B、pass 拆分流程与 LoadOp、门控、已知限制、
Metal 折返 hack 删除。

## 10. 里程碑规划(A→B→C)

| 批次 | 内容 | 状态 |
|---|---|---|
| A | clearcoat/sheen/specular/ior | 完成 |
| **B(本 spec)** | transmission/volume | 设计确认 |
| C | morph targets + Draco | 待启动 |
