# P4-C 设计:glTF Morph Targets(形变目标,GPU 纹理方案)

日期:2026-09-05
状态:已确认(用户逐节审阅通过)
前置:总设计 docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md;
P4-B 完成(docs/superpowers/specs/2026-08-31-p4b-transmission-volume-design.md)

## 0. 目标与范围

里程碑 C(P4 渲染能力扩充第三批):glTF morph targets(blend shapes 形变目标)。

1. **解析**:`mesh.primitives[].targets[]`(POSITION/NORMAL 增量)+ 初始 `weights`
   + `extras.targetNames`。
2. **动画**:glTF animation 的 weights 通道进现有 Animator(play/crossfade/pause
   免费复用);另加手动 C API 权重接口(交互/演示)。
3. **渲染**:GPU 形变——deltas 打包 RGBA16F 纹理,顶点着色器 texelFetch 取增量
   按权重累加;支持 morph 单独与 morph+蒙皮组合。

非目标:Draco(几何压缩,另批)、TANGENT 增量(忽略+日志,见 §8)、morph 驱动的
实例化(排除分组,同蒙皮惯例)、逐 target 命名 UI(仅保留数据)。

**零回归硬约束**(同 P4-A/B):材质无 morph(或全零权重)时渲染结果与现状逐像素
一致;现有 golden 全量不动;无 morph 项时不产生任何新绑定/开销。

## 1. 核心方案:GPU morph 纹理

**选型结论**(已论证排除):
- 顶点属性方案不可行——glTF 一致性要求 ≥8 目标,8×2 属性 + 基础 6 > GLES3
  保证的 16 属性上限。
- CPU 每帧重写 VBO——每帧带宽(10k 顶点≈480KB/帧)移动端不友好,且与
  "GPU 蒙皮 + 双后端像素一致"的既有路线相悖。

**采用**:deltas 打包为 **RGBA16F 纹理**(宽=max(顶点数,1),高=目标数×2;
texel=(dx,dy,dz,pad);双行/目标=POSITION/NORMAL 增量分离),顶点着色器按 `texelFetch(texMorph, ivec2(gl_VertexIndex, i), 0)`
取第 i 目标增量 × 权重累加到基础 POSITION/NORMAL。

- ES3.0 保证 ≥16 vertex texture image units;texelFetch 无过滤,不依赖浮点
  线性过滤扩展 → 三后端可用。
- 内存 ≈ 顶点数 × 目标数 × 2 行 × 8B(10k×8=1.28MB,可接受)。
- 权重每帧只更新 UBO(几十字节),零纹理/VBO 重上传。

## 2. Loader(resource/gltf_loader)

`MeshData` 追加:

```cpp
// ---- morph targets(P4-C)----
bool morph = false;                    // 有 ≥1 目标(管线选型用)
std::vector<float> morphPosDeltas;     // target 主序:每目标连续 [dx,dy,dz]×vertexCount
std::vector<float> morphNormalDeltas;  // 同上(NORMAL 增量;无该增量的目标填 0)
std::vector<float> morphWeights;       // 初始权重(动画/手动改前的基础值)
std::vector<std::string> morphTargetNames;  // extras.targetNames(元数据)
```

解析要点:

- 增量读 accessor **经 `cgltf_accessor_read_float`**(sparse accessor 自动展开;
  直接读 bufferView 会漏 sparse 数据——Khronos FaceCap 等模型用 sparse)。
- **上限 8 目标**(glTF 一致性最低要求):超出截断 + RD_LOGW 告警。
- TANGENT 增量存在时忽略 + 日志(v1 限制)。
- 非索引图元/strip 分解、法线 flat 生成等现有后处理在 morph 解析**之后**不变
  (morph 增量按最终顶点序对齐——解析顺序:先构顶点/索引,再逐 target 读增量)。
- glTF 节点世界变换烘焙(非蒙皮)对 morph 顶点同样施加(pos 基础值烘焙;
  增量是相对量,平移不影响,线性变换下方向增量按 mat3 烘)——与现有蒙皮跳过
  烘焙不同,**morph+非蒙皮**项按现有非蒙皮路径烘焙(morph 不豁免)。

## 3. 资源层(resource/mesh_render_resource)

`MeshGpuData` 追加:

```cpp
TextureHandle morphTex;   // RGBA16F:宽=顶点数,高=目标数×2(无 morph=无效句柄)
bool morph = false;       // 快速判断(record 选管线;等于 !morphDeltas.empty())
```

- 上传:增量打包 **RGBA16F 纹理**(宽=max(顶点数,1),高=目标数×2;
  偶数行=POSITION 增量,奇数行=NORMAL 增量;texel=(dx,dy,dz,pad);宿主侧逐行补
  pad 成 RGBA,数据量 = verts×targets×2×8B)。
- 销毁/有效性检查/占位惯例与现有纹理一致(无 morph 不建纹理,record 不绑槽)。

## 4. UBO 与纹理槽

### ItemUBO 336→368B(槽距 512 不变)

```cpp
float ext5[4];  // morph weights[0..3]
float ext6[4];  // morph weights[4..7]
// 目标数 = ext4.w(原为 0 空位;0=无 morph → 零操作)
```

- 块 = 23 vec4 = 368B;实例化 Item `_pad[11]→_pad[9]`(32 vec4=512B 对齐)。
- 零权重 + ext4.w=0 → shader 循环零迭代 = 逐像素零操作 ✓。

### 纹理槽 19(caps max_texture_slots 19→20)

- **slot19 = texMorph**(binding **24** = SAMPLED_IMAGE;slot19 是对
  "binding=slot+4" 约定的**一次性例外**——23 已被 smpMat 占用,Vulkan pbr 布局
  与 descriptorSetFor 的 slot→binding 映射对 19 特判到 24)。
- **复用 smpMat**(不新增 SAMPLER 描述符,sampler 总数保持 5):
  - texelFetch 不经采样器滤波;Metal `texel.read()` 根本不消费 sampler;
    Vulkan OpImageFetch 同理(构造 combined 需描述符合法,smpMat 恒绑定 ✓)。
  - GLES 完备性:纹理格式用 **RGBA16F**(ES3 核心可滤波,配 linear smpMat
    完备;texelFetch 精确取值不受滤波影响——three.js morph 纹理同方案)。
  - 量化代价:16F 尾数 10 位,增量幅度下误差 ~1e-5 量级(记入 §9 已知限制)。
- GLES:spirv-cross 合并名 `SPIRV_Cross_CombinedtexMorphsmpMat` 入
  kSamplerTable(slot 19)。
- blit/post/unlit/shadow 族布局不动(combined 布局不含 24);Metal/GLES 采样器
  映射零改动(无新分离 sampler)。

## 5. Shader(仅顶点阶段,frag 零改动)

4 个新 vert = 复制现有对应物 + 形变块:

```
shaders/pbr_forward_morph.vert             ← pbr_forward.vert
shaders/pbr_forward_morph_skinned.vert     ← pbr_forward_skinned.vert
shaders/shadow_depth_morph.vert            ← shadow_depth.vert
shaders/shadow_depth_morph_skinned.vert    ← shadow_depth_skinned.vert
```

形变块(以 pbr_forward_morph.vert 为例):

```glsl
layout(binding = 24) uniform texture2D texMorph;   // slot19(RGBA16F,双行/目标)
// 复用 smpMat(binding 23);ItemUBO 内 ext5/ext6 = weights[8],ext4.w = 目标数
// 纹理行布局:目标 i 的 POSITION 增量在行 i*2,NORMAL 增量在行 i*2+1
vec3 dP = vec3(0.0); vec3 dN = vec3(0.0);
const int mc = int(ext4.w + 0.5);
if (mc > 0) {
  float wts[8] = float[8](ext5.x, ext5.y, ext5.z, ext5.w,
                          ext6.x, ext6.y, ext6.z, ext6.w);
  for (int i = 0; i < 8; ++i) {
    if (i >= mc) break;
    dP += texelFetch(sampler2D(texMorph, smpMat),
                     ivec2(int(gl_VertexIndex), i * 2), 0).xyz * wts[i];
    dN += texelFetch(sampler2D(texMorph, smpMat),
                     ivec2(int(gl_VertexIndex), i * 2 + 1), 0).xyz * wts[i];
  }
}
```

(skinned 变体经 `iu.items[vItem]` 或等价块访问;shadow 变体无 ItemUBO 完整块,
仅取 ext4.w/ext5/ext6 所需字段——shadow_depth 系 ItemUBO 块声明同现状裁剪。)

**应用顺序**:先 morph 后 skin(glTF/Khronos viewer 语义:形变作用于绑定姿态,
再蒙皮)。skinned 变体:morph 累加发生在蒙皮矩阵乘法之前的绑定空间。

法线/切线:法线累加 dN 后按现有路径(normalize;TBN 构造在 frag,不变)。
世界变换经 normalMatrix——morph 后法线自然跟随 ✓。

## 6. Renderer / 动画 / C API

### 管线选择(MeshRenderable::record)

| mesh 标志 | 场景 pass 管线 | 阴影 pass 管线 |
|---|---|---|
| 无 morph 无 skin(现状) | pbr/unlit | shadow_depth(_mask/_instanced) |
| skin | skinned | shadow_depth_skinned |
| morph | **pbr_forward_morph** | **shadow_depth_morph** |
| morph+skin | **pbr_forward_morph_skinned** | **shadow_depth_morph_skinned** |

- RenderContext 追加 `TextureHandle morphTex; SamplerHandle morphSampler;`
  (Renderer 恒注入;无效则 record 不绑)。
- RendererShaderDesc 追加 4 个 vert 代码字段;init/ensureScenePipelines 建
  4 条 morph 管线(shader 空=不建,向后兼容)。
- **实例化分组排除 morph 项**(组条件追加 !morph,同蒙皮)。
- **视锥剔除跳过 morph 项**(动态包围,同蒙皮:culling 谓词追加 morph)。
- ItemUBO 填充:ext5/ext6 = 当前权重(Animator 或静态初始值),ext4.w = 目标数。

### Animator(scene/animator)

- AnimationClip 追加 weights 轨道:目标节点 + 每关键帧 `targetCount` 个分量
  (cgltf path==weights;现有 loader 对该 path 跳过,改为解析)。
- Animator::update 在 jointMatrices 之外维护 `morphWeights`(每受影响 mesh 一组),
  线性插值;STEP 退化保持。
- 提交路径:submit(带 morph)时从 Animator 读当前权重写 ItemUBO(渲染循环
  经 RenderContext 或 MeshRenderable 侧注入——与 jointPalette 同模式)。

### C API

- `rd_engine_set_morph_weight(engine, uint32_t target, float w)`:作用于当前
  模型首个 morph mesh(动画播放中优先动画值,暂停/无动画时手动值生效——
  简化语义:手动调用覆盖对应分量并暂停该轨道)。
- play/crossfade/pause 复用现有(权重轨道免费搭车)。

### 门控

**不做画质门控**:每顶点 ≤8 次 fetch + 小 UBO 更新,成本轻;所有档全开。
(与 transmission/ext_materials 的重特性不同。)

## 7. 错误处理与降级

- 目标数 >8:截断 + RD_LOGW(FaceCap 53 目标 → 前 8)。
- 增量 accessor 与顶点数不匹配:跳过该目标(记日志),不崩溃。
- RGBA16F 纹理创建失败:该 mesh 按**无 morph**渲染 + 日志(蒙皮/基础路径仍在)。
- morph+unlit:不支持(unlit 语义无光照,形变无视觉意义)——按 unlit 渲染 +
  一次性日志。
- 顶点数为 0/无效 morph 数据:morph=false 兜底。

## 8. 测试

1. **单测**(tests/resource/gltf_test.cpp 追加):程序化 glTF JSON 含
   targets/初始 weights/targetNames → 断言 morphDeltas/morphWeights/截断(10
   目标 → 8)/sparse accessor 展开(cgltf_accessor_read_float 路径)。
2. **零操作语义**(tests/renderer/morph_test.cpp):全零权重渲染 == 同模型去掉
   morph 数据渲染,逐像素一致(compareSSIM 阈值 0.0)。
3. **golden**:AnimatedMorphCube / AnimatedMorphSphere(fetch_assets 收录;
   缺失自动 skip)+ 程序化 `morph_demo` 场景(球体多目标波浪形变,
   `--scene morph_demo` / interactive 直驱)。
4. **morph+skin 组合**:tests/common 运行时生成资产(扩展 skinned_gen 思路:
   单骨旋转 + 单目标位移)golden 双后端。
5. 全量回归:现有 golden 不动(硬约束)。

## 9. 已知限制

- 8 目标上限(超出截断;glTF 一致性最低要求线)。
- TANGENT 增量忽略(法线贴图视角下的切线不随形变,视觉误差通常可忽略)。
- 增量纹理 RGBA16F 量化(尾数 10 位;demo/golden 自洽,极端模型可见细微阶梯)。
- 拾取(picking)用绑定姿态(同蒙皮既有限制)。
- morph 项不参与实例化/视锥剔除(同蒙皮惯例)。
- 手动权重与动画同 target 冲突时动画优先(语义见 §6)。
