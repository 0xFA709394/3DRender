# P4-B KHR transmission/volume 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 落地 glTF 透射/体积材质两件套（场景两段 pass + 颜色缓冲折射采样 + mip 模糊 + Beer-Lambert 吸收），默认因子零操作、现有 golden 全量不动。

**Architecture:** 场景 pass 拆两段（opaque → 拷贝 SceneTarget 到 transmissionTex + generateMipmaps → load 重开渲 transmission/blend 项）；pbr 系 shader 采样器声明改「texture2D 分离 + 共享 smpMat」（sampler 描述符 16→5，绕开 MoltenVK/Metal 每阶段 16 sampler 上限）；门控同 ext_materials（选项 × 画质档与关系，关闭时零拆分开销）。

**Tech Stack:** C++17 / GLSL 450(经 glslang→SPIR-V→spirv-cross 三后端) / cgltf / glm / ctest。

**Spec:** `docs/superpowers/specs/2026-08-31-p4b-transmission-volume-design.md`（已确认）

---

## 0. 背景（实测结论与关键约束）

1. **MoltenVK 每阶段 sampler 上限实测 = 16**（Apple M2 Pro 探针：maxPerStageDescriptorSamplers=16、
   maxPerStageDescriptorSampledImages=256；Metal iOS sampler 状态上限同为 16）。P4-A 已用满 16 个
   combined sampler → 直接加 3 个必超限。**sampled image 维度宽裕** → 分离 texture/sampler 是唯一出路。
2. **GLSL 450 原生支持分离声明**（glslang Vulkan 模式）：`uniform texture2D` + `uniform sampler` +
   `sampler2D(tex, smp)` 构造；spirv-cross 转 MSL 天然分离，转 GLSL ES 时自动合并回 combined
   （合并名规则待 Task 1 实测，通常为 `texX_smpMat`）。
3. **Vulkan 布局是全局单布局**（一个 setLayout_ 服务所有管线）——blit/post 族 shader 声明的是
   combined `tex0`，不能跟着改（会破坏 GLES `tex%u` 回放约定）。故 pbr 族管线独立布局族。
4. **beginRenderPass 恒清屏**（三后端硬编码 Clear）；Vulkan MSAA 颜色 storeOp=DONT_CARE（resolve 后
   即弃）、深度 storeOp=DONT_CARE——pass B 的 load 需要它们持久。
5. **generateMipmaps 已存在**（三后端实现，阶段一引入），可直接复用。

## 0.1 与 spec 的偏差（已论证）

| # | 偏差 | 理由 |
|---|------|------|
| 1 | Vulkan 引入**双管线布局族**（pbr 族混合布局 / blit+post+unlit+shadow 族保留 combined 布局），spec 原文隐含全局单布局改造 | blit 族 shader 用 `tex%u` 命名（GLES 回放约定），跟随分离改造会破坏该约定且改动面×5；布局族是 Vulkan 内部机制，PipelineDesc 加 `separateSamplers` 标记，Renderer 建 pbr 系管线时置位 |
| 2 | Metal 折返 hack **未删除，缩减为单行** `sampler(23)→sampler(0)` | smpMat 声明在 binding 23 → MSL `sampler(23)` 超上限，仍需一行文本折返；slot 12..15 折返行删除（分离后 sampler 索引只剩 9..12 + 23） |
| 3 | `OffscreenTargetDesc.preserveContent` 标记：scene target 置位后 Vulkan 该目标 render pass 的 MSAA 颜色 storeOp=DONT_CARE→STORE、深度 DONT_CARE→STORE；Metal 对应 StoreAndMultisampleResolve/Store | spec 只说了 load 语义；MSAA 内容持久化必须 pass A 就 store，按目标标记（非按 pass）最简，代价仅 scene target 一处带宽 |
| 4 | 透射 shader 公式按 glTF-Sample-Viewer/three.js 结构具体化（漫反射位替换 + 入射面菲涅尔权重 + 屏幕空间偏移投影），spec 伪代码为骨架 | golden 自生成自洽；spec §5 已注明"具体公式细节 plan 阶段移植" |
| 5 | slot16 全局场景纹理在 pass A 绑 1×1 占位、pass B 绑真图，均由 Renderer 绑定（MeshRenderable 不感知） | Vulkan 布局声明了 binding 20 后，描述符须完整（即使均匀分支不采样）；全局纹理按 pass 绑定与 skybox/env 同模式 |

## 0.2 关键布局决定（全程有效）

- **纹理槽（最终形态）**：slot N ↔ Vulkan set0 binding(N+4) ↔ Metal texture(N+4) ↔ GLES 单元 N。
  - 分离槽（`texture2D` + 共享 smpMat）：0..4（材质五张）、9..15（P4-A 七张）、16=transmissionScene、
    17=transmission(R)、18=thickness(G) —— 共 15 个 **SAMPLED_IMAGE** 描述符
  - combined 槽：5=prefilterCube、6=brdfLut(nearest)、7=shadow、8=spotShadow（采样器状态特殊）
  - **binding 23 = `smpMat`（SAMPLER 描述符）**：与 mesh sampler 同状态（linear/mipmap/repeat）；
    场景折射 UV 由 shader 内 clamp 抵消 repeat 差异
  - sampler 描述符总数 = 5（smpMat + cube + lut + shadow ×2），远低于 16 上限
- **ItemUBO**：块 304→**336B**（追加 `ext3/ext4`），槽距 512B 不变；实例化 Item `_pad[13]→_pad[11]`。
  - `ext3`: x=transmissionFactor y=thicknessFactor z=attenuationDistance(0=+∞ 哨兵) w=0
  - `ext4`: xyz=attenuationColor w=0
- **FrameUBO**：256→**272B**，追加 `transmissionParams`: x=1/transW y=1/transH z=maxLod w=0；
  全部 FrameUBO 绑定位 range 256→272。
- **门控**：`render.transmission` 选项（默认 true）× `QualityPreset.transmission`（High/Mid=1,Low=0）
  与关系；关闭时 ItemUBO 写默认因子（ext3.x=0）+ 单段 pass。
- **零回归硬约束**：Task 1（分离重构）与 Task 5（UBO 扩容）后的现有 golden 全量必须通过；
  新 golden 只在 Task 8 新增。

---

### Task 1: pbr 系 shader 分离采样器改造（纯重构，零回归）

**Files:**
- Modify: `shaders/pbr_forward.frag`
- Modify: `shaders/pbr_forward_instanced.frag`
- Modify: `core/rhi/rhi_types.h`（PipelineDesc.separateSamplers）
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`（双布局族 + DescriptorKey + bindTexture + descriptorSetFor）
- Modify: `core/rhi/backends/metal/metal_device.mm`（bindTexture 映射 + TargetRec 无关）
- Modify: `core/rhi/backends/gles/gles_device.cpp`（kSamplerTable 合并名）
- Modify: `cmake/fixup_msl_samplers.cmake`
- Modify: `core/rhi/rhi_constants.inc.h:7`、三后端 caps `max_texture_slots`
- Modify: `core/renderer/renderer.cpp`（pbr 族管线创建处置位 separateSamplers）

- [ ] **Step 1: pbr_forward.frag 声明改分离模型**

`shaders/pbr_forward.frag` 顶部纹理声明区（第 36..53 行）整体替换为：

```glsl
// 分离采样器模型(每阶段 sampler 上限 16;此处 sampler 仅 5 个):
// 材质 2D 纹理=texture2D + 共享 smpMat(binding23,状态=mesh sampler);
// cube/lut/shadow 状态特殊保持 combined(binding 9..12 → slot 5..8)。
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
layout(binding = 15) uniform texture2D texClearcoatNormal; // slot11:清漆法线
layout(binding = 16) uniform texture2D texSheenColor;      // slot12:RGB
layout(binding = 17) uniform texture2D texSheenRough;      // slot13:A=粗糙度
layout(binding = 18) uniform texture2D texSpecularColor;   // slot14:RGB
layout(binding = 19) uniform texture2D texSpecular;        // slot15:A=specular 因子
layout(binding = 23) uniform sampler smpMat;  // 共享材质采样器(线性+repeat;UV 差异 shader 内 clamp)
```

（binding 20/21/22 = slot16/17/18，Task 7 再加声明。）

- [ ] **Step 2: pbr_forward.frag 全部采样点改构造语法**

同文件逐处替换（共 13 处，`texPrefilter/texBrdfLut/texShadow/texShadowSpot` 不动）：

```glsl
  vec4 baseColor = texture(sampler2D(texBaseColor, smpMat), vUV) * baseColorFactor;
```
```glsl
  vec2 mr = texture(sampler2D(texMR, smpMat), vUV).bg;   // glTF: G=roughness, B=metallic
```
```glsl
  vec3 nMap = (texture(sampler2D(texNormal, smpMat), vUV).xyz * 2.0 - 1.0) *
```
```glsl
    specWeight = clamp(ext0.w * texture(sampler2D(texSpecular, smpMat), vUV).a, 0.0, 1.0);
    vec3 specColor =
        clamp(ext2.xyz * texture(sampler2D(texSpecularColor, smpMat), vUV).rgb,
              vec3(0.0), vec3(1.0));
```
```glsl
    sheenColor = ext1.xyz * texture(sampler2D(texSheenColor, smpMat), vUV).rgb;
    sheenRough = clamp(ext1.w * texture(sampler2D(texSheenRough, smpMat), vUV).a, 0.03, 1.0);
```
```glsl
    ccFactor = clamp(ext0.x * texture(sampler2D(texClearcoat, smpMat), vUV).r, 0.0, 1.0);
    ccRough = clamp(ext0.y * texture(sampler2D(texClearcoatRough, smpMat), vUV).g, 0.03, 1.0);
    vec3 nMapCc = (texture(sampler2D(texClearcoatNormal, smpMat), vUV).xyz * 2.0 - 1.0) *
```
```glsl
  float ao = mix(1.0, texture(sampler2D(texOcclusion, smpMat), vUV).r, emissiveOcclusion.a);
  vec3 emissive = texture(sampler2D(texEmissive, smpMat), vUV).rgb * emissiveOcclusion.rgb;
```

- [ ] **Step 3: pbr_forward_instanced.frag 同步**

`shaders/pbr_forward_instanced.frag` 声明区（第 46..61 行）替换为与 Step 1 完全相同的 18 行声明
（两个文件声明一字不差）；采样点替换与 Step 2 相同（该文件主体与 pbr_forward.frag 同构，
`grep -n "texture(tex" shaders/pbr_forward_instanced.frag` 逐处确认，同样 13 处）。

- [ ] **Step 4: PipelineDesc 加布局族标记**

`core/rhi/rhi_types.h` PipelineDesc（约 179 行 `BlendDesc blend;` 附近）追加：

```cpp
  /// pbr 族管线=true:Vulkan 用分离采样器布局族(texture2D+smpMat,含 binding 23);
  /// 其余族(blit/post/unlit/shadow/env)保留 combined 布局。仅 Vulkan 后端消费。
  bool separateSamplers = false;
```

- [ ] **Step 5: Vulkan 双描述符布局 + 池扩容**

`core/rhi/backends/vulkan/vulkan_device.cpp` init 内（约 553..589 行），现有 combined 布局
创建代码**保持不动**（blit 族继续用 setLayout_），紧随其后追加 pbr 族布局：

```cpp
  // pbr 族布局(分离采样器):binding 0..3 UBO;
  // 4..8/13..22 = SAMPLED_IMAGE(slot 0..4,9..18 分离纹理);
  // 9..12 = COMBINED(slot 5..8 cube/lut/shadow);23 = SAMPLER(共享 smpMat)
  VkDescriptorSetLayoutBinding pb[24]{};
  for (uint32_t i = 0; i < kMaxUniformSlots; ++i) {
    pb[i].binding = i;
    pb[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    pb[i].descriptorCount = 1;
    pb[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  auto setBinding = [&](uint32_t b, VkDescriptorType t) {
    pb[b].binding = b;
    pb[b].descriptorType = t;
    pb[b].descriptorCount = 1;
    pb[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  };
  for (uint32_t s = 0; s <= 4; ++s) setBinding(4 + s, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
  for (uint32_t s = 5; s <= 8; ++s) setBinding(4 + s, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
  for (uint32_t s = 9; s <= 18; ++s) setBinding(4 + s, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
  setBinding(23, VK_DESCRIPTOR_TYPE_SAMPLER);
  VkDescriptorSetLayoutCreateInfo pbci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  pbci.bindingCount = 24;
  pbci.pBindings = pb;
  VK_CHECK(vkCreateDescriptorSetLayout(device_, &pbci, nullptr, &pbrSetLayout_));

  VkPipelineLayoutCreateInfo pblci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pblci.setLayoutCount = 1;
  pblci.setLayouts = &pbrSetLayout_;
  VK_CHECK(vkCreatePipelineLayout(device_, &pblci, nullptr, &pbrPipelineLayout_));
```

池大小数组追加两项（池共用，两种族都从同一池分配）：

```cpp
  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxUniformSlots * kMaxDescSets},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * kMaxDescSets},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 15 * kMaxDescSets},
      {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxDescSets},
  };
```
（dpci.poolSizeCount = 4。）

类声明区（约 341 行 findOrCreateRenderPass 声明附近）追加成员：
`VkDescriptorSetLayout pbrSetLayout_ = VK_NULL_HANDLE;`、
`VkPipelineLayout pbrPipelineLayout_ = VK_NULL_HANDLE;`；设备清理处 destroy 两个句柄。

- [ ] **Step 6: Vulkan 管线创建按族选布局 + PipelineRec 记族**

createPipeline 内（找现有引用 `pipelineLayout_` 创建 VkGraphicsPipeline 的位置）：

```cpp
  const bool separate = desc.separateSamplers;
  const VkPipelineLayout layout = separate ? pbrPipelineLayout_ : pipelineLayout_;
```
管线创建信息 `.layout = layout`；PipelineRec 追加 `bool separate = false;` 并存 `rec.separate = separate;`。
（PipelineCached/缓存结构若含 layout，同步携带族标记。）

- [ ] **Step 7: Vulkan DescriptorKey 扩容 + 族维度**

同文件（约 183 行）：

```cpp
/// 每 draw 的绑定状态 key(POD;memcmp 比较,须零初始化构造)。
struct DescriptorKey {
  VkBuffer ubo[4];
  uint64_t uboOffset[4];
  uint64_t uboSize[4];
  VkImageView texView[19];      // slot 0..18(分离槽只写 view;combined 槽 5..8)
  VkSampler texSampler[19];     // 仅 combined 槽 5..8 有效
  VkSampler sharedSampler = VK_NULL_HANDLE;  // 分离族:binding 23(smpMat 状态)
  bool pbrFamily = false;       // 布局族(键维度)
  bool operator<(const DescriptorKey& o) const {
    return memcmp(this, &o, sizeof(DescriptorKey)) < 0;
  }
};
```

- [ ] **Step 8: Vulkan bindTexture 按族分流 + 槽位 19**

同文件 bindTexture（约 1943 行）：

```cpp
/// 绑定约定：texture slot N ↔ set0 binding(N+4)。
/// pbr 族：分离槽(除 5..8)只记 view+sharedSampler;combined 槽 5..8 记 view+sampler。
/// blit 族：binding(N+4) combined(view+sampler)。
void VulkanCommandBuffer::bindTexture(uint32_t slot, TextureHandle texture,
                                      SamplerHandle sampler) {
  const TextureRec* rec = device_->texture(texture);
  VkSampler s = device_->sampler(sampler);
  if (!rec || s == VK_NULL_HANDLE || slot >= 19) return;
  bound_.texView[slot] = rec->view;
  if (bound_.pbrFamily && !(slot >= 5 && slot <= 8)) {
    bound_.sharedSampler = s;  // 全部分离槽共享同一采样器状态(mesh/全局)
  } else {
    bound_.texSampler[slot] = s;
  }
}
```

bindPipeline 内（约 1933 行）：`currentLayout_ = ...` 改为按 rec.separate 选
`device_->pbrPipelineLayout()` / `device_->pipelineLayout()`（Device 类补一个 pbrPipelineLayout()
访问器），并 `bound_.pbrFamily = rec.separate;`（须在换管线时把上一族状态作废——
descriptorSetFor 按 key 缓存，族入 key 后天然隔离；bindPipeline 处同时 `bound_ = DescriptorKey{};`
重置绑定状态最稳妥，与"每 draw 按绑定状态"语义一致）。

- [ ] **Step 9: Vulkan descriptorSetFor 双族写描述符**

descriptorSetFor（DescriptorKey → VkDescriptorSet 的构建函数，搜 `vkUpdateDescriptorSets`）：
按 `key.pbrFamily` 分支——

```cpp
  // 写法骨架：pbr 族
  // binding 0..3: UBO（与现路径相同）
  // binding 4..8: VkWriteDescriptorSet{SAMPLED_IMAGE, imageView=key.texView[slot], }
  // binding 9..12: COMBINED{imageView + key.texSampler[slot]}
  // binding 13..22: SAMPLED_IMAGE（slot 9..18；view 无效(未绑)的 binding 跳过不写，
  //                 Vulkan 允许布局声明多于 shader 实际使用）
  // binding 23: SAMPLER{sampler = key.sharedSampler}
  // blit 族：保持现有 16 combined 写法不变
```
（写循环按 binding 号组装 `VkDescriptorImageInfo`；SAMPLED_IMAGE 的 imageLayout 用
`VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`，与现路径 combined 写法一致。）

- [ ] **Step 10: Metal bindTexture 族分流**

`core/rhi/backends/metal/metal_device.mm` 的 MetalCommandBuffer::bindTexture（搜
`sampler` 折返注释）：折返映射（slot12..15→0..3）改为按当前管线族——

```cpp
// 纹理索引恒 = slot+4(≤22 < 31 上限)。
// 采样器索引:pbr 族 → 分离槽共享 index 0(smpMat),combined 槽 5..8 → index slot+4(9..12);
// 其余族(blit 等) → index slot+4(4..7,无折返需求)。
```
实现：PipelineRec 追加 `bool separate;`（createPipeline 存 desc.separateSamplers）；
bindTexture 内按 `pipeline_.separate` 分支：分离槽 `setFragmentTexture:nil` 之外只
`setFragmentSamplerState:smp atIndex:0`；combined 槽与旧路径一致（index = slot+4）。
sampler state 句柄由入参 sampler 取（现有逻辑）。

- [ ] **Step 11: Metal 折返脚本缩为单行**

`cmake/fixup_msl_samplers.cmake` 全文替换：

```cmake
# MSL sampler 索引折返:smpMat(binding 23)→ sampler(0)。
# Metal sampler 参数上限 0..15;分离采样器模型下仅剩 smpMat(23)超限
# (combined cube/lut/shadow = 9..12 在限内;共享 index 0 空闲)。
# 与 core/rhi/backends/metal bindTexture 的共享映射一致。
if(NOT DEFINED CMAKE_ARGV3)
  message(FATAL_ERROR "用法: cmake -P fixup_msl_samplers.cmake <file.metal>")
endif()
if(NOT EXISTS "${CMAKE_ARGV3}")
  message(FATAL_ERROR "MSL 不存在: ${CMAKE_ARGV3}")
endif()
file(READ "${CMAKE_ARGV3}" MSL)
string(REPLACE "sampler(23)" "sampler(0)" MSL "${MSL}")
file(WRITE "${CMAKE_ARGV3}" "${MSL}")
```

- [ ] **Step 12: caps 扩到 19**

- `core/rhi/backends/vulkan/vulkan_device.cpp:477`：`caps_.set(Capability::max_texture_slots, 19);  // slot0..18(transmission 三槽)`
- `core/rhi/backends/metal/metal_device.mm:290`、`core/rhi/backends/gles/gles_device.cpp:615` 同改
- `core/rhi/rhi_constants.inc.h:7` 注释改为 `// 纹理槽数(绑定约定上限 19:slot0..18;GLES 真机普遍 32+)`

- [ ] **Step 13: Renderer pbr 族管线置位**

`core/renderer/renderer.cpp`：init 与 ensureScenePipelines 中创建 pbrPipeline_/blendPipeline_/
skinnedPipeline_/instancedPipeline_ 的四处 `PipelineDesc` 追加 `pd.separateSamplers = true;`
（unlit/skybox/shadow/blit/post 管线不动）。搜 `pbrPipeline_ =` / `blendPipeline_ =` /
`skinnedPipeline_ =` / `instancedPipeline_ =` 逐处确认。

- [ ] **Step 14: 构建 + 验证 GLSL ES 合并名（前置风险实测）**

```bash
./scripts/check.sh
```
构建后找 GLES 产物（`find build -name "*.gles" -o -name "*pbr_forward*" | grep -v metal` 或
embedded_shaders 生成目录），dump pbr_forward 的 GLSL ES 文本：

```bash
grep -o "uniform sampler2D [A-Za-z0-9_]*" build/<gles 产物路径>/pbr_forward*.frag* | sort -u
```

- **预期**：出现 15 个合并名（`texBaseColor` 或 `texBaseColor_smpMat` 风格，spirv-cross 决定）。
- **若合并失败**（GLSL ES 含裸 `texture2D`/`sampler` 声明 → 编译必炸）：本任务止步，回报
  「方案 A 不可行」，回落 spec 备选（最小腾位：仅 slot12..15+新槽分离）——但同一机制，
  大概率同样失败则需进一步评估；**此验证是后续所有任务的前置门**。

- [ ] **Step 15: GLES kSamplerTable 填实测合并名**

`core/rhi/backends/gles/gles_device.cpp:816` kSamplerTable：保留全部现有条目（blit 族
`tex%u` 回放共用此函数无关，语义名表只对声明的 uniform 生效），把 pbr 语义名替换为实测
合并名。若合并名 = 原名（`texBaseColor`），则表**零改动**；若为 `texBaseColor_smpMat` 风格，
则 pbr 系 15 条改名追加（旧名条目保留无害——`glGetUniformLocation` 找不到就跳过）：

```cpp
  } kSamplerTable[] = {
      {"texBaseColor", 0},     {"texMR", 1},           {"texNormal", 2},
      {"texEmissive", 3},      {"texOcclusion", 4},    {"texPrefilter", 5},
      {"texBrdfLut", 6},       {"texShadow", 7},       {"texShadowSpot", 8},
      {"texEquirect", 0},      {"texEnv", 0},          {"texClearcoat", 9},
      {"texClearcoatRough", 10}, {"texClearcoatNormal", 11}, {"texSheenColor", 12},
      {"texSheenRough", 13},   {"texSpecularColor", 14}, {"texSpecular", 15},
      // 分离模型合并名(实测后按需替换;两者共存无害)
      {"texBaseColor_smpMat", 0}, {"texMR_smpMat", 1}, ...
  };
```

- [ ] **Step 16: 全量 golden 零回归验证**

```bash
./scripts/check.sh
ctest --test-dir build --output-on-failure
```
预期：全部通过（本任务纯重构：同采样器状态、同纹理、同绑定语义，输出应逐像素一致）。
**任何 golden 失败 = 本任务未完成，不得进入 Task 2。**

- [ ] **Step 17: Commit**

```bash
git add -A && git commit -m "refactor(rhi): pbr 系 shader 分离采样器(16 combined→5 sampler+15 sampled image;Vulkan 双布局族/Metal 折返缩单行/GLES 合并名;caps 19)"
```

---

### Task 2: RHI LoadOp + preserveContent + 契约测试

**Files:**
- Modify: `core/rhi/rhi_device.h:43`
- Modify: `core/rhi/rhi_types.h`（OffscreenTargetDesc.preserveContent）
- Modify: `core/rhi/backends/metal/metal_device.mm`（beginRenderPass + TargetRec + createOffscreenTarget）
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`（beginRenderPass + RenderPassKey + findOrCreateRenderPass）
- Modify: `core/rhi/backends/gles/gles_device.cpp`（beginRenderPass 跳过清屏）
- Test: `tests/rhi/loadop_test.cpp`（新增，注册进对应 CMakeLists——参照现有 rhi 测试目标）

- [ ] **Step 1: 写失败契约测试**

`tests/rhi/loadop_test.cpp`（参照同目录现有测试的 device 创建/三角形绘制样板；若无样板，
用 tests/renderer/renderer_test.cpp 的离屏渲染骨架裁剪）：

```cpp
// LoadOp 契约:pass1 清屏画红三角 → pass2 loadContent 重开画蓝三角(不重叠) →
// 回读断言两色共存(红保留 + 蓝新画);depth 保留:pass2 三角在 pass1 三角后方仍被遮挡。
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"
#include "rhi/rhi_factory.h"
// ...(device 创建 + 全屏三角形 shader 加载,参照现有 rhi 契约测试工具)

TEST(LoadOp, PreserveAcrossPasses) {
  for (rd::Backend b : {rd::Backend::Metal, rd::Backend::Vulkan}) {
    auto device = rd::createDevice({b});
    ASSERT_TRUE(device);
    // 离屏目标:preserveContent=true,带深度
    rd::OffscreenTargetDesc td;
    td.width = 64; td.height = 64; td.depth = true; td.preserveContent = true;
    auto target = device->createOffscreenTarget(td);
    ASSERT_TRUE(target.valid());
    // pass1: clear 黑,画左半红三角(深度写)
    // pass2: beginRenderPass(target, clear, /*loadContent=*/true),画右半蓝三角
    // readbackTarget → 断言:左半红(保留)、右半蓝(新画)、均非黑
    device->destroyTarget(target);
  }
}
```
（三角形绘制细节：复用现有测试里的 blit.vert/全屏三角形方式，或直接拷贝 Cube 测试的
顶点缓冲绘制样板——以仓库现有 rhi 契约测试为准，保证三后端同一套。）

CMake：把文件加进 rhi 测试目标（`tests/rhi/CMakeLists.txt` 或全局 tests CMake，按现状）。

- [ ] **Step 2: 跑测试确认编译失败**

```bash
cmake --build build --target rd_tests 2>&1 | head -20
```
预期：编译错误（beginRenderPass 无第三参 / OffscreenTargetDesc 无 preserveContent）。

- [ ] **Step 3: RHI 接口扩展**

`core/rhi/rhi_types.h` OffscreenTargetDesc（241 行 `}` 前）追加：

```cpp
  /// 内容跨 pass 持久:pass A 结束后内容(MSAA 样本/深度)须 store 以便 load 重开续画
  /// (transmission 两段 pass 用)。scene target 置位;默认 false 零开销。
  bool preserveContent = false;
```

`core/rhi/rhi_device.h:43` 改为：

```cpp
  virtual void beginRenderPass(TargetHandle target, const ClearColor& clear,
                               bool loadContent = false) = 0;
```

- [ ] **Step 4: Metal 实现**

`metal_device.mm`：
- TargetRec 追加 `bool preserve = false;`；createOffscreenTarget 存 `desc.preserveContent`。
- beginRenderPass（983 行）签名加 `bool loadContent`；颜色/深度 loadAction：

```objc
  const MTLLoadAction load = loadContent ? MTLLoadActionLoad : MTLLoadActionClear;
```
（depthOnly 分支的深度 loadAction 保持 Clear——阴影目标每次清。）
- MSAA 颜色 storeAction（1002 行）：

```objc
    rp.colorAttachments[0].storeAction =
        t.preserve ? MTLStoreActionStoreAndMultisampleResolve
                   : MTLStoreActionMultisampleResolve;
```
- 深度 storeAction（1015 行）：

```objc
    rp.depthAttachment.storeAction =
        t.preserve ? MTLStoreActionStore : MTLStoreActionDontCare;
```

- [ ] **Step 5: Vulkan 实现**

- RenderPassKey（搜 `struct RenderPassKey`）追加 `bool load; bool preserve;`
  （POD key，memcmp/比较器同步）。
- findOrCreateRenderPass（647 行）签名加 `bool load, bool preserve`：
  - 颜色（700..708 行）：`loadOp = load ? LOAD : CLEAR`；
    `storeOp = samples > 1 ? (preserve ? STORE : DONT_CARE) : STORE`；
    `initialLayout = load ? (samples > 1 ? COLOR_ATTACHMENT_OPTIMAL
                                        : SHADER_READ_ONLY_OPTIMAL)
                         : UNDEFINED`
  - 深度（713..718 行）：`loadOp = load ? LOAD : CLEAR`；
    `storeOp = preserve ? STORE : DONT_CARE`；
    `initialLayout = load ? DEPTH_STENCIL_ATTACHMENT_OPTIMAL : UNDEFINED`
  - resolve 附件（720 行起）：loadOp 恒 DONT_CARE/storeOp STORE 不变
  - load 变体子依赖：追加 srcStage=FRAGMENT_SHADER(color) 的外部依赖
    （copy pass 采样 transmissionTex 后 pass B 写同目标；简单起见 deps[0].srcStageMask
    追加 `VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT`、srcAccessMask 追加
    `VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT`）
- 550..551 行预热调用同步传 `(false, false)`。
- beginRenderPass（1892 行）：`renderPassAt(toVkFormat(t.format), t.hasDepth, t.samples)`
  → `findOrCreateRenderPass(toVkFormat(t.format), t.hasDepth, t.samples, loadContent, t.preserve)`
  （depthOnly 分支传 `(true→保持现状: format UNDEFINED 分支不动)`）。
- beginRenderPass 签名加 `bool loadContent`；clears 保持填充（load 时 Vulkan 忽略 clear 值，
  只 loadOp 生效——vkCmdBeginRenderPass 传了 clearValue 也无害）。

- [ ] **Step 6: GLES 实现**

`gles_device.cpp` beginRenderPass（370 行）签名加 `bool loadContent`，lambda 捕获，
清屏调用改为：

```cpp
    if (loadContent) return;  // load 语义:附着内容天然持久,跳过清屏
```
（放在 FBO 绑定/viewport 之后、glClearColor 之前；depthOnly 分支保持清屏——阴影目标。）

- [ ] **Step 7: 跑契约测试通过（双后端）**

```bash
cmake --build build && ctest --test-dir build -R LoadOp --output-on-failure
```
预期：PASS（Metal + Vulkan）。

- [ ] **Step 8: scene target 置位 preserve + 全量回归**

`core/renderer/renderer.cpp` ensureSceneTarget（590..595 行）追加：`td.preserveContent = true;`

```bash
./scripts/check.sh && ctest --test-dir build --output-on-failure
```
预期：全绿（无 load 调用方 = 行为不变；MSAA store 增量不影响像素结果）。

- [ ] **Step 9: Commit**

```bash
git add -A && git commit -m "feat(rhi): beginRenderPass loadContent 语义 + OffscreenTargetDesc.preserveContent(三后端;契约测试双后端)"
```

---

### Task 3: loader 解析 transmission/volume + 单测

**Files:**
- Modify: `core/resource/gltf_loader.h`（MaterialData）
- Modify: `core/resource/gltf_loader.cpp`（readMaterial）
- Test: `tests/resource/gltf_test.cpp`（追加用例）

- [ ] **Step 1: 写失败单测**

`tests/resource/gltf_test.cpp` 追加（参照现有最小 gltf JSON 构造样板——搜
`extensions` 或 `KHR_materials` 现有用例的 JSON 拼装与临时文件写入方式）：

```cpp
// KHR_materials_transmission + KHR_materials_volume 解析(默认值零操作)
TEST(Gltf, TransmissionVolume) {
  // 1) 带两扩展的材质:transmissionFactor=0.9 + attenuationColor(0.8,0.2,0.1)/
  //    attenuationDistance=0.5 + thicknessFactor=2.0
  //    (JSON: materials[0].extensions.KHR_materials_transmission/volume;
  //     单三角 mesh 最小骨架,复制现有测试的 asset 结构)
  // 2) 断言 m.transmissionFactor==0.9f、thicknessFactor==2.0f、
  //    attenuationColor 近似 (0.8,0.2,0.1)、attenuationDistance==0.5f
  // 3) 无扩展材质断言默认:0/0/(1,1,1)/0(∞ 哨兵)
}
```
（具体 JSON 骨架从现有 clearcoat/sheen 单测拷贝改造——`rg -n "KHR_materials_clearcoat" tests/resource/gltf_test.cpp`。）

- [ ] **Step 2: 跑测试确认失败**

```bash
cmake --build build && ctest --test-dir build -R Gltf.Transmission --output-on-failure
```
预期：FAIL（字段不存在，编译错误即失败）。

- [ ] **Step 3: MaterialData 追加字段**

`core/resource/gltf_loader.h` MaterialData（59 行 ior 之后）：

```cpp
  // ---- KHR transmission/volume(P4-B;默认值 = 零操作语义)----
  // KHR_materials_transmission
  ImageData transmissionTex;        float transmissionFactor = 0.0f;  // 0=无透射
  // KHR_materials_volume
  ImageData thicknessTex;           float thicknessFactor = 0.0f;      // 0=薄壁
  float attenuationColor[3] = {1, 1, 1};
  float attenuationDistance = 0.0f; // 0 哨兵 = spec 默认 +∞(无吸收)
```

- [ ] **Step 4: readMaterial 解析**

`core/resource/gltf_loader.cpp` readMaterial（has_ior 块之后，约 139..170 行区域）：

```cpp
  // KHR_materials_transmission:透射(场景色折射采样,渲染层 pass 拆分)
  if (mat->has_transmission) {
    const auto& tr = mat->transmission;
    m.transmissionFactor = float(tr.transmission_factor);
    if (tr.transmission_texture.texture)
      m.transmissionTex = decodeImage(tr.transmission_texture.texture, gltfDir, pref);
  }
  // KHR_materials_volume:厚度 + Beer-Lambert 吸收
  if (mat->has_volume) {
    const auto& vo = mat->volume;
    m.thicknessFactor = float(vo.thickness_factor);
    if (vo.thickness_texture.texture)
      m.thicknessTex = decodeImage(vo.thickness_texture.texture, gltfDir, pref);
    for (int c = 0; c < 3; ++c) m.attenuationColor[c] = float(vo.attenuation_color[c]);
    m.attenuationDistance =
        vo.attenuation_distance > 0.0f ? float(vo.attenuation_distance) : 0.0f;
  }
```

- [ ] **Step 5: 测试通过 + Commit**

```bash
cmake --build build && ctest --test-dir build -R Gltf --output-on-failure
git add -A && git commit -m "feat(resource): glTF KHR_materials_transmission/volume 解析(默认零操作)+ 单测"
```

---

### Task 4: MeshGpuData 透射纹理 + 资源层接线

**Files:**
- Modify: `core/resource/mesh_render_resource.h`
- Modify: `core/resource/mesh_render_resource.cpp`
- Modify: `core/renderer/mesh_renderable.cpp`（bindExtTextures → 17/18）

- [ ] **Step 1: MeshGpuData 字段**

`mesh_render_resource.h` MeshGpuData（33 行 specularTex 之后）：

```cpp
  // ---- KHR transmission/volume(P4-B;缺省白:因子默认 0 压制贡献)----
  TextureHandle transmissionTex;   // R=透射强度
  TextureHandle thicknessTex;      // G=厚度
```

- [ ] **Step 2: 上传与销毁**

`mesh_render_resource.cpp`：
- upload（77 行 specularTex 之后）：

```cpp
  g.transmissionTex = uploadOr(dev, m.material.transmissionTex, res->fallbackWhite_);
  g.thicknessTex = uploadOr(dev, m.material.thicknessTex, res->fallbackWhite_);
```
- 有效性检查（94 行区域）追加 `!g.transmissionTex.valid() || !g.thicknessTex.valid() ||`
- destroy（110 行纹理销毁清单）追加两句柄。

- [ ] **Step 3: MeshRenderable 恒绑定 17/18**

`mesh_renderable.cpp` bindExtTextures（17..25 行）追加：

```cpp
    cmd->bindTexture(17, g.transmissionTex, mesh_->sampler());
    cmd->bindTexture(18, g.thicknessTex, mesh_->sampler());
```
（三条 pbr 路径自动生效;shader 尚未声明这些槽 → 无视觉变化。）

- [ ] **Step 4: 构建 + 回归 + Commit**

```bash
./scripts/check.sh && ctest --test-dir build --output-on-failure
git add -A && git commit -m "feat(resource): MeshGpuData transmission/thickness 纹理上传 + 槽 17/18 恒绑定"
```

---

### Task 5: ItemUBO 336B + FrameUBO 272B（零回归）

**Files:**
- Modify: `core/renderer/renderable.h:17`
- Modify: `core/renderer/renderer.cpp`（ItemUBOData + 填充 + FrameUBO fu）
- Modify: `shaders/pbr_forward.frag` / `shaders/pbr_forward_instanced.frag`（块声明）
- Modify: `core/renderer/mesh_renderable.cpp`（bind range 256→272）
- Modify: `tests/renderer/renderer_test.cpp`（FrameUBO 骨架同步，如有 256 字面量）

- [ ] **Step 1: 常量与宿主结构**

`renderable.h:17`：`kItemUboSize = 336;`（注释 `块 336B`）。
`renderer.cpp` ItemUBOData（60..72 行）：

```cpp
struct ItemUBOData {
  math::Mat4 mvp;
  math::Mat4 world;
  math::Mat4 normalMatrix;
  float baseColorFactor[4];
  float emissiveOcc[4];     // rgb=emissiveFactor, a=occlusionStrength
  float metallicRough[4];   // x=metallic, y=roughness, z=normalScale, w=alphaCutoff
  float uvTransform[4];     // xy=offset, zw=scale
  float ext0[4];            // x=clearcoatFactor y=clearcoatRoughness z=clearcoatNormalScale w=specularFactor
  float ext1[4];            // xyz=sheenColorFactor w=sheenRoughnessFactor
  float ext2[4];            // xyz=specularColorFactor w=ior
  float ext3[4];            // x=transmissionFactor y=thicknessFactor z=attenuationDistance(0=∞) w=0
  float ext4[4];            // xyz=attenuationColor w=0
};
static_assert(sizeof(ItemUBOData) == kItemUboSize, "ItemUBO 必须 336B(槽距 512)");
```

- [ ] **Step 2: endScene 填充（透传门控占位——门控字段 Task 6 接线，此处先恒走材质值）**

`renderer.cpp` ext 四件套填充块（882..902 行）`if (extOn && ...)` 分支追加：

```cpp
        iu.ext3[0] = mm.transmissionFactor;
        iu.ext3[1] = mm.thicknessFactor;
        iu.ext3[2] = mm.attenuationDistance;
        iu.ext4[0] = mm.attenuationColor[0];
        iu.ext4[1] = mm.attenuationColor[1];
        iu.ext4[2] = mm.attenuationColor[2];
```
else 分支 ext3/ext4 保持 ItemUBOData{} 零值（transmissionFactor=0 → 零操作）✓ 无需补默认。

- [ ] **Step 3: FrameUBO 272B**

`renderer.cpp` beginScene fu 结构（690..697 行）：

```cpp
  struct {
    math::Mat4 viewProj;
    math::Vec4 cameraPos;
    math::Vec4 lightDir;
    math::Vec4 lightColor;
    float sh[9][4];
    float transmissionParams[4];  // x=1/transW y=1/transH z=maxLod w=0(Task 6 填真值)
  } fu{};
  static_assert(sizeof(fu) == 272, "FrameUBO 必须 272B");
```
frameUbo_ 创建尺寸 256→272（111 行：`dev.createBuffer({272, ...})`）。
transmissionParams 本任务恒零（Task 6 填）。

- [ ] **Step 4: shader 块声明同步**

两个 frag 的 FrameUBO 块（`vec4 sh[9];` 之后）：

```glsl
  vec4 transmissionParams;  // x=1/transW y=1/transH z=maxLod w=0
```
instanced.frag Item 结构（ext2 之后）：

```glsl
  vec4 ext3;  // x=transmissionFactor y=thicknessFactor z=attenuationDistance(0=∞) w=0
  vec4 ext4;  // xyz=attenuationColor w=0
  vec4 _pad[11];  // std140 数组元素 stride 对齐 CPU 槽距 512B(336+176)
```

- [ ] **Step 5: 绑定 range 256→272**

```bash
rg -n "frameUbo_, 0, 256" core/ tests/
```
逐处改 272（mesh_renderable.cpp 三处 pbr 路径、renderer.cpp 实例化路径 1055 行、
tests/renderer/renderer_test.cpp 骨架若有）。unlit 路径只绑 ItemUBO 前 64B 不涉及 FrameUBO ✓。

- [ ] **Step 6: 全量零回归 + Commit**

```bash
./scripts/check.sh && ctest --test-dir build --output-on-failure
git add -A && git commit -m "feat(renderer): ItemUBO 336B(ext3/ext4)+FrameUBO 272B(transmissionParams 预留;零回归)"
```

---

### Task 6: endScene 三分区 + 两段 pass + transmission 目标 + 门控

**Files:**
- Modify: `core/renderer/renderer.h` / `renderer.cpp`
- Modify: `core/renderer/quality.h` / `quality.cpp`
- Modify: `core/api/options.json`（render.transmission）
- Modify: `core/api/rd_api.cpp`（applyOptions）
- Test: 门控语义测试（Task 8 一并写,此处先手验）

- [ ] **Step 1: 门控字段**

`quality.h` QualityPreset（22 行后）：`uint32_t transmission;  ///< KHR transmission/volume(High/Mid=1,Low=0)`。
`quality.cpp` 三档初始化列表各追加末元素：High/Mid `1`、Low `0`（default 行 `0`）。
`renderer.h`：`void setTransmissionEnabled(bool on) { transmissionManual_ = on; }` +
成员 `bool transmissionManual_ = true; bool transmissionQuality_ = false;`；
setQuality（569 行区域）`transmissionQuality_ = q.transmission != 0;`。
`options.json` 追加：

```json
  , "render.transmission": {"type": "bool", "default": true, "doc": "KHR transmission/volume(透射/体积吸收)开关"}
```
`rd_api.cpp` applyOptions（146 行旁）：`e->renderer.setTransmissionEnabled(o.render.transmission);`

- [ ] **Step 2: Renderer 成员与目标管理**

`renderer.h` 追加成员：

```cpp
  // ---- transmission(P4-B)----
  TargetHandle transTarget_;            // transmissionTex mip0 拷贝目标(texture-backed)
  TextureHandle transTex_;              // 场景颜色拷贝(全 mip 链,格式随 SceneTarget)
  TextureHandle transPlaceholderTex_;   // 1x1 RGBA8 白(pass A 占位/降级)
  PipelineHandle transBlitPipeline_;    // blit 管线的 sceneFormat_ 变体(拷贝进 R16F 目标)
  uint32_t transW_ = 0, transH_ = 0;
  Format transFmt_ = Format::RGBA8_UNORM;
  /// 按场景尺寸/格式确保 transmission 纹理(全 mip)与拷贝目标;变化重建。
  bool ensureTransmissionTarget(uint32_t w, uint32_t h, Format fmt);
```
init：创建 transPlaceholderTex_（1×1 RGBA8 白，参照 shadowFallbackTex_ 创建方式）。
shutdown/destroy：释放三句柄。
`ensureTransmissionTarget`：

```cpp
bool Renderer::ensureTransmissionTarget(uint32_t w, uint32_t h, Format fmt) {
  if (transTex_.valid() && w == transW_ && h == transH_ && fmt == transFmt_) return true;
  if (transTarget_.valid()) dev_->destroyTarget(transTarget_);
  if (transTex_.valid()) dev_->destroyTexture(transTex_);
  const uint32_t mips = 1 + uint32_t(std::floor(std::log2(float(std::max(w, h)))));
  TextureDesc td;
  td.width = w; td.height = h; td.format = fmt;
  td.usage = TextureUsage::Sampled | TextureUsage::RenderTargetAttachment;
  td.mipLevels = mips;
  transTex_ = dev_->createTexture(td);
  OffscreenTargetDesc od;
  od.width = w; od.height = h; od.colorFromTexture = transTex_; od.mipLevel = 0;
  transTarget_ = dev_->createOffscreenTarget(od);
  transW_ = w; transH_ = h; transFmt_ = fmt;
  return transTex_.valid() && transTarget_.valid();
}
```
ensureScenePipelines 内同步重建 transBlitPipeline_（blitVs/blitFs + `colorFormat=fmt`
+ 无深度 + samples=1——与 blitPipeline_ 同构仅格式不同；scene==target 直渲回落时不建）。

- [ ] **Step 3: 三分区排序**

`renderer.cpp` endScene（748..763 行）替换：

```cpp
  // opaque → transmission → blend 三分区:前者和后者均按视距远→近;opaque 保提交序
  const bool transOn = transmissionManual_ && transmissionQuality_;
  std::vector<uint32_t> order(count);
  for (uint32_t i = 0; i < count; ++i) order[i] = i;
  auto matOf = [&](uint32_t i) -> const MaterialData* {
    const auto* r = static_cast<const MeshRenderable*>(queue_[i].get());
    return r->meshData().empty() ? nullptr : &r->meshData()[0].material;
  };
  auto tierOf = [&](uint32_t i) {
    const MaterialData* m = matOf(i);
    if (!m) return 0;
    if (m->alphaBlend) return 2;
    if (transOn && m->transmissionFactor > 0.0f) return 1;
    return 0;
  };
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    const int ta = tierOf(a), tb = tierOf(b);
    if (ta != tb) return ta < tb;
    if (ta == 0) return false;  // opaque 稳定
    const math::Vec4 e(cameraEye_, 1.0f);
    const float da = glm::dot(worldStack_[a][3] - e, worldStack_[a][3] - e);
    const float db = glm::dot(worldStack_[b][3] - e, worldStack_[b][3] - e);
    return da > db;
  });
```

- [ ] **Step 4: 场景 pass 拆两段**

同文件场景 pass（997..1097 行）重排：

```cpp
  cmd->beginRenderPass(scene, clear_);
  // 天空盒(...现有 999..1018 行不动...)
  // bind slot16 占位(pass A 无透射采样,占位满足描述符完整)
  cmd->bindTexture(16, transPlaceholderTex_, blitSampler_);
  RenderContext ctx;  // (...1019..1032 行现有填充不动...)
  bool anyTrans = false;
  for (uint32_t vi = 0; vi < camVis.size();) {
    const uint32_t idx = camVis[vi];
    if (tierOf(idx) != 0) { anyTrans = anyTrans || tierOf(idx) == 1; ++vi; continue; }
    // ...(现有 1035..1088 行实例化分组与非实例化路径,组条件追加:
    //    rid 判定加 `&& tierOf(idx) == 0` 风格的 transmission 排除——
    //    即 rid 非空条件追加 `matOf(idx) && matOf(idx)->transmissionFactor <= 0.0f`)...
  }
  cmd->endRenderPass();
  // ---- pass B:transmission + blend ----
  if (anyTrans && ensureTransmissionTarget(sceneW_, sceneH_, sceneFormat_)) {
    // 拷贝链:blit(scene 颜色 → transTex mip0) → generateMipmaps
    cmd->beginRenderPass(transTarget_, clear_);
    cmd->bindPipeline(transBlitPipeline_);
    const float vf = dev_->backend() == Backend::GLES ? 1.0f : 0.0f;
    const float bp[4] = {vf, 0.0f, 0.0f, 0.0f};
    dev_->updateBuffer(blitUbo_, bp, sizeof(bp), 0);
    cmd->bindUniformBuffer(0, blitUbo_, 0, 16);
    cmd->bindTexture(0, dev_->targetColorTexture(scene), blitSampler_);
    cmd->draw(3, 0);
    cmd->endRenderPass();
    dev_->generateMipmaps(transTex_);
    // FrameUBO transmissionParams(texel/maxLod;偏移 256 处 16B 局部更新)
    const float tp[4] = {1.0f / float(sceneW_), 1.0f / float(sceneH_),
                         float(mipsOf(transTex_)) - 1.0f, 0.0f};  // mipsOf:存 transMips_ 成员更简
    dev_->updateBuffer(frameUbo_, tp, sizeof(tp), 256);
    cmd->beginRenderPass(scene, clear_, /*loadContent=*/true);
    cmd->bindTexture(16, transTex_, /*mipmap 采样器*/ meshSamplerOfSceneTex());
    for (uint32_t idx : camVis)  // 先 transmission 后 blend(order 已排)
      if (tierOf(idx) == 1) {
        ctx.itemOffset = uint64_t(slotOf[idx]) * kUboStride;
        ctx.jointOffset = jointSlot_[idx] >= 0 ? uint64_t(jointSlot_[idx]) * kJointItemStride : 0;
        queue_[idx]->record(cmd, ctx);
      }
    for (uint32_t idx : camVis)
      if (tierOf(idx) == 2) {
        ctx.itemOffset = uint64_t(slotOf[idx]) * kUboStride;
        ctx.jointOffset = jointSlot_[idx] >= 0 ? uint64_t(jointSlot_[idx]) * kJointItemStride : 0;
        queue_[idx]->record(cmd, ctx);
      }
    cmd->endRenderPass();
  } else if (anyTrans) {
    RD_LOGW("renderer", "transmission 目标创建失败,透射项按 opaque 渲染");
  }
```
实现注意：
- `mipsOf`/mesh 采样器：Renderer 无 mesh sampler——init 时另建 `transSampler_ = dev_->createSampler({})`
  （linear+mipmap，与 mesh sampler 同状态）作 slot16 采样器；mips 存成员 `transMips_`。
- blend 项原本在单段 pass 内渲染,现移入 pass B;`anyTrans==false` 时 blend 项仍须渲染——
  **保持单段 pass 结构**：`!anyTrans` 时走现有单循环（含 blend,零回归路径）；
  上面骨架的 pass A 循环在 `!anyTrans` 时不应跳过 blend——实现为：
  pass A 循环条件 `tierOf(idx) == 0 || (!anyTransExpected && tierOf(idx) == 2)`,
  其中 `anyTransExpected` 在排序后即可判定（tier1 可见项存在且 transOn）。
  即:预期要拆分才在 pass A 跳过 transmission/blend;否则全走单段（与现状一致）。
- pass B 的 transmission 项管线选择在 record() 内按材质（alphaBlend→blendPipeline/
  skinned→skinnedPipeline/pbr）自动正确 ✓。
- `generateMipmaps` 是否可入 cmd 录制：现有签名 `Device::generateMipmaps(tex)` 是**立即执行**
  （Device 方法非 CommandBuffer）——现有调用方（env/ktx2）如何处理帧内时序,搜
  `generateMipmaps` 现有调用:若为帧外/立即场景,此处紧随 endRenderPass 后调用（录制器
  立即模式,GGUI 哲学下 cmd 与 device 同线程顺序执行,Metal 编码器已结束该 pass,GLES
  延迟回放会乱序！）——**GLES 须验证**：若 GLES 命令为延迟回放,generateMipmaps 需进
  录制队列。检查 gles generateMipmaps(1178 行)是否已走 cmds_ 队列;若否,给它加
  `std::lock_guard`/队列包装或在 GLES 端实现 `enqueueGenerateMipmaps`。实现时以
  「GLES 真机/模拟器跑 transmission_gallery 黄金图正确」为验收。
- 视锥剔除/ItemUBO 填充循环用 `order`,tier 不影响（透射项也占槽 ✓ 现逻辑不变）。

- [ ] **Step 5: 构建验证 + 手动冒烟（无透射资产 = 零回归路径）**

```bash
./scripts/check.sh && ctest --test-dir build --output-on-failure
./build/tools/render_test/render_test --backend metal --model build/../assets/DamagedHelmet.glb --out /tmp/helmet_t6.png 2>/dev/null || true
```
（现有测试资产无 transmission → 全部走单段零回归路径,全绿即验收。）

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "feat(renderer): 场景两段 pass(opaque→拷贝+mip→load 续画 transmission/blend)+三分区排序+render.transmission 门控"
```

---

### Task 7: shader 透射瓣 + transmission_gallery 演示场景

**Files:**
- Modify: `shaders/pbr_forward.frag`
- Modify: `shaders/pbr_forward_instanced.frag`
- Modify: `tools/render_test/scenes.cpp`（场景注册）
- Test: 手动/冒烟（golden 在 Task 8）

- [ ] **Step 1: pbr_forward.frag 新槽声明 + 透射瓣**

声明区（Task 1 的 19 行后）追加：

```glsl
layout(binding = 20) uniform texture2D texTransmissionScene;  // slot16:场景色拷贝(mip 链)
layout(binding = 21) uniform texture2D texTransmission;       // slot17:R=透射强度
layout(binding = 22) uniform texture2D texThickness;          // slot18:G=厚度
```
main() 内 clearcoat 块（167 行 `}` 后）插入：

```glsl
  // ---- KHR transmission + volume(默认 factor=0 → 跳过,零回归)----
  float transFactor = 0.0;
  vec3 transmitted = vec3(0.0);
  const bool transOn = ext3.x > 0.0;
  if (transOn) {
    transFactor = clamp(ext3.x * texture(sampler2D(texTransmission, smpMat), vUV).r, 0.0, 1.0);
    float thickness = ext3.y * texture(sampler2D(texThickness, smpMat), vUV).g;
    vec2 suv = gl_FragCoord.xy * transmissionParams.xy;
    vec3 refr = refract(-v, n, 1.0 / clamp(ext2.w, 1.001, 3.0));
    // 屏幕空间折射偏移:薄壁小偏移,厚度放大(Khronos viewer 投影近似;clamp 抵消 repeat)
    suv += refr.xy * max(thickness, 0.05) * transmissionParams.xy * 4.0;
    float lod = roughness * transmissionParams.z;
    transmitted = textureLod(sampler2D(texTransmissionScene, smpMat),
                             clamp(suv, vec2(0.0), vec2(1.0)), lod).rgb;
    if (ext3.z > 0.0 && thickness > 0.0) {  // Beer-Lambert 吸收
      vec3 atten = clamp(ext4.xyz, vec3(1e-4), vec3(1.0));
      transmitted *= exp(-log(atten) / ext3.z * thickness);
    }
  }
```
光循环内漫反射（209..210 行区域）追加抑制：

```glsl
    vec3 diffuse = baseColor.rgb * (1.0 - metallic) / PI;
    if (transOn) diffuse *= (1.0 - transFactor);  // 透射替换漫反射位
```
IBL 漫反射（171 行后）：

```glsl
  if (transOn) iblDiffuse *= (1.0 - transFactor);
```
分层组合（285 行 clearcoat 块 `}` 后、`vec3 color = base * ao + emissive;` 前）：

```glsl
  if (transOn) {  // 透射:入射面菲涅尔权重(高光保留在 base 中)
    float fTrans = f0d.x + (1.0 - f0d.x) * pow(1.0 - ndv, 5.0);
    base += transmitted * transFactor * (1.0 - fTrans);
  }
```

- [ ] **Step 2: pbr_forward_instanced.frag 同步**

同 Step 1 的声明追加 + 透射瓣（`ext3/ext4/transmitted` 经 `iu.items[vItem].ext3` 等——
该文件 uniform 块展开为 `items[vItem].xxx` 访问模式,按现有 ext0..2 的访问写法逐处对应;
注意 transmission 项不参与实例化分组（Task 6 排除）,此文件透射路径实际不触发,
保持同构仅为块大小一致）。

- [ ] **Step 3: transmission_gallery 场景**

`tools/render_test/scenes.cpp`：场景名单（19 行）追加 `"transmission_gallery"`；
注册分支（471 行 material_ext_gallery 旁）追加——

```cpp
  } else if (n == "transmission_gallery") {
    // 棋盘地板 + 三球:清玻璃(t=1,rough=0)/毛玻璃(t=1,rough=0.45)/红吸收
    // (t=1,rough=0,thickness=2,attenColor=(0.9,0.1,0.1),attenDist=0.5)
    // 地板:8x8 黑白棋盘(小盒网格,两种 baseColor 材质,primitives::makeBox)
    // 球:primitives::makeSphere + MaterialData 赋值 transmissionFactor 等
    // 相机/灯光参照 material_ext_gallery 取景
  }
```
（实现照抄 material_ext_gallery 分支的结构:ModelAsset 组装 → 材质字段赋值 →
相机自动取景;球半径 0.5 间距 1.2,地板 y=0 球心 y=0.5。）

- [ ] **Step 4: 冒烟验证**

```bash
cmake --build build --target render_test
./build/tools/render_test/render_test --backend metal --scene transmission_gallery --out /tmp/tg.png
./build/tools/img_check/img_check /tmp/tg.png --min-coverage 0.03
```
预期:img_check 通过;目视 /tmp/tg.png——三球应透出棋盘纹理,毛玻璃模糊,红球变暗偏红。
（vulkan 同跑一次。）

- [ ] **Step 5: 全量零回归 + Commit**

```bash
ctest --test-dir build --output-on-failure
git add -A && git commit -m "feat(shaders): KHR transmission/volume 透射瓣(屏幕折射 mip 模糊+Beer-Lambert)+ transmission_gallery 场景"
```

---

### Task 8: Khronos 资产 + golden 四件 + 门控语义测试

**Files:**
- Modify: `scripts/fetch_assets.sh`
- Test: `tests/renderer/material_transmission_test.cpp`（新增 + CMake 注册）
- Golden: `tests/golden/ext_transmission_{metal,vulkan}.png` 等 8 张（运行生成）

- [ ] **Step 1: fetch_assets 收录四模型**

`scripts/fetch_assets.sh` P4-A 段后追加：

```bash
# P4-B KHR transmission/volume golden 模型
dl TransmissionTest.glb TransmissionTest/glTF-Binary/TransmissionTest.glb
dl TransmissionRoughnessTest.glb TransmissionRoughnessTest/glTF-Binary/TransmissionRoughnessTest.glb
dl AttenuationTest.glb AttenuationTest/glTF-Binary/AttenuationTest.glb
dl MosquitoInAmber.glb MosquitoInAmber/glTF-Binary/MosquitoInAmber.glb
```
执行 `./scripts/fetch_assets.sh` 下载（弱网可重跑）。

- [ ] **Step 2: 门控语义测试 + golden 四件**

`tests/renderer/material_transmission_test.cpp`——整体拷贝 `material_ext_test.cpp` 骨架
（extPreset 改名 transPreset 并补 `q.transmission = 1;`；renderModel 的门控开关换
`setTransmissionEnabled`）。内容：

```cpp
// 1) 门控语义:程序化透射球(transmissionFactor=1 悬于棋盘上)开/关渲染应不同;
//    DamagedHelmet(无扩展)开/关应逐像素一致(零操作)。
TEST(MaterialTransmission, GateSemantics) {
  // renderModel(b, sphereOverChecker, true) != renderModel(b, sphereOverChecker, false)
  // renderModel(b, helmet, true) ≈ renderModel(b, helmet, false)(SSIM ≥ 0.99 或逐像素)
}
// 2) golden 四件(资产缺失自动 skip,参照 ext 测试的文件存在检查)
rd::test::Image renderTransmissionTest(rd::Backend b);   // assets/TransmissionTest.glb
rd::test::Image renderRoughnessTest(rd::Backend b);      // assets/TransmissionRoughnessTest.glb
rd::test::Image renderAttenuationTest(rd::Backend b);    // assets/AttenuationTest.glb
rd::test::Image renderAmber(rd::Backend b);              // assets/MosquitoInAmber.glb
RD_GOLDEN_TEST(MaterialTransmission, Test, "ext_transmission", 0.05, renderTransmissionTest)
RD_GOLDEN_TEST(MaterialTransmission, Roughness, "ext_transmission_roughness", 0.05, renderRoughnessTest)
RD_GOLDEN_TEST(MaterialTransmission, Volume, "ext_volume", 0.05, renderAttenuationTest)
RD_GOLDEN_TEST(MaterialTransmission, Amber, "ext_amber", 0.05, renderAmber)
```
（renderModel 内 QualityPreset 落 transmission 字段;模型加载用 loadGltf+MeshRenderResource::upload,
取景参照 ext 测试。）

- [ ] **Step 3: 生成新 golden + 验证**

```bash
cmake --build build && ctest --test-dir build -R "MaterialTransmission" --output-on-failure
RD_UPDATE_GOLDENS=1 ctest --test-dir build -R "MaterialTransmission"
ls tests/golden/ | grep -E "ext_transmission|ext_volume|ext_amber"
```
预期：8 张（4 场景 × 双后端）；目视核对——TransmissionTest 球阵应透出背景格、
RoughnessTest 粗糙行模糊、AttenuationTest 颜色随距离衰减、Amber 琥珀偏黄吸收。
再跑一次非 update 模式确认 SSIM 通过。

- [ ] **Step 4: 全量回归 + Commit**

```bash
ctest --test-dir build --output-on-failure
git add -A && git commit -m "feat(test): KHR transmission/volume golden 四件(TransmissionTest/Roughness/Attenuation/Amber 双后端)+ 门控语义测试 + fetch_assets 收录"
```

---

### Task 9: AGENTS.md 文档 + 收尾

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: AGENTS.md 更新**

「当前状态」追加一条 P4-B 完成条目（transmission/volume 两件套、两段 pass、分离采样器
16→5、已知限制）；「下一步」行改为 `P4-C(morph/Draco),或 P3(AR+鸿蒙)`。
「代码约定」更新：

- 纹理槽位段：追加 `16=transmissionScene(全局,pass A 占位/pass B 真图,Renderer 绑)、
  17=transmission(R)、18=thickness(G) —— 共 19 槽(GLES 真机普遍 32+)`；
  删除 Metal 折返 12..15 描述，改为 `Metal sampler 折返仅剩 smpMat:binding23→sampler(0)
  (分离采样器模型:sampler 描述符 5 个)`
- 新增段落「pbr 系分离采样器模型」：15 张 2D 纹理 texture2D + 共享 smpMat(binding23,
  mesh sampler 状态)；cube/lut/shadow combined(5..8)；Vulkan 双布局族
  （PipelineDesc.separateSamplers:pbr 族/blit 族）；GLES spirv-cross 合并名表
- UBO 约定段：ItemUBO 304B→336B(ext3/ext4)、FrameUBO 256B→272B(transmissionParams,
  range 272)
- 新增段落「transmission 两段 pass」：三分区排序、拷贝+mip 链、LoadOp、门控
  (render.transmission × 画质档)、已知限制（不互相折射/blend 不入折射/屏幕空间单次
  折射近似/Low 档退化 opaque）
- golden 段追加：ext_transmission/ext_transmission_roughness/ext_volume/ext_amber
  (fetch_assets 下载,缺失 skip)；交互 `--interactive --scene transmission_gallery`

- [ ] **Step 2: 全量验证**

```bash
./scripts/check.sh && ctest --test-dir build --output-on-failure
```

- [ ] **Step 3: Commit**

```bash
git add AGENTS.md && git commit -m "docs: P4-B transmission/volume 记入 AGENTS.md(19 槽/分离采样器/两段 pass/门控/已知限制)"
```

---

## 自审记录（Self-Review）

1. **Spec 覆盖**：spec §1 两段流程→Task 6；§2 loader→Task 3；§3.1 资源→Task 4；
   §3.2 ItemUBO→Task 5；§3.3 FrameUBO→Task 5；§3.4 分离采样器+槽位→Task 1/4/7；
   §4 LoadOp→Task 2；§5 shader→Task 7；§6 门控→Task 6；§7 测试→Task 2/3/8；
   §8 限制→Task 9 文档；§9 文档→Task 9。✓ 无缺口。
2. **占位符扫描**：Task 6 Step 4 骨架中 `mipsOf`/`meshSamplerOfSceneTex` 已在注释中
   落为具体成员方案（transSampler_/transMips_）；Task 8 的测试体为骨架+精确断言描述,
   素材来自 material_ext_test.cpp 现成样板拷贝——执行者按样板补全属常规移植非设计留白。✓
3. **类型一致性**：ext3/ext4 字段名、texTransmissionScene/texTransmission/texThickness、
   setTransmissionEnabled/transmissionManual_/transmissionQuality_、preserveContent/
   loadContent、separateSamplers 全文一致。✓
