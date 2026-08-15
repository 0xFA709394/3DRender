# 阶段二 b:PBR/IBL(uber-shader + 混合路径 IBL + DamagedHelmet golden)设计文档

- **日期**: 2026-08-15
- **状态**: 已评审
- **前置文档**: `docs/superpowers/specs/2026-08-13-renderer-framework-skeleton-design.md`(2a)、`docs/superpowers/plans/2026-08-09-p1-1-pbr-core.md`(旧 P1 计划,T7-T9 参考吸收)
- **目标**: renderer 实现 glTF metallic-roughness PBR + IBL,主机离屏渲染 DamagedHelmet 出 golden,Metal/Vulkan 双后端一致

## 1. 已确认决策

| 决策点 | 结论 |
|---|---|
| IBL 路径 | **混合**:Specular 预滤波 GPU(cube face/mip 渲染目标 + prefilter shader),SH9 irradiance 与 BRDF LUT CPU;HDR 浮点纹理格式归 2c(KTX2) |
| 材质范围 | 全 glTF MR:baseColor/MR 纹理+factor、normal map、emissive、occlusion、`KHR_texture_transform`;`KHR_materials_unlit` 走已有 unlit 路径 |
| 直接光 | 1 盏方向光(无阴影)+ IBL;多光源/阴影归 P2 |
| 顶点布局 | **stride 48 一步改到位**:pos3@0|normal3@12|tangent4@24|uv2@40(location 0/1/2/3);unlit shader uv location 2→3 同步 |
| shader 策略 | 单支 uber-shader,缺省贴图绑占位资源(无 shader 变体) |
| 环境 | 程序化"摄影棚"cubemap,亮度 ≤1.0(LDR 约束,与 RGBA8 输出匹配) |

### 非目标(YAGNI)

- 不做 HDR 环境/KTX2/浮点渲染目标(2c)
- 不做阴影/多光源/后处理/骨骼动画(P2)
- 不做 GPU 版 SH/LUT(重活只有 specular 预滤波;CPU 版精确可测,不重复造)
- 不做 clearcoat/sheen 等 KHR 扩展(P4)

## 2. 组件设计

### 2.1 材质与加载扩展(resource 层)

- `gltf_loader`:`MeshData` 增加 `MaterialData`:
  ```cpp
  struct MaterialData {
    ImageData baseColor;      float baseColorFactor[4] = {1,1,1,1};
    ImageData metallicRoughness; float metallicFactor = 1, roughnessFactor = 1;
    ImageData normal;         float normalScale = 1;
    ImageData emissive;       float emissiveFactor[3] = {0,0,0};
    ImageData occlusion;      float occlusionStrength = 1;
    float uvOffset[2] = {0,0}; float uvScale[2] = {1,1};  // KHR_texture_transform
    bool unlit = false;        // KHR_materials_unlit
  };
  ```
  全部内嵌纹理解码(URI 外链维持 2a 的警告跳过);KHR 扩展解析。
- `mesh_utils`(新):CPU 切线计算(标准 Lengyel 法;无 uv 或退化三角形跳过并记警告;手写性存 w 分量)。
- **顶点布局 stride 48**:pos3@0|normal3@12|tangent4@24|uv2@40,location 0/1/2/3;`gltf_loader` 直接产出 48B 交错;`unlit.vert` 的 uv 改 location 3(Renderer 管线 attributes 同步)。
- `MeshRenderResource`:上传全部 5 类纹理 + 各占位(1x1:白/黑/法线(128,128,255));MaterialData 随资源走(CPU 侧)。

### 2.2 环境(renderer/environment)

- `buildEnvCubemap(size=64)`:暗色地平渐变 + 两块柔光箱;RGB float 中间数据,RGBA8 输出。
- `projectToSH(faces)→ float[27]`(9 系数×RGB):CPU 网格采样投影,Ã 折叠(π, 2.0944×3, 0.7854×5);`evalSH` 供测试。
- `integrateBrdfLut(32)`:GGX 积分(NdotV×roughness 网格,64 Hammersley 样本),RG float 输出 → `R32G32_FLOAT` 纹理上传,**nearest 采样**(规避 GLES float 线性过滤限制,旧计划既定对策)。
- GPU specular 预滤波:`prefilterSpecular(dev, envTex, outCube)`:
  - 环境 cubemap 上传为采样纹理;
  - prefilter cubemap 创建为 `Sampled|RenderTargetAttachment` RGBA8,5 级 mip(64→1);
  - `prefilter.frag`:全屏三角形,per-fragment 重建视线方向(由 UBO 传入 face 基向量),GGX 重要性采样 64 样本,roughness = mip/(mipCount-1);
  - 逐 face × mip 经 `OffscreenTargetDesc.colorFromTexture` 渲染(init 期一次性,不占帧 prepass 钩子)。

### 2.3 pbr uber-shader 与渲染集成(renderer 层)

- `shaders/pbr_forward.vert`:location 0/1/2/3(pos/normal/tangent/uv);输出 world pos、TBN、uv;UBO per-item{mvp, world}。
- `shaders/pbr_forward.frag`:
  - 材质:baseColor(MR 模型)、metallic/roughness(B/G 通道)、normal map(TBN)、emissive、occlusion(R 通道)、uvTransform;
  - 直接光:1 方向光,Lambert + GGX specular(与 IBL 同 NDF 近似);
  - IBL:diffuse = SH(normal) × albedo;specular = prefilterLod(roughness)× (F0·A + B)(split-sum,LUT);
  - 纹理槽位:0=baseColor,1=MR,2=normal,3=emissive,4=occlusion,5=prefilterCube,6=brdfLUT。
- UBO 分层:per-frame{frameUBO: viewProj, cameraPos, lightDir, lightColor, SH[27]}+ per-item{itemUBO: mvp, world, factors, uvTransform}(256B 步进沿用)。
- `Renderer` 扩展:
  - `Environment` 子对象:init 期 buildEnvCubemap → 上传 envTex、projectToSH、生成 LUT 纹理、GPU 预滤波出 prefilterCube;
  - PBR 管线(depthTest 开)与 unlit 管线并存,`MeshRenderable` 按材质 unlit 标记选管线;
  - 槽位不足的资源绑占位纹理(uber-shader 无变体)。

### 2.4 帧流程变化

仅 init 期增加"环境生成"阶段(buildEnv → 上传 → SH/LUT → GPU 预滤波);每帧流程与 2a 相同,per-frame UBO 在 beginScene 填。

## 3. 测试策略

| 层级 | 内容 | 通过标准 |
|---|---|---|
| 单测 | 切线(正交性/手性);loader 材质(DamagedHelmet 5 纹理+factor+unlit 标记);env cubemap 尺寸;SH 常亮 DC 方向无关;LUT 端点(rough=0→A≈1/B≈0) | 断言通过 |
| 契约 | GPU 预滤波:mip0 readback ≈ 原环境(低 roughness 近原图),高 mip 方差显著小于 mip0(模糊验证) | Metal/Vulkan 通过 |
| golden | **DamagedHelmet 固定机位 PBR+IBL** | 双后端容差比对;RD_UPDATE_GOLDENS 生成后像素分析核对 |
| 回归 | 既有 72 项 | 全绿 |

## 4. 提交节奏(6 个 commit)

1. `feat(resource): mesh_utils 切线 + loader 材质扩展(KHR 扩展)+ 顶点布局 48B`
2. `feat(renderer): environment 程序化环境 + SH + BRDF LUT(CPU)+ 单测`
3. `feat(renderer): GPU specular 预滤波(prefilter shader + cube targets)+ 契约测试`
4. `feat(renderer): pbr uber-shader + Renderer PBR/Environment 集成`
5. `test(renderer): DamagedHelmet golden 双后端 + render_test 验证`
6. `docs: AGENTS.md + 阶段二 b spec/plan 同步`

## 5. 对 2c/P2 的预埋

- 预滤波管线抽象为 `Environment::prefilter`,HDR 时代只换纹理格式(RGBA16F)与 shader 精度;
- per-frame UBO 的 lights 段预留多光源扩展;
- prepass 钩子仍未占,留给 P2 阴影图;
- 48B 布局与 uber-shader 槽位约定即 2c KTX2 的接入面。

## 6. 风险与对策

| 风险 | 对策 |
|---|---|
| GPU 预滤波与 CPU 参考实现视觉偏差 | 契约测试双断言(低 mip 近似原图/高 mip 方差下降)+ golden 双后端一致兜底 |
| GLES fragment 预滤波循环性能 | 样本数 64 恒定 + init 一次性;GLES 用同 shader(GLSL ES 310 离线转译) |
| 48B 布局改动波及 unlit 路径 | 同步改 unlit.vert location 与 Renderer attributes;BoxUnlit golden 作回归 |
| DamagedHelmet 无切线数据需 CPU 计算 | mesh_utils 单测 + loader 集成;失败降级(跳切线,normal map 近似) |
