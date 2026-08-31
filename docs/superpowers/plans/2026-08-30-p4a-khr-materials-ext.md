# P4-A KHR 材质扩展四件套 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 落地 glTF 材质扩展 clearcoat/sheen/specular/ior 四件套（分层 BRDF + ItemUBO 扩容 + 纹理槽 9→16 + 画质/选项门控），默认因子零操作、现有 golden 全量不动。

**Architecture:** uber-shader 均匀分支（扩展因子全默认时跳过纹理采样与瓣计算）；ItemUBO 块 256→304B、槽距 256→512B；纹理槽 9→16（恒绑定占位，同 shadow map 模式）；门控发生在 ItemUBO 填充处（关闭时写默认因子，shader 走均匀分支）。

**Tech Stack:** C++17 / GLSL 450(经 glslang→SPIR-V→spirv-cross 三后端) / cgltf / glm / ctest。

**Spec:** `docs/superpowers/specs/2026-08-27-khr-materials-ext-design.md`（已确认）

---

## 0. 背景与存量修复（调研发现）

实现本 spec 必须先修两个存量 bug（否则新槽位在三后端无法正确工作）：

1. **Vulkan 纹理槽实际只支持 8 个**：`DescriptorKey.texView[8]` + `bindTexture` 的
   `slot >= 8` 防护（vulkan_device.cpp:187/1947）——slot8（聚光阴影）绑定被静默丢弃，
   布局却声明了 9 个 sampler（binding 4..12）。聚光阴影 golden 此前在"未写描述符"
   的 UB 输出下生成。扩到 16 顺带修复；**helmet_spot_shadow Vulkan golden 可能需重生成**
   （Metal 侧本就正常，不受影响）。
2. **GLES 语义 sampler 名从未生效**：GLES 后端 bindTexture 按 `tex%u` 查 uniform，
   而 pbr_forward.frag 的 sampler 叫 `texBaseColor` 等 → `glGetUniformLocation` 返回 -1，
   所有 sampler uniform 保持默认 0 → **Android GLES 上 PBR 全部采样纹理单元 0**（存量 bug，
   Android 截图校验只有覆盖率门槛未暴露）。修复：createPipeline 链接后按"语义名→槽位"表
   `glUniform1i` 一次性写入（texN 命名的简单 shader 仍走 bind 期路径，两机制共存）。

## 0.1 与 spec 的偏差（已论证）

| # | 偏差 | 理由 |
|---|------|------|
| 1 | clearcoatNormal 缺省绑 `fallbackNormal_`（spec 原文统一白图） | factor>0 而无纹理时白图解码 (1,1,1) 是错误法线；平面法线占位=退化成基层法线，符合 glTF 语义；默认因子下同样零操作 |
| 2 | sheen 能量守恒 = 常数 `0.157`（three.js 惯例，Charlie 方向反照率无 LUT 拟合） | spec 允许"无 LUT 解析拟合"；direct 衰减与 IBL 同用；golden 自生成自洽 |
| 3 | 漫反射能量扣 `(1-specWeight×F)` 仅在扩展非默认时生效（均匀分支） | spec 的零回归硬约束要求默认材质逐像素不变；分支条件=因子非默认 |
| 4 | GLES 链接期语义 sampler 名表 | spec "sampler 名字表 +7 条"的落地形式；顺带修复存量 bug 2 |
| 5 | Vulkan DescriptorKey 8→16 | 修复存量 bug 1 的必然结果 |
| 6 | 实例化场景 pass 补绑 slot8（聚光阴影）与 9..15 | 该路径此前漏绑 slot8（Metal 靠编码器粘性碰巧可用） |

## 0.2 关键布局决定（全程有效）

- **纹理槽**：slot N ↔ Vulkan set0 binding(N+4) ↔ Metal texture(N+4) ↔ GLES 单元 N。
  新增 9=clearcoat(R)、10=clearcoatRough(G)、11=clearcoatNormal、12=sheenColor、
  13=sheenRough(A)、14=specularColor、15=specular(A)。
- **ItemUBO**：块 304B（3×mat4 + 4×vec4 + ext0/1/2），槽距 512B（256 对齐的下一档），
  缓冲 128 槽 × 512B = 64KB。ext 打包：
  - `ext0`: x=clearcoatFactor y=clearcoatRoughness z=clearcoatNormalScale w=specularFactor
  - `ext1`: xyz=sheenColorFactor w=sheenRoughnessFactor
  - `ext2`: xyz=specularColorFactor w=ior
- **实例化数组**：GLSL `Item` 补 `ext0..2 + vec4 _pad[13]` 到 512B（std140 数组元素
  stride 必须匹配 CPU 槽距），`items[64] → items[32]`（块=16KB，恰在 GLES 保证线），
  分组上限 64→32（超组拆分）。
- **零回归硬约束**：每任务后现有 golden 全量必须通过（唯一例外见 Task 1 的
  helmet_spot_shadow Vulkan 重生成）。

---

### Task 1: RHI 纹理槽 9→16（含 Vulkan/GLES 两个存量修复）

**Files:**
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`
- Modify: `core/rhi/backends/metal/metal_device.mm:290`
- Modify: `core/rhi/backends/gles/gles_device.cpp`
- Modify: `core/rhi/rhi_constants.inc.h:7`
- Modify: `core/rhi/rhi_types.h:251`

- [ ] **Step 1: Vulkan DescriptorKey 扩到 16**

`core/rhi/backends/vulkan/vulkan_device.cpp` 结构体（约 183 行）：

```cpp
/// 每 draw 的绑定状态 key(POD;memcmp 比较,须零初始化构造)。
struct DescriptorKey {
  VkBuffer ubo[4];
  uint64_t uboOffset[4];
  uint64_t uboSize[4];
  VkImageView texView[16];
  VkSampler texSampler[16];
  bool operator<(const DescriptorKey& o) const {
    return memcmp(this, &o, sizeof(DescriptorKey)) < 0;
  }
};
```

- [ ] **Step 2: Vulkan bindTexture 防护 8→16**（修复 slot8 丢弃）

同文件约 1943 行：

```cpp
/// 绑定约定：texture slot N ↔ set0 binding(N+4) combined-image-sampler；
/// 只记录绑定状态,descriptor set 在 draw 时按状态缓存命中/创建后绑定。
void VulkanCommandBuffer::bindTexture(uint32_t slot, TextureHandle texture,
                                      SamplerHandle sampler) {
  const TextureRec* rec = device_->texture(texture);
  VkSampler s = device_->sampler(sampler);
  if (!rec || s == VK_NULL_HANDLE || slot >= 16) return;
  bound_.texView[slot] = rec->view;
  bound_.texSampler[slot] = s;
}
```

- [ ] **Step 3: Vulkan 描述符布局/池扩到 16 sampler**

同文件 init 内（约 553-584 行）替换为：

```cpp
  // 描述符布局（绑定约定）：
  // binding 0..3：uniform buffer；binding 4..19：combined image sampler（texture slot 0..15）
  VkDescriptorSetLayoutBinding bindings[20]{};
  for (uint32_t i = 0; i < kMaxUniformSlots; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  for (uint32_t i = 0; i < 16; ++i) {
    bindings[4 + i].binding = 4 + i;
    bindings[4 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[4 + i].descriptorCount = 1;
    bindings[4 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dslci.bindingCount = 20;
  dslci.pBindings = bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &setLayout_));

  // 描述符池:按绑定状态缓存分配(每 draw 的实际绑定组合一个 set;
  // 容量 256 覆盖帧内异构绑定,超出时 descriptorSetFor 记错误日志)
  constexpr uint32_t kMaxDescSets = 256;
  VkDescriptorPoolSize poolSizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxUniformSlots * kMaxDescSets},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * kMaxDescSets},
  };
```

同文件 `setLayout_` 成员注释（约 378 行）改为：

```cpp
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;  ///< set0：binding 0..3 uniform + 4..19 sampler
```

- [ ] **Step 4: Vulkan descriptorSetFor 写入循环扩到 16**

同文件（约 604-645 行），数组与循环上限改：

```cpp
  VkWriteDescriptorSet writes[20]{};
  VkDescriptorBufferInfo uboInfos[4]{};
  VkDescriptorImageInfo imgInfos[16]{};
  uint32_t count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    // ……(不变)
  }
  for (uint32_t i = 0; i < 16; ++i) {
    if (key.texView[i] == VK_NULL_HANDLE) continue;
    imgInfos[i] = {key.texSampler[i], key.texView[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    // ……(写法不变:dstBinding = i + 4)
  }
```

（仅 `writes[20]`、`imgInfos[16]`、`i < 16` 三处数字变化。）

- [ ] **Step 5: Vulkan caps 9→16**

同文件约 477 行：

```cpp
    caps_.set(Capability::max_texture_slots, 16);  // slot0..15(KHR 扩展材质纹理)
```

- [ ] **Step 6: Metal caps 9→16**

`core/rhi/backends/metal/metal_device.mm` 约 290 行：

```objc
    caps_.set(Capability::max_texture_slots, 16);  // slot0..15(KHR 扩展材质纹理)
```

- [ ] **Step 7: GLES caps 9→16 + 链接期语义 sampler 名表**

`core/rhi/backends/gles/gles_device.cpp` 约 615 行：

```cpp
  caps_.set(Capability::max_texture_slots, 16);  // slot0..15(恰压 ES3 保证的 16 单元线)
```

`createPipeline` 内、glLinkProgram 成功检查之后、`kBlockTable` 循环之后（约 809 行）追加：

```cpp
  // 语义命名 sampler → slot 一次性写入(链接期;pbr_forward 系用描述性命名,
  // bindTexture 回放期的 tex%u 查表对它们无效——此前 GLES 上全部落单元 0,本表修复)。
  // texN 命名的简单 shader(blit/composite 等)仍由 bindTexture 回放期覆盖。
  static const struct {
    const char* name;
    uint32_t slot;
  } kSamplerTable[] = {
      {"texBaseColor", 0},     {"texMR", 1},           {"texNormal", 2},
      {"texEmissive", 3},      {"texOcclusion", 4},    {"texPrefilter", 5},
      {"texBrdfLut", 6},       {"texShadow", 7},       {"texShadowSpot", 8},
      {"texEquirect", 0},      {"texEnv", 0},          {"texClearcoat", 9},
      {"texClearcoatRough", 10}, {"texClearcoatNormal", 11}, {"texSheenColor", 12},
      {"texSheenRough", 13},   {"texSpecularColor", 14}, {"texSpecular", 15},
  };
  glUseProgram(program);
  for (const auto& s : kSamplerTable) {
    GLint loc = glGetUniformLocation(program, s.name);
    if (loc >= 0) glUniform1i(loc, GLint(s.slot));
  }
```

（注意：表含 Task 5 才新增的 texClearcoat 等 7 名——未命中只是 -1 跳过，无害。）

- [ ] **Step 8: 常量注释同步**

`core/rhi/rhi_constants.inc.h` 第 7 行：

```c
RD_CAPABILITY(max_texture_slots)        // 纹理槽数(绑定约定上限 16:slot0..15)
```

`core/rhi/rhi_types.h` 约 251 行注释改为：

```cpp
//   texture slot N(0..15) ↔ Metal texture/sampler(N+4) ↔ Vulkan set0 binding(N+4) combined-image-sampler
//                         ↔ GLES 纹理单元 N（sampler uniform:语义名表见 GLES createPipeline;texN 由 bindTexture 写）
```

- [ ] **Step 9: 构建 + 全量测试**

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

预期：全部通过；**若 `SpotShadow.GoldenVulkan` 失败**——这是 slot8 丢弃修复生效
（Vulkan 聚光阴影真正开始采样阴影图，不再依赖未写描述符的 UB 返回值）。确认后重生成：

```bash
RD_UPDATE_GOLDENS=1 ctest --test-dir build -R "SpotShadow" --output-on-failure
```

目视核对 `tests/golden/helmet_spot_shadow_vulkan.png`：聚光锥内应出现阴影、与
`helmet_spot_shadow_metal.png`（Metal 侧本就正确）画面趋势一致；确认差异方向
合理（阴影生效而非消失/全黑）再保留。

（若 golden 本就通过——MoltenVK 对未写描述符可能返回了恰好一致的结果——则无需重生成。）

- [ ] **Step 10: Android 侧编译 sanity（可选，须 `source /tmp/rd_env.sh`）**

```bash
cd samples/android && ./gradlew :app:assembleDebug
```

预期 BUILD SUCCESSFUL（GLES 改动只在 Android 编译）。

- [ ] **Step 11: Commit**

```bash
git add core/rhi/ tests/golden/helmet_spot_shadow_vulkan.png
git commit -m "fix(rhi): 纹理槽 9→16 扩容 + Vulkan slot8 丢弃修复(DescriptorKey/布局/池 16)+ GLES 语义 sampler 名表(修复 PBR 全落单元 0)"
```

---

### Task 2: Loader — MaterialData 四扩展字段 + readMaterial 解析（TDD）

**Files:**
- Modify: `core/resource/gltf_loader.h:36-47`（MaterialData）
- Modify: `core/resource/gltf_loader.cpp:96-140`（readMaterial）
- Test: `tests/resource/gltf_test.cpp`

- [ ] **Step 1: 写失败的单测**

`tests/resource/gltf_test.cpp` 末尾追加（需在文件头部 include 区加
`#include "common/image.h"` 与 `#include <filesystem>`——后者已有别名 fs 则复用）：

```cpp
// KHR 材质扩展四件套:因子/纹理/法线 scale 解析
TEST(Gltf, ExtMaterials) {
  const char* gltf = R"({
    "asset": {"version": "2.0"},
    "extensionsUsed": ["KHR_materials_clearcoat","KHR_materials_sheen",
                       "KHR_materials_specular","KHR_materials_ior"],
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                "indices": 1, "material": 0}]}],
    "materials": [{"extensions": {
      "KHR_materials_clearcoat": {"clearcoatFactor": 0.8,
        "clearcoatRoughnessFactor": 0.25,
        "clearcoatTexture": {"index": 0},
        "clearcoatRoughnessTexture": {"index": 0},
        "clearcoatNormalTexture": {"index": 0, "scale": 0.6}},
      "KHR_materials_sheen": {"sheenColorFactor": [0.5, 0.6, 0.7],
        "sheenRoughnessFactor": 0.4,
        "sheenColorTexture": {"index": 0},
        "sheenRoughnessTexture": {"index": 0}},
      "KHR_materials_specular": {"specularFactor": 0.7,
        "specularColorFactor": [0.9, 0.8, 0.7],
        "specularColorTexture": {"index": 0},
        "specularTexture": {"index": 0}},
      "KHR_materials_ior": {"ior": 1.33}
    }}],
    "textures": [{"source": 0}],
    "images": [{"uri": "ext.png"}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  const std::string dir = (std::filesystem::temp_directory_path() / "rd_gltf_extmat").string();
  std::filesystem::create_directories(dir);
  { FILE* f = fopen((dir + "/tri.gltf").c_str(), "w"); fputs(gltf, f); fclose(f); }
  { FILE* f = fopen((dir + "/tri.bin").c_str(), "wb");
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const uint16_t idx[3] = {0, 1, 2};
    fwrite(pos, 4, 9, f); fwrite(idx, 2, 3, f); fclose(f); }
  const uint8_t px[16] = {255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,255,255};
  ASSERT_TRUE(rd::test::savePNG(dir + "/ext.png", 2, 2, px));
  auto model = rd::loadGltf((dir + "/tri.gltf").c_str());
  ASSERT_TRUE(model.valid());
  const auto& m = model.meshes[0].material;
  EXPECT_FLOAT_EQ(m.clearcoatFactor, 0.8f);
  EXPECT_FLOAT_EQ(m.clearcoatRoughnessFactor, 0.25f);
  EXPECT_FLOAT_EQ(m.clearcoatNormalScale, 0.6f);
  EXPECT_FLOAT_EQ(m.sheenColorFactor[0], 0.5f);
  EXPECT_FLOAT_EQ(m.sheenColorFactor[2], 0.7f);
  EXPECT_FLOAT_EQ(m.sheenRoughnessFactor, 0.4f);
  EXPECT_FLOAT_EQ(m.specularFactor, 0.7f);
  EXPECT_FLOAT_EQ(m.specularColorFactor[0], 0.9f);
  EXPECT_FLOAT_EQ(m.ior, 1.33f);
  EXPECT_EQ(m.clearcoat.width, 2u);        // 外链 URI 走 decodeImage
  EXPECT_EQ(m.clearcoatRough.width, 2u);
  EXPECT_EQ(m.clearcoatNormal.width, 2u);
  EXPECT_EQ(m.sheenColor.width, 2u);
  EXPECT_EQ(m.sheenRough.width, 2u);
  EXPECT_EQ(m.specularColorTex.width, 2u);
  EXPECT_EQ(m.specularTex.width, 2u);
}

// 默认零操作语义:无扩展材质 → 全部默认值(渲染零回归的解析侧依据)
TEST(Gltf, ExtMaterialsDefaults) {
  const char* gltf = R"({
    "asset": {"version": "2.0"},
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0},
                                "indices": 1, "material": 0}]}],
    "materials": [{}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  const std::string dir = (std::filesystem::temp_directory_path() / "rd_gltf_extmat_d").string();
  std::filesystem::create_directories(dir);
  { FILE* f = fopen((dir + "/tri.gltf").c_str(), "w"); fputs(gltf, f); fclose(f); }
  { FILE* f = fopen((dir + "/tri.bin").c_str(), "wb");
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const uint16_t idx[3] = {0, 1, 2};
    fwrite(pos, 4, 9, f); fwrite(idx, 2, 3, f); fclose(f); }
  auto model = rd::loadGltf((dir + "/tri.gltf").c_str());
  ASSERT_TRUE(model.valid());
  const auto& m = model.meshes[0].material;
  EXPECT_FLOAT_EQ(m.clearcoatFactor, 0.0f);
  EXPECT_FLOAT_EQ(m.clearcoatRoughnessFactor, 0.0f);
  EXPECT_FLOAT_EQ(m.clearcoatNormalScale, 1.0f);
  EXPECT_FLOAT_EQ(m.sheenColorFactor[0], 0.0f);
  EXPECT_FLOAT_EQ(m.sheenRoughnessFactor, 0.0f);
  EXPECT_FLOAT_EQ(m.specularFactor, 1.0f);
  EXPECT_FLOAT_EQ(m.specularColorFactor[0], 1.0f);
  EXPECT_FLOAT_EQ(m.ior, 1.5f);
  EXPECT_EQ(m.clearcoat.width, 0u);   // 无纹理 → 无效 ImageData
  EXPECT_EQ(m.sheenColor.width, 0u);
  EXPECT_EQ(m.specularTex.width, 0u);
}
```

- [ ] **Step 2: 运行确认失败**

```bash
cmake --build build -j && ctest --test-dir build -R "Gltf.ExtMaterials" --output-on-failure
```

预期：编译失败（`clearcoatFactor` 等成员不存在）——即"失败"状态。

- [ ] **Step 3: MaterialData 新字段**

`core/resource/gltf_loader.h` MaterialData（在 `float alphaCutoff ...` 之后、结构体闭合前追加）：

```cpp
  // ---- KHR 材质扩展四件套(默认值 = 零操作语义:渲染与无扩展逐像素一致)----
  // KHR_materials_clearcoat
  ImageData clearcoat;           float clearcoatFactor = 0.0f;
  ImageData clearcoatRough;      float clearcoatRoughnessFactor = 0.0f;
  ImageData clearcoatNormal;      float clearcoatNormalScale = 1.0f;
  // KHR_materials_sheen
  ImageData sheenColor;          float sheenColorFactor[3] = {0, 0, 0};
  ImageData sheenRough;          float sheenRoughnessFactor = 0.0f;
  // KHR_materials_specular(specular 因子在纹理 A 通道)
  ImageData specularColorTex;    float specularColorFactor[3] = {1, 1, 1};
  ImageData specularTex;         float specularFactor = 1.0f;
  // KHR_materials_ior(独立于 specular 生效;1.5 → f0=0.04 与现状一致)
  float ior = 1.5f;
```

- [ ] **Step 4: readMaterial 解析**

`core/resource/gltf_loader.cpp` readMaterial 内，occlusion 块之后、`return m;` 之前追加：

```cpp
  // KHR_materials_clearcoat:清漆层(独立 GGX 瓣 + 独立法线/粗糙度)
  if (mat->has_clearcoat) {
    const auto& cc = mat->clearcoat;
    m.clearcoatFactor = float(cc.clearcoat_factor);
    m.clearcoatRoughnessFactor = float(cc.clearcoat_roughness_factor);
    if (cc.clearcoat_texture.texture)
      m.clearcoat = decodeImage(cc.clearcoat_texture.texture, gltfDir, pref);
    if (cc.clearcoat_roughness_texture.texture)
      m.clearcoatRough = decodeImage(cc.clearcoat_roughness_texture.texture, gltfDir, pref);
    if (cc.clearcoat_normal_texture.texture) {
      m.clearcoatNormal = decodeImage(cc.clearcoat_normal_texture.texture, gltfDir, pref);
      m.clearcoatNormalScale = float(cc.clearcoat_normal_texture.scale);
    }
  }
  // KHR_materials_sheen:织物绒面(Charlie 分布)
  if (mat->has_sheen) {
    const auto& sh = mat->sheen;
    for (int c = 0; c < 3; ++c) m.sheenColorFactor[c] = float(sh.sheen_color_factor[c]);
    m.sheenRoughnessFactor = float(sh.sheen_roughness_factor);
    if (sh.sheen_color_texture.texture)
      m.sheenColor = decodeImage(sh.sheen_color_texture.texture, gltfDir, pref);
    if (sh.sheen_roughness_texture.texture)
      m.sheenRough = decodeImage(sh.sheen_roughness_texture.texture, gltfDir, pref);
  }
  // KHR_materials_specular:介质高光强度/颜色
  if (mat->has_specular) {
    const auto& sp = mat->specular;
    m.specularFactor = float(sp.specular_factor);
    for (int c = 0; c < 3; ++c) m.specularColorFactor[c] = float(sp.specular_color_factor[c]);
    if (sp.specular_color_texture.texture)
      m.specularColorTex = decodeImage(sp.specular_color_texture.texture, gltfDir, pref);
    if (sp.specular_texture.texture)
      m.specularTex = decodeImage(sp.specular_texture.texture, gltfDir, pref);
  }
  // KHR_materials_ior:折射率 → 介质 f0
  if (mat->has_ior) m.ior = float(mat->ior.ior);
```

- [ ] **Step 5: 运行测试通过**

```bash
cmake --build build -j && ctest --test-dir build -R "Gltf" --output-on-failure
```

预期：全部 Gltf 用例 PASS。

- [ ] **Step 6: Commit**

```bash
git add core/resource/gltf_loader.h core/resource/gltf_loader.cpp tests/resource/gltf_test.cpp
git commit -m "feat(resource): glTF 材质扩展四件套解析(clearcoat/sheen/specular/ior;默认值零操作语义)+ 单测"
```

---

### Task 3: GPU 资源 — MeshGpuData 7 个扩展纹理

**Files:**
- Modify: `core/resource/mesh_render_resource.h:15-27`
- Modify: `core/resource/mesh_render_resource.cpp`

- [ ] **Step 1: MeshGpuData 新纹理句柄**

`core/resource/mesh_render_resource.h` MeshGpuData，`occlusionTex` 之后、`skinned` 之前追加：

```cpp
  // ---- KHR 扩展四件套纹理(缺省占位:恒有效,renderer 恒绑定)----
  TextureHandle clearcoatTex;      // 缺省绑 1x1 白(factor 默认 0 压制贡献)
  TextureHandle clearcoatRoughTex; // 缺省绑 1x1 白
  TextureHandle clearcoatNormalTex;  // 缺省绑平面法线(=退化基层法线,glTF 语义)
  TextureHandle sheenColorTex;     // 缺省绑 1x1 白
  TextureHandle sheenRoughTex;     // 缺省绑 1x1 白
  TextureHandle specularColorTex;  // 缺省绑 1x1 白(白 × factor(1,1,1) = 恒等)
  TextureHandle specularTex;       // 缺省绑 1x1 白(A=1 → 因子直通)
```

- [ ] **Step 2: 上传接线**

`core/resource/mesh_render_resource.cpp` upload 循环内，`g.occlusionTex = ...` 之后追加：

```cpp
    g.clearcoatTex = uploadOr(dev, m.material.clearcoat, res->fallbackWhite_);
    g.clearcoatRoughTex = uploadOr(dev, m.material.clearcoatRough, res->fallbackWhite_);
    g.clearcoatNormalTex = uploadOr(dev, m.material.clearcoatNormal, res->fallbackNormal_);
    g.sheenColorTex = uploadOr(dev, m.material.sheenColor, res->fallbackWhite_);
    g.sheenRoughTex = uploadOr(dev, m.material.sheenRough, res->fallbackWhite_);
    g.specularColorTex = uploadOr(dev, m.material.specularColorTex, res->fallbackWhite_);
    g.specularTex = uploadOr(dev, m.material.specularTex, res->fallbackWhite_);
```

同处 pixels 清空列表（`m.material.occlusion.pixels.clear();` 之后）追加：

```cpp
    m.material.clearcoat.pixels.clear();
    m.material.clearcoatRough.pixels.clear();
    m.material.clearcoatNormal.pixels.clear();
    m.material.sheenColor.pixels.clear();
    m.material.sheenRough.pixels.clear();
    m.material.specularColorTex.pixels.clear();
    m.material.specularTex.pixels.clear();
```

有效性检查（`!g.occlusionTex.valid()` 条件追加）：

```cpp
    if (!g.vbo.valid() || !g.ibo.valid() || !g.baseColorTex.valid() || !g.mrTex.valid() ||
        !g.normalTex.valid() || !g.emissiveTex.valid() || !g.occlusionTex.valid() ||
        !g.clearcoatTex.valid() || !g.clearcoatRoughTex.valid() ||
        !g.clearcoatNormalTex.valid() || !g.sheenColorTex.valid() ||
        !g.sheenRoughTex.valid() || !g.specularColorTex.valid() || !g.specularTex.valid()) {
      res->destroy(dev);
      return nullptr;
    }
```

- [ ] **Step 3: destroy 释放列表**

`MeshRenderResource::destroy` 内纹理循环改为：

```cpp
    for (TextureHandle t : {g.baseColorTex, g.mrTex, g.normalTex, g.emissiveTex,
                            g.occlusionTex, g.clearcoatTex, g.clearcoatRoughTex,
                            g.clearcoatNormalTex, g.sheenColorTex, g.sheenRoughTex,
                            g.specularColorTex, g.specularTex}) {
      // 只销毁非占位纹理(占位纹理由本对象统一销毁)
      if (t.valid() && t != fallbackWhite_ && t != fallbackBlack_ && t != fallbackNormal_)
        dev.destroyTexture(t);
    }
```

- [ ] **Step 4: 构建 + 全量测试（行为无变化——占位纹理尚未被采样）**

```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```

预期：全部通过（golden 不变）。

- [ ] **Step 5: Commit**

```bash
git add core/resource/mesh_render_resource.h core/resource/mesh_render_resource.cpp
git commit -m "feat(resource): MeshGpuData 扩展材质 7 纹理上传(clearcoatNormal 缺省平面法线,余白图;恒有效占位)"
```

---

### Task 4: ItemUBO 512B 槽距 + 扩展因子填充 + record 绑定 + 实例化组上限 32

**Files:**
- Modify: `core/renderer/renderable.h`
- Modify: `core/renderer/renderer.h:100-103`
- Modify: `core/renderer/renderer.cpp`
- Modify: `core/renderer/mesh_renderable.cpp`

- [ ] **Step 1: 布局常量（renderable.h）**

`core/renderer/renderable.h` 在 `namespace rd {` 打开后、`RenderContext` 之前插入：

```cpp
/// ItemUBO 布局常量(P4-A:块 304B,槽距 512B;renderer/mesh_renderable 共用,
/// 改动须与 renderer.cpp 的 ItemUBOData static_assert 联动)。
inline constexpr uint32_t kItemUboStride = 512;   ///< 槽距(256 对齐下一档,三后端 minUniformBufferOffsetAlignment)
inline constexpr uint32_t kItemUboSize = 304;     ///< 块大小 = sizeof(ItemUBOData)
inline constexpr uint32_t kItemUboMaxSlots = 128; ///< UBO 缓冲槽位(per-mesh)
```

- [ ] **Step 2: renderer.h 常量改引 + 组上限**

```cpp
  static constexpr uint32_t kUboStride = kItemUboStride;      // 512(块 304B)
  static constexpr uint32_t kMaxItems = 64;     // 渲染项(模型)容量
  static constexpr uint32_t kMaxItemSlots = kItemUboMaxSlots; // 128 槽 ×512B=64KB
  static constexpr uint32_t kMaxInstGroup = 32; // 实例化组上限(items[32],16KB 线)
```

- [ ] **Step 3: ItemUBOData 扩 304B（renderer.cpp）**

```cpp
/// ItemUBO 布局(304B 块,槽距 512B):mvp|world|normalMatrix|baseColorFactor|
/// emissiveOcc|metalRough|uvTf|ext0|ext1|ext2
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
};
static_assert(sizeof(ItemUBOData) == kItemUboSize, "ItemUBO 必须 304B(槽距 512)");
```

- [ ] **Step 4: 填充扩展因子（renderer.cpp endScene 的 per-mesh 填充循环）**

在 `iu.uvTransform[3] = m.uvScale[1];` 之后、`} else {` 之前不动；把整个
`if (!meshes.empty() && mi < meshes.size()) {...} else {...}` 块之后追加（即 ext 填充
统一覆盖两种情况）：

```cpp
      // ---- KHR 扩展四件套因子(关闭/无扩展时写默认 = 零操作;禁用置 extOn=false)----
      if (extOn && !meshes.empty() && mi < meshes.size()) {
        const auto& mm = meshes[mi].material;
        iu.ext0[0] = mm.clearcoatFactor;
        iu.ext0[1] = mm.clearcoatRoughnessFactor;
        iu.ext0[2] = mm.clearcoatNormalScale;
        iu.ext0[3] = mm.specularFactor;
        iu.ext1[0] = mm.sheenColorFactor[0];
        iu.ext1[1] = mm.sheenColorFactor[1];
        iu.ext1[2] = mm.sheenColorFactor[2];
        iu.ext1[3] = mm.sheenRoughnessFactor;
        iu.ext2[0] = mm.specularColorFactor[0];
        iu.ext2[1] = mm.specularColorFactor[1];
        iu.ext2[2] = mm.specularColorFactor[2];
        iu.ext2[3] = mm.ior;
      } else {
        iu.ext0[2] = 1.0f;   // clearcoatNormalScale 默认
        iu.ext0[3] = 1.0f;   // specularFactor 默认
        iu.ext2[0] = iu.ext2[1] = iu.ext2[2] = 1.0f;  // specularColorFactor 默认
        iu.ext2[3] = 1.5f;   // ior 默认(f0=0.04 与现状一致)
      }
```

填充循环之前（`// 统一填充 per-mesh ItemUBO` 注释行处）定义：

```cpp
  const bool extOn = extMaterialsManual_ && extMaterialsQuality_;  // 与关系门控
```

（本任务先加成员：renderer.h private 区追加
`bool extMaterialsManual_ = true;` 与 `bool extMaterialsQuality_ = false;`，
public 区追加 setter——Task 6 才接线选项，此处成员默认值保证行为=当前。）

```cpp
  /// KHR 扩展材质四件套开关(选项 render.ext_materials;与画质档为与关系)。
  void setExtMaterialsEnabled(bool on) { extMaterialsManual_ = on; }
```

- [ ] **Step 5: 实例化分组上限 32（两处 while）**

阴影 pass 分组（`while (groupEnd < lightVis.size())`）加条件：

```cpp
        while (groupEnd < lightVis.size() && groupEnd - li < kMaxInstGroup) {
```

场景 pass 分组（`while (groupEnd < camVis.size())`）加条件：

```cpp
      while (groupEnd < camVis.size() && groupEnd - vi < kMaxInstGroup) {
```

- [ ] **Step 6: 场景 pass 实例化路径补绑 slot8..15（renderer.cpp）**

实例化组纹理绑定循环内（`if (shadowActive) cmd->bindTexture(7, ...)` 之后）：

```cpp
        if (spotShadowActive) cmd->bindTexture(8, spotShadowDepthTex_, shadowSampler_);
        // KHR 扩展材质纹理(9..15;恒绑定,占位由资源层保证)
        cmd->bindTexture(9, g.clearcoatTex, sampler);
        cmd->bindTexture(10, g.clearcoatRoughTex, sampler);
        cmd->bindTexture(11, g.clearcoatNormalTex, sampler);
        cmd->bindTexture(12, g.sheenColorTex, sampler);
        cmd->bindTexture(13, g.sheenRoughTex, sampler);
        cmd->bindTexture(14, g.specularColorTex, sampler);
        cmd->bindTexture(15, g.specularTex, sampler);
```

- [ ] **Step 7: mesh_renderable.cpp 槽距/范围/绑定**

文件头注释后整体替换关键片段：

```cpp
void MeshRenderable::record(CommandBuffer* cmd, const RenderContext& ctx) {
  const bool skinned = !mesh_->meshes().empty() && mesh_->meshes()[0].skinned;
  // per-mesh ItemUBO 槽:itemOffset + meshIdx×512(容量截断时钳到末槽=mesh0 兜底语义)
  const uint64_t kLastSlot = uint64_t(kItemUboMaxSlots - 1) * kItemUboStride;
  uint32_t meshIdx = 0;
  auto itemOff = [&]() {
    const uint64_t o = ctx.itemOffset + uint64_t(meshIdx) * kItemUboStride;
    return o < kLastSlot ? o : kLastSlot;
  };
  // KHR 扩展材质纹理(9..15;pbr/blend/skinned 三路径共用,恒绑定)
  auto bindExtTextures = [&](const MeshGpuData& g) {
    cmd->bindTexture(9, g.clearcoatTex, mesh_->sampler());
    cmd->bindTexture(10, g.clearcoatRoughTex, mesh_->sampler());
    cmd->bindTexture(11, g.clearcoatNormalTex, mesh_->sampler());
    cmd->bindTexture(12, g.sheenColorTex, mesh_->sampler());
    cmd->bindTexture(13, g.sheenRoughTex, mesh_->sampler());
    cmd->bindTexture(14, g.specularColorTex, mesh_->sampler());
    cmd->bindTexture(15, g.specularTex, mesh_->sampler());
  };
```

阴影路径 bind 改 range：`cmd->bindUniformBuffer(1, ctx.itemUbo, itemOff(), 256);` →

```cpp
    cmd->bindUniformBuffer(1, ctx.itemUbo, itemOff(), kItemUboSize);
```

blend/skinned/pbr 三路径的 `bindUniformBuffer(1, ctx.itemUbo, itemOff(), 256)` 同样改
`kItemUboSize`；三路径各自的 `if (ctx.shadowSpotMap.valid()) cmd->bindTexture(8, ...)`
之后追加一行：

```cpp
      bindExtTextures(g);
```

（unlit 路径 `bindUniformBuffer(1, ctx.itemUbo, itemOff(), 64)` 保持 64——只读 mvp。）

- [ ] **Step 8: 构建 + 全量 golden 回归**

```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```

预期：全部通过（shader 尚未读 ext 字段/新纹理；槽距变化对现有单-item 绑定语义等价）。

- [ ] **Step 9: Commit**

```bash
git add core/renderer/renderable.h core/renderer/renderer.h core/renderer/renderer.cpp core/renderer/mesh_renderable.cpp
git commit -m "feat(renderer): ItemUBO 304B 块/512B 槽距 + 扩展因子填充(默认零操作)+ 实例化组上限 32 + record 恒绑 9..15"
```

---

### Task 5: Shader 分层 BRDF（5 个文件）

**Files:**
- Modify: `shaders/pbr_forward.vert`
- Modify: `shaders/pbr_forward_skinned.vert`
- Modify: `shaders/pbr_forward.frag`（完整重写，见下）
- Modify: `shaders/pbr_forward_instanced.vert`
- Modify: `shaders/pbr_forward_instanced.frag`（完整重写，见下）
- Modify: `shaders/shadow_depth_instanced.vert`

- [ ] **Step 1: 两个顶点 shader 的 ItemUBO 声明扩 3 vec4**

GLES 要求同名 uniform block 跨阶段声明一致——pbr_forward.vert 与
pbr_forward_skinned.vert 的 ItemUBO 块各追加（frag 同步后三处一致）：

```glsl
  vec4 uvTransform;         // xy=offset, zw=scale
  vec4 ext0;  // x=clearcoatFactor y=clearcoatRoughness z=clearcoatNormalScale w=specularFactor
  vec4 ext1;  // xyz=sheenColorFactor w=sheenRoughnessFactor
  vec4 ext2;  // xyz=specularColorFactor w=ior
};
```

（vert 不读 ext 字段，纯声明对齐。）

- [ ] **Step 2: pbr_forward.frag 完整替换**

```glsl
// pbr_forward.frag：glTF metallic-roughness + 多光源(LightUBO) + 阴影(PCF) + IBL
//   + KHR 扩展四件套(clearcoat/sheen/specular/ior;默认因子 = 零操作,零回归)。
// slot：0=baseColor(b4) 1=MR(b5) 2=normal(b6) 3=emissive(b7) 4=occlusion(b8)
//       5=prefilterCube(b9) 6=brdfLut(b10) 7=shadowMap(b11) 8=spotShadow(b12)
//       9=clearcoat(b13) 10=clearcoatRough(b14) 11=clearcoatNormal(b15)
//       12=sheenColor(b16) 13=sheenRough(b17) 14=specularColor(b18) 15=specular(b19);
//       FrameUBO(b0) ItemUBO(b1,304B) LightUBO(b2)。
#version 450
layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec4 vTangent;
layout(location = 3) in vec2 vUV;

layout(binding = 0) uniform FrameUBO {
  mat4 viewProj;
  vec4 cameraPos;
  vec4 lightDir;      // 占位(多光源后由 LightUBO 接管)
  vec4 lightColor;    // 占位
  vec4 sh[9];         // xyz=SH 系数（Ã 已折叠）
};
layout(binding = 1) uniform ItemUBO {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;
  vec4 uvTransform;
  vec4 ext0;  // x=clearcoatFactor y=clearcoatRoughness z=clearcoatNormalScale w=specularFactor
  vec4 ext1;  // xyz=sheenColorFactor w=sheenRoughnessFactor
  vec4 ext2;  // xyz=specularColorFactor w=ior
};
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
layout(binding = 13) uniform sampler2D texClearcoat;       // slot9:R=清漆强度
layout(binding = 14) uniform sampler2D texClearcoatRough;  // slot10:G=清漆粗糙度
layout(binding = 15) uniform sampler2D texClearcoatNormal; // slot11:清漆法线(缺省平面法线占位)
layout(binding = 16) uniform sampler2D texSheenColor;      // slot12:RGB
layout(binding = 17) uniform sampler2D texSheenRough;      // slot13:A=粗糙度
layout(binding = 18) uniform sampler2D texSpecularColor;   // slot14:RGB
layout(binding = 19) uniform sampler2D texSpecular;        // slot15:A=specular 因子

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
  vec4 baseColor = texture(texBaseColor, vUV) * baseColorFactor;
  // alphaMode=MASK:cutoff(metallicRoughness.w)> 0 时按阈值裁剪
  if (metallicRoughness.w > 0.0 && baseColor.a < metallicRoughness.w) discard;
  vec2 mr = texture(texMR, vUV).bg;   // glTF: G=roughness, B=metallic
  float metallic = clamp(mr.y * metallicRoughness.x, 0.0, 1.0);
  float roughness = clamp(mr.x * metallicRoughness.y, 0.03, 1.0);

  // 法线贴图(TBN;清漆法线共享此切线空间)
  vec3 n = normalize(vNormal);
  vec3 t = normalize(vTangent.xyz - n * dot(n, vTangent.xyz));
  vec3 b = cross(n, t) * vTangent.w;
  vec3 nMap = (texture(texNormal, vUV).xyz * 2.0 - 1.0) *
              vec3(metallicRoughness.z, metallicRoughness.z, 1.0);
  n = normalize(t * nMap.x + b * nMap.y + n * nMap.z);

  vec3 v = normalize(cameraPos.xyz - vWorldPos);
  vec3 r = reflect(-v, n);
  float ndv = clamp(dot(n, v), 0.0, 1.0);

  // ---- KHR_materials_specular + ior:介质 f0 改造(全默认 → 跳过,零回归)----
  float specWeight = 1.0;
  vec3 f0d = vec3(0.04);
  const bool specIor = ext0.w != 1.0 || ext2.w != 1.5 ||
                       ext2.x != 1.0 || ext2.y != 1.0 || ext2.z != 1.0;
  if (specIor) {
    specWeight = clamp(ext0.w * texture(texSpecular, vUV).a, 0.0, 1.0);
    vec3 specColor =
        clamp(ext2.xyz * texture(texSpecularColor, vUV).rgb, vec3(0.0), vec3(1.0));
    float k = (1.0 - ext2.w) / (1.0 + ext2.w);
    f0d = min(k * k * specColor, vec3(1.0));
  }
  vec3 f0 = mix(f0d, baseColor.rgb, metallic);

  // ---- KHR_materials_sheen(默认 sheenColorFactor=0 → 跳过)----
  vec3 sheenColor = vec3(0.0);
  float sheenRough = 0.0;
  const bool sheenOn = max(max(ext1.x, ext1.y), ext1.z) > 0.0;
  if (sheenOn) {
    sheenColor = ext1.xyz * texture(texSheenColor, vUV).rgb;
    sheenRough = clamp(ext1.w * texture(texSheenRough, vUV).a, 0.03, 1.0);
  }

  // ---- KHR_materials_clearcoat(默认 factor=0 → 跳过)----
  float ccFactor = 0.0, ccRough = 0.0;
  vec3 ncc = n;
  const bool ccOn = ext0.x > 0.0;
  if (ccOn) {
    ccFactor = clamp(ext0.x * texture(texClearcoat, vUV).r, 0.0, 1.0);
    ccRough = clamp(ext0.y * texture(texClearcoatRough, vUV).g, 0.03, 1.0);
    vec3 nMapCc = (texture(texClearcoatNormal, vUV).xyz * 2.0 - 1.0) *
                  vec3(ext0.z, ext0.z, 1.0);
    ncc = normalize(t * nMapCc.x + b * nMapCc.y + n * nMapCc.z);
  }

  // IBL(基层):SH diffuse + prefilter specular(split-sum)
  vec3 irradiance = evalIrradiance(n);
  vec3 iblDiffuse = irradiance * baseColor.rgb * (1.0 - metallic);
  vec3 prefiltered = textureLod(texPrefilter, r, roughness * (PREFILTER_MIPS - 1.0)).rgb;
  vec2 brdf = texture(texBrdfLut, vec2(ndv, roughness)).rg;
  vec3 Fenv = f0 * brdf.x + brdf.y;
  vec3 iblSpec = prefiltered * Fenv;
  if (specIor) iblDiffuse *= (vec3(1.0) - specWeight * Fenv);  // 介质漫反射能量扣

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

  float ao = mix(1.0, texture(texOcclusion, vUV).r, emissiveOcclusion.a);
  vec3 emissive = texture(texEmissive, vUV).rgb * emissiveOcclusion.rgb;

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
  vec3 color = base * ao + emissive;
  if (lightCount.y > 0.5) {
    outColor = vec4(color, 1.0);  // hdrMode:线性输出,tone mapping 在 composite
  } else {
    color = color / (color + vec3(1.0));          // LDR:Reinhard(现状)
    outColor = vec4(pow(color, vec3(1.0 / 2.2)), 1.0);
  }
}
```

- [ ] **Step 3: pbr_forward_instanced.vert — Item 补齐到 512B + items[32]**

struct Item 与声明替换为（其余不动）：

```glsl
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
  vec4 _pad[13];  // std140 数组元素 stride 对齐 CPU 槽距 512B(304+208)
};
layout(binding = 1) uniform ItemUBO { Item items[32]; } iu;  // 组上限 32(16KB 线)
```

- [ ] **Step 4: pbr_forward_instanced.frag 完整替换**

与 Step 2 的 pbr_forward.frag 一致，仅四处不同：

1. 头注释第 2 行改为 `// pbr_forward_instanced.frag:pbr_forward 的实例化变体;ItemUBO 按 vItem 索引。`
2. `layout(location = 4) flat in uint vItem;` 保留在输入声明区；
3. ItemUBO 块改为：

```glsl
struct Item {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;
  vec4 uvTransform;
  vec4 ext0;  // x=clearcoatFactor y=clearcoatRoughness z=clearcoatNormalScale w=specularFactor
  vec4 ext1;  // xyz=sheenColorFactor w=sheenRoughnessFactor
  vec4 ext2;  // xyz=specularColorFactor w=ior
  vec4 _pad[13];  // std140 数组元素 stride 对齐 CPU 槽距 512B
};
layout(binding = 1) uniform ItemUBO { Item items[32]; } iu;
```

4. main() 内所有 uniform 成员访问加 `iu.items[vItem].` 前缀，逐项对应：
   - `baseColorFactor` → `iu.items[vItem].baseColorFactor`
   - `metallicRoughness`(4 处：MASK 裁剪、metallic/roughness、normalScale) → 同前缀
   - `ext0`/`ext1`/`ext2`(specIor/sheen/cc 各块) → `iu.items[vItem].ext0` 等
   - `emissiveOcclusion`(ao/emissive 2 处) → 同前缀
   其余（FrameUBO/LightUBO/采样器/光照循环/分层逻辑）与 Step 2 完全相同。

- [ ] **Step 5: shadow_depth_instanced.vert — Item 补齐到 512B + items[32]**

```glsl
// shadow_depth_instanced.vert:阴影 pass 实例化;ItemUBO items[] 按 gl_InstanceIndex。
// 宿主 bind ItemUBO(offset=组首槽×512, size=组大小×512);同资源组共享光 VP。
// Item 布局与 pbr_forward_instanced 一致(ext0..2 + padding 到 512B 槽距)。
#version 450
layout(location = 0) in vec3 aPos;
layout(binding = 0) uniform ShadowUBO { mat4 lightViewProj; } u;
struct Item {
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
  vec4 _pad[13];
};
layout(binding = 1) uniform ItemUBO { Item items[32]; } iu;
void main() {
  gl_Position = u.lightViewProj * iu.items[gl_InstanceIndex].world * vec4(aPos, 1.0);
}
```

- [ ] **Step 6: 构建 + 全量 golden 回归（零回归硬约束的验证点）**

```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```

预期：全部通过——默认因子走均匀分支跳过全部新采样与瓣计算，输出与旧 shader
逐像素一致（阴影重构 shadowF 对基层语义等价；Fenv 与旧式内联相同）。
若失败：优先排查 ext 填充默认值（ior 必须 1.5 而非 0）与 specIor 分支条件。

- [ ] **Step 7: Commit**

```bash
git add shaders/
git commit -m "feat(shaders): pbr 分层 BRDF——clearcoat(GGX f0=0.04 独立法线)+sheen(Charlie/Neubelt/0.157 拟合)+specular/ior(f0 改造+漫反射能量扣);默认因子零操作;实例化 Item 512B/32 项"
```

---

### Task 6: 画质/选项门控（render.ext_materials × QualityPreset 与关系）

**Files:**
- Modify: `core/renderer/quality.h:21`
- Modify: `core/renderer/quality.cpp:5-12`
- Modify: `tools/render_test/scenes.cpp:182,264`（两处静态 kHigh）
- Modify: `core/renderer/renderer.cpp`（setQuality 写入 extMaterialsQuality_——Task 4 已加成员）
- Modify: `core/api/options.json`
- Modify: `core/api/rd_api.cpp:147`（applyOptions）
- Create: `tests/renderer/material_ext_test.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: QualityPreset 字段 + 预设表**

`quality.h` 追加字段（fxaaEnabled 之后）：

```cpp
  uint32_t extMaterials;      // KHR 扩展材质四件套(clearcoat/sheen/specular/ior)
```

`quality.cpp` 预设改为：

```cpp
QualityPreset qualityPreset(QualityTier t) {
  switch (t) {
    case QualityTier::High: return {1.0f, 4, 256, 6, 4096, 2048, 1, 0, 1};
    case QualityTier::Mid:  return {0.75f, 2, 128, 5, 2048, 1024, 1, 0, 1};
    case QualityTier::Low:  return {0.5f, 1, 64, 4, 1024, 0, 0, 1, 0};
  }
  return {1.0f, 1, 64, 5, 4096, 0, 0, 0, 0};
}
```

`tools/render_test/scenes.cpp` 两处 `static const QualityPreset kHigh = {1.0f, 4, 256, 6, 4096, 2048, 1, 0};`
改为 `{1.0f, 4, 256, 6, 4096, 2048, 1, 0, 1};`。

- [ ] **Step 2: Renderer setQuality 写入档位旗标**

`renderer.cpp` setQuality 内（`fxaaEnabled_ = q.fxaaEnabled != 0;` 之后）：

```cpp
  extMaterialsQuality_ = q.extMaterials != 0;
```

- [ ] **Step 3: options.json 新条目（命令总线自动获得 set/get/toggle）**

`core/api/options.json` 末尾条目后追加：

```json
  ,"render.ext_materials": {"type": "bool", "default": true, "doc": "KHR 材质扩展四件套(clearcoat/sheen/specular/ior)开关"}
```

（注意 JSON 逗号；构建时 GenOptions.cmake 自动重生成 options_generated。）

- [ ] **Step 4: applyOptions 接线**

`core/api/rd_api.cpp` applyOptions 内（`setFrustumCulling` 行附近）：

```cpp
  e->renderer.setExtMaterialsEnabled(o.render.ext_materials);
```

- [ ] **Step 5: 写门控语义测试（失败态：文件不存在无法编译即视为红）**

创建 `tests/renderer/material_ext_test.cpp`：

```cpp
// KHR 扩展材质门控语义:程序化 clearcoat 球 开/关 渲染应不同;
// DamagedHelmet(无扩展)开/关应逐像素一致(零操作语义)。
// (P4-A Task 7 再追加三 golden 模型用例。)
#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include "common/image.h"
#include "common/shader_code.h"
#include "renderer/renderer.h"
#include "renderer/quality.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "resource/primitives.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 256, kH = 256;

// 确定性画质档:阴影/后处理关,extMaterials=1
rd::QualityPreset extPreset() {
  rd::QualityPreset q{};
  q.renderScale = 1.0f;
  q.msaa = 1;
  q.iblPrefilterSize = 64;
  q.iblPrefilterMips = 5;
  q.maxTextureDim = 4096;
  q.shadowMapSize = 0;
  q.postEnabled = 0;
  q.fxaaEnabled = 0;
  q.extMaterials = 1;
  return q;
}

// 渲染一个 ModelAsset(extOn 控制门控)→ RGBA8;失败返回空(width==0)
rd::test::Image renderModel(rd::Backend b, const rd::ModelAsset& model, bool extOn) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto load = [&](const char* name) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, name); };
  auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
  auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
  auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
  auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
  auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
  auto exFs = load("bloom_extract.frag");
  auto bbFs = load("bloom_blur.frag");
  auto cpFs = load("composite.frag");
  auto fxFs = load("fxaa.frag");
  auto skVs = load("pbr_forward_skinned.vert");
  auto sdsVs = load("shadow_depth_skinned.vert");
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   skVs.code,   sdsVs.code,
                            {}, {}, {}, {}, {}, {}, {}, {}, unlitVs.entry,
                            rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  if (!target.valid() || !renderer.init(*device, sd)) return {};
  renderer.setQuality(extPreset());
  renderer.setExtMaterialsEnabled(extOn);
  auto res = rd::MeshRenderResource::upload(*device, model);
  if (!res) return {};
  rd::scene::Scene scene;
  auto node = std::make_unique<rd::scene::MeshNode>();
  node->mesh = res;
  scene.root().addChild(std::move(node));
  // 包围球取景:45° 方位角、20° 仰角
  rd::scene::Camera cam;
  const float dist = model.boundingRadius * 2.5f;
  glm::vec3 c(model.boundingCenter[0], model.boundingCenter[1], model.boundingCenter[2]);
  glm::vec3 eye = c + glm::vec3(dist * 0.65f, dist * 0.35f, dist * 0.65f);
  cam.lookAt({eye.x, eye.y, eye.z}, {c.x, c.y, c.z}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, dist * 0.1f, dist * 10.0f);
  device->beginFrame();
  renderer.beginScene(cam, {0.05f, 0.05f, 0.06f, 1.0f});
  scene.collect(renderer);
  auto* cmd = device->acquireCommandBuffer();
  renderer.endScene(cmd, target);
  device->submit(cmd);
  device->waitIdle();
  device->endFrame();
  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(size_t(kW) * kH * 4);
  device->readbackTarget(target, img.pixels.data(), img.pixels.size());
  renderer.shutdown();
  return img;
}

} // namespace

TEST(MaterialExtGate, ZeroOpOnPlainMaterial) {
  // DamagedHelmet 无扩展材质:ext 开/关逐像素一致(零操作硬约束)
  auto model = rd::loadGltf(RD_TEST_DATA_DIR "/assets/DamagedHelmet.glb");
  ASSERT_TRUE(model.valid());
  auto on = renderModel(rd::Backend::Metal, model, true);
  auto off = renderModel(rd::Backend::Metal, model, false);
  if (on.width == 0) GTEST_SKIP() << "Metal 不可用";
  ASSERT_EQ(on.pixels.size(), off.pixels.size());
  EXPECT_TRUE(rd::test::compareSSIM(on.pixels.data(), off.pixels.data(), kW, kH, 0.0).pass);
}

TEST(MaterialExtGate, ToggleChangesClearcoat) {
  // 程序化 clearcoat 球:开/关渲染应显著不同
  auto mesh = rd::primitives::makeSphere(0.5f, 48, 24);
  mesh.material.clearcoatFactor = 1.0f;
  mesh.material.clearcoatRoughnessFactor = 0.1f;
  mesh.material.roughnessFactor = 0.6f;
  mesh.material.metallicFactor = 0.1f;
  rd::ModelAsset model;
  model.meshes.push_back(std::move(mesh));
  model.boundingRadius = 0.5f;
  auto on = renderModel(rd::Backend::Metal, model, true);
  auto off = renderModel(rd::Backend::Metal, model, false);
  if (on.width == 0) GTEST_SKIP() << "Metal 不可用";
  ASSERT_EQ(on.pixels.size(), off.pixels.size());
  auto cmp = rd::test::compareSSIM(on.pixels.data(), off.pixels.data(), kW, kH, 0.05);
  EXPECT_GT(cmp.error, 0.01) << "clearcoat 开关未产生可见差异";
}
```

`tests/CMakeLists.txt` 源列表（shadow_test.cpp 行附近）追加：

```cmake
  renderer/material_ext_test.cpp
```

- [ ] **Step 6: 构建 + 运行门控测试 + 全量回归**

```bash
cmake --build build -j && ctest --test-dir build -R "MaterialExtGate" --output-on-failure
ctest --test-dir build --output-on-failure
```

预期：两个门控用例 PASS（ZeroOp 逐像素一致；Toggle 误差 > 0.01），全量不变。

- [ ] **Step 7: Commit**

```bash
git add core/renderer/quality.h core/renderer/quality.cpp core/renderer/renderer.cpp core/api/options.json core/api/rd_api.cpp tools/render_test/scenes.cpp tests/renderer/material_ext_test.cpp tests/CMakeLists.txt
git commit -m "feat(api): render.ext_materials 选项 + 画质档 extMaterials(High/Mid=1,Low=0,与关系门控)+ 门控语义测试(零操作/开关可见)"
```

---

### Task 7: 测试资产 + golden 三场景 + 演示场景 material_ext_gallery

**Files:**
- Modify: `scripts/fetch_assets.sh`
- Modify: `tools/render_test/scenes.cpp`（kNames + gallery 构建）
- Modify: `tests/renderer/material_ext_test.cpp`（追加 3 golden）

- [ ] **Step 1: fetch_assets.sh 增加 Khronos 官方模型**

`scripts/fetch_assets.sh` 末段（Sponza 块之前）追加：

```bash
# P4-A KHR 材质扩展 golden 模型(clearcoat/sheen/specular)
dl ClearCoatTest.glb ClearCoatTest/glTF-Binary/ClearCoatTest.glb
dl SheenClothPillow.glb SheenClothPillow/glTF-Binary/SheenClothPillow.glb
dl SpecularTest.glb SpecularTest/glTF-Binary/SpecularTest.glb
```

- [ ] **Step 2: 下载资产**

```bash
./scripts/fetch_assets.sh
```

预期：三模型 OK（弱网可重跑）。

- [ ] **Step 3: golden 三用例（追加到 material_ext_test.cpp）**

文件 include 区追加 `#include <cstdlib>` 与 `#include <filesystem>`；
`renderModel` 之后追加：

```cpp
// 资产定位:RD_ASSETS_DIR(默认 <repo>/assets);缺失 → 空图(golden skip)
std::string findAsset(const char* rel) {
  if (!getenv("RD_ASSETS_DIR"))
    setenv("RD_ASSETS_DIR", (std::string(RD_TEST_DATA_DIR) + "/../assets").c_str(), 1);
  std::string p = std::string(getenv("RD_ASSETS_DIR")) + "/" + rel;
  return std::filesystem::exists(p) ? p : std::string();
}

rd::test::Image renderAsset(rd::Backend b, const char* rel) {
  const std::string path = findAsset(rel);
  if (path.empty()) return {};
  auto model = rd::loadGltf(path.c_str());
  if (!model.valid()) return {};
  return renderModel(b, model, true);
}

rd::test::Image renderClearCoat(rd::Backend b) { return renderAsset(b, "ClearCoatTest.glb"); }
rd::test::Image renderSheenPillow(rd::Backend b) { return renderAsset(b, "SheenClothPillow.glb"); }
rd::test::Image renderSpecular(rd::Backend b) { return renderAsset(b, "SpecularTest.glb"); }

RD_GOLDEN_TEST(MaterialExt, ClearCoat, "ext_clearcoat", 0.05, renderClearCoat)
RD_GOLDEN_TEST(MaterialExt, SheenPillow, "ext_sheen_pillow", 0.05, renderSheenPillow)
RD_GOLDEN_TEST(MaterialExt, Specular, "ext_specular", 0.05, renderSpecular)
```

- [ ] **Step 4: 演示场景 material_ext_gallery（scenes.cpp）**

kNames 追加（fox_anim 之后）：

```cpp
const char* const kNames[] = {"material_balls", "cornell_box", "light_playground",
                              "skinned_demo",   "instanced_field",
                              "sponza",         "cesium_man",
                              "emissive_bloom", "normal_map_wall",
                              "shadow_gallery", "ktx2_gallery", "alpha_blend",
                              "fox_anim",       "material_ext_gallery"};
```

匿名命名空间内新增构建函数（buildFamousGlb 之后）：

```cpp
// KHR 扩展材质三模型并排(资产缺失返回 false → 调用方 skip)
bool buildMaterialExtGallery(Device& dev, DemoScene& out) {
  struct Entry {
    const char* rel;
    float x;
  };
  const Entry entries[] = {
      {"ClearCoatTest.glb", -1.6f}, {"SheenClothPillow.glb", 0.0f}, {"SpecularTest.glb", 1.6f}};
  std::string base = "assets/";
  if (!std::filesystem::exists(base)) {
    if (const char* alt = getenv("RD_ASSETS_DIR")) base = std::string(alt) + "/";
  }
  for (const auto& e : entries) {
    const std::string path = base + e.rel;
    if (!std::filesystem::exists(path)) {
      RD_LOGW("demo.scene", "资产缺失(scripts/fetch_assets.sh 下载): %s", path.c_str());
      return false;
    }
    auto model = loadGltf(path.c_str());
    if (!model.valid()) return false;
    auto res = MeshRenderResource::upload(dev, model);
    if (!res) return false;
    const float s = 0.8f / std::max(model.boundingRadius, 1e-4f);  // 归一到 0.8 半径
    out.resources.push_back(res);
    out.worlds.push_back(glm::translate(math::Mat4(1.0f), math::Vec3(e.x, 0.8f, 0)) *
                         glm::scale(math::Mat4(1.0f), math::Vec3(s)));
  }
  LightData dir;  // 默认方向光 + 适度亮度,高光可见
  dir.color[0] = dir.color[1] = dir.color[2] = 3.0f;
  out.lights.push_back(dir);
  out.camera.lookAt({0, 0.9f, 4.2f}, {0, 0.6f, 0}, {0, 1, 0});
  out.camera.setPerspective(0.78539816f, 1.0f, 0.1f, 50.0f);
  out.framingCenter[1] = 0.8f;
  out.framingRadius = 2.6f;
  return true;
}
```

buildDemoScene 分发（fox_anim 分支之后）：

```cpp
  } else if (n == "material_ext_gallery") {
    if (!buildMaterialExtGallery(dev, out)) return false;
  } else {
```

（若 scenes.cpp 未含 `<glm/gtc/matrix_transform.hpp>` 则确认 glm::scale 可用——
现有代码已用 glm::translate，同头文件提供 scale。）

- [ ] **Step 5: 生成 golden（先红后绿）**

```bash
cmake --build build -j
ctest --test-dir build -R "MaterialExt" --output-on-failure   # 预期:golden 缺失断言失败
RD_UPDATE_GOLDENS=1 ctest --test-dir build -R "MaterialExt.ClearCoat|MaterialExt.SheenPillow|MaterialExt.Specular" --output-on-failure
ls tests/golden/ext_*.png   # 应有 6 张
```

**目视核对**（必须）：`ext_clearcoat_metal.png`（车漆高光+边缘反光）、
`ext_sheen_pillow_metal.png`（织物边缘绒光）、`ext_specular_metal.png`（高光强度梯度），
Vulkan 三张与 Metal 大体一致。确认后才进入下一步。

- [ ] **Step 6: 全量回归 + SmokeAll 覆盖 gallery**

```bash
ctest --test-dir build --output-on-failure
./build/tools/render_test/render_test --scene material_ext_gallery --out /tmp/gallery.png
```

预期：全部通过；gallery 渲染三模型并排（SmokeAll 因 kNames 自动覆盖此场景）。

- [ ] **Step 7: Commit**

```bash
git add scripts/fetch_assets.sh tools/render_test/scenes.cpp tests/renderer/material_ext_test.cpp tests/golden/
git commit -m "feat(test): KHR 扩展三模型 golden(ClearCoat/SheenPillow/SpecularTest 双后端)+ material_ext_gallery 演示场景 + fetch_assets 收录"
```

---

### Task 8: 文档（AGENTS.md）

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: 更新 AGENTS.md**

"当前状态"追加一条：

```markdown
- P4-A 完成：KHR 材质扩展四件套(clearcoat GGX f0=0.04 独立法线/sheen Charlie+Neubelt
  0.157 拟合/specular+ior f0 改造+漫反射能量扣;默认因子零操作,golden 零回归)
  + 纹理槽 9→16 + ItemUBO 304B 块/512B 槽距 + 实例化组上限 32
  + render.ext_materials 选项与画质档与关系;
  顺带修复:Vulkan slot8 聚光阴影描述符丢弃 + GLES 语义 sampler 名全落单元 0
```

"代码约定"区修改/追加：

- 纹理槽位条目改为 16 槽全表（0..15，新增 9..15 语义；注明"恰压 GLES 3.0 保证的
  16 纹理单元线，真机普遍 32+"）。
- UBO 约定条目：slot1=ItemUBO 更新为"304B 块、512B 槽距、容量 128 槽(64KB)"。
- 自动实例化条目：分组上限 64→32、Item 结构体 512B（ext0..2 + _pad[13]）。
- 新增条目"KHR 扩展材质"：四扩展默认值零操作语义表、分层 BRDF 结构（specIor/sheen/
  clearcoat 均匀分支；0.157 拟合与 LUT 版的微小差异说明）、门控（选项×画质档与关系，
  关闭时 ItemUBO 写默认因子）。
- GLES 采样器命名条目：链接期语义名表（kSamplerTable）+ bindTexture tex%u 双机制说明。

"构建与测试"追加：

```markdown
- KHR 扩展 golden:`ext_clearcoat/ext_sheen_pillow/ext_specular`(fetch_assets 下载
  ClearCoatTest/SheenClothPillow/SpecularTest;缺失自动 skip);
  交互 `--interactive --scene material_ext_gallery`;
  门控命令:`set render.ext_materials false`
```

"下一步"行更新为：`P4-B(transmission/volume)或 P4-C(morph/Draco),或 P3(AR+鸿蒙)`。

- [ ] **Step 2: 最终全量验证**

```bash
./scripts/check.sh
```

预期：配置 + 构建 + 全部测试通过。

- [ ] **Step 3: Commit**

```bash
git add AGENTS.md
git commit -m "docs: P4-A KHR 材质扩展四件套记入 AGENTS.md(纹理槽 16 表/ItemUBO 512 槽距/分层 BRDF/门控/两存量修复)"
```

---

## Self-Review 结论

1. **Spec 覆盖**：§2 loader→Task 2；§3 GPU 资源→Task 3；§4 ItemUBO→Task 4/5；
   §5 纹理槽→Task 1；§6 shader→Task 5；§7 门控→Task 6；§8 测试→Task 2/6/7；
   §9 文档→Task 8。零回归门槛=各任务的 ctest 全量步骤。✔
2. **占位符扫描**：所有代码步骤含完整代码/精确数字；无 TBD/"适当处理"。✔
3. **类型一致性**：`kItemUboStride/kItemUboSize/kItemUboMaxSlots`（renderable.h）与
   renderer.h `kUboStride/kMaxItemSlots`、mesh_renderable.cpp `kLastSlot`、
   `kMaxInstGroup=32` 与 shader `items[32]`、`ext0/1/2` 打包在 C++ 与 GLSL 两侧
   字段语义一一对应。✔
