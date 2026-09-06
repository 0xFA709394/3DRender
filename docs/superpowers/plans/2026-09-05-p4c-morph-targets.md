# P4-C Morph Targets 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 落地 glTF morph targets（GPU 纹理形变：RGBA16F 增量纹理 + texelFetch 顶点累加 + ItemUBO 权重 + 动画/手动驱动），零权重零操作、现有 golden 全量不动。

**Architecture:** 增量打包为双行/目标（POSITION/NORMAL）的 RGBA16F 纹理，4 个新 vert 变体（pbr/morph、pbr/morph+skin、shadow/morph、shadow/morph+skin）在顶点阶段按 ItemUBO ext5/ext6 权重累加；权重经 Animator（glTF weights 通道）或静态初始值填充，C API 手动覆盖；slot19=binding24（复用 smpMat，一次性 binding 例外）。

**Tech Stack:** C++17 / GLSL 450(经 glslang→SPIR-V→spirv-cross 三后端) / cgltf / glm / ctest。

**Spec:** `docs/superpowers/specs/2026-09-05-p4c-morph-targets-design.md`（已确认）

---

## 0. 关键布局决定（全程有效）

- **增量纹理**：RGBA16F，宽=max(顶点数,1)，高=目标数×2（偶行 POSITION、奇行
  NORMAL 增量；无 NORMAL 增量的目标填 0 行）；texel=(dx,dy,dz,pad)。
- **slot19 = texMorph**：Vulkan pbr 布局 binding **24**（slot→binding 约定的一次性
  例外，23 已被 smpMat 占用）；**复用 smpMat**（texelFetch 不滤波；Metal `read()`
  不消费 sampler；RGBA16F 可滤波 → GLES 完备）。caps 19→**20**。
- **ItemUBO 336→368B**（槽距 512 不变）：`ext5`=weights[0..3]、`ext6`=weights[4..7]、
  目标数=`ext4.w`（原 0 空位）。8 目标上限（glTF 一致性最低线，超出截断+告警）。
- **应用顺序**：先 morph 后 skin（glTF 语义：形变作用于绑定姿态再蒙皮）。
- **morph 项排除**：自动实例化分组、视锥剔除（同蒙皮惯例）；unlit+morph 不支持
  （按 unlit 渲染 + 一次性日志）。
- **门控**：无画质档/选项（每顶点 ≤8 次 fetch 成本轻）。
- **零回归硬约束**：Task 4（UBO 扩容）/Task 5（shader 槽位）后的现有 golden 全量
  必须通过；新 golden 只在 Task 8。

---

### Task 1: Loader 静态 morph 解析（targets/初始权重/名字/截断/烘焙）

**Files:**
- Modify: `core/resource/gltf_loader.h`（MeshData）
- Modify: `core/resource/gltf_loader.cpp`（primitive 解析 + 尾部烘焙）
- Test: `tests/resource/gltf_test.cpp`（追加）

- [ ] **Step 1: 写失败单测**

`tests/resource/gltf_test.cpp` 末尾追加（复用现有临时目录+JSON 拼装样板）：

```cpp
// morph targets:双目标增量 + 初始权重 + targetNames + 超限截断 + sparse 展开烘焙
TEST(Gltf, MorphTargets) {
  // 顶点 3 个三角形,2 个 morph target:
  //   t0 = POSITION 增量 (0.1,0,0);t1 = POSITION+NORMAL 增量 (0,0.2,0)/(1,0,0)
  // 初始 weights=(0.5, 1.0);targetNames=("swell","lift")
  const char* gltf = R"({
    "asset": {"version": "2.0"},
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"weights": [0.5, 1.0],
      "extras": {"targetNames": ["swell", "lift"]},
      "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1},
                      "indices": 2, "targets": [
        {"POSITION": 3},
        {"POSITION": 4, "NORMAL": 5}]}]}],
    "buffers": [{"uri": "tri.bin", "byteLength": 200}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0,   "byteLength": 36},
      {"buffer": 0, "byteOffset": 36,  "byteLength": 36},
      {"buffer": 0, "byteOffset": 72,  "byteLength": 6},
      {"buffer": 0, "byteOffset": 80,  "byteLength": 36},
      {"buffer": 0, "byteOffset": 120, "byteLength": 36},
      {"buffer": 0, "byteOffset": 160, "byteLength": 36}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"},
      {"bufferView": 3, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 5, "componentType": 5126, "count": 3, "type": "VEC3"}]
  })";
  const std::string dir = (std::filesystem::temp_directory_path() / "rd_gltf_morph").string();
  std::filesystem::create_directories(dir);
  { FILE* f = fopen((dir + "/tri.gltf").c_str(), "w"); fputs(gltf, f); fclose(f); }
  {
    FILE* f = fopen((dir + "/tri.bin").c_str(), "wb");
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const float nrm[9] = {0,0,1, 0,0,1, 0,0,1};
    const uint16_t idx[3] = {0, 1, 2};
    const float d0[9] = {0.1f,0,0, 0.1f,0,0, 0.1f,0,0};      // t0 pos
    const float d1[9] = {0,0.2f,0, 0,0.2f,0, 0,0.2f,0};      // t1 pos
    const float dn[9] = {1,0,0, 1,0,0, 1,0,0};               // t1 normal
    fwrite(pos, 4, 9, f); fwrite(nrm, 4, 9, f); fwrite(idx, 2, 3, f);
    fwrite(d0, 4, 9, f); fwrite(d1, 4, 9, f); fwrite(dn, 4, 9, f);
    fclose(f);
  }
  auto model = rd::loadGltf((dir + "/tri.gltf").c_str());
  ASSERT_TRUE(model.valid());
  const auto& m = model.meshes[0];
  EXPECT_TRUE(m.morph);
  ASSERT_EQ(m.morphPosDeltas.size(), size_t(2 * 9));
  EXPECT_FLOAT_EQ(m.morphPosDeltas[0], 0.1f);          // t0 行首
  EXPECT_FLOAT_EQ(m.morphPosDeltas[9], 0.0f);          // t1 行首 = (0,0.2,0)
  EXPECT_FLOAT_EQ(m.morphPosDeltas[10], 0.2f);
  ASSERT_EQ(m.morphNormalDeltas.size(), size_t(2 * 9));
  EXPECT_EQ(m.morphNormalDeltas[3], 0.0f);             // t0 normal 全 0(未声明)
  EXPECT_FLOAT_EQ(m.morphNormalDeltas[9 + 3], 1.0f);   // t1 normal x
  ASSERT_EQ(m.morphWeights.size(), 2u);
  EXPECT_FLOAT_EQ(m.morphWeights[0], 0.5f);
  EXPECT_FLOAT_EQ(m.morphWeights[1], 1.0f);
  ASSERT_EQ(m.morphTargetNames.size(), 2u);
  EXPECT_EQ(m.morphTargetNames[0], "swell");
}

// 超限截断:10 目标 → 8 + morphWeights 同步截断
TEST(Gltf, MorphTargetTruncation) {
  std::string tgts;
  for (int i = 0; i < 10; ++i) tgts += "{\"POSITION\": 0},";
  tgts.pop_back();
  std::string wts;
  for (int i = 0; i < 10; ++i) wts += "0.1,";
  wts.pop_back();
  const std::string gltf = std::string(R"({
    "asset": {"version": "2.0"},
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"weights": [)" + wts + R"(],
      "primitives": [{"attributes": {"POSITION": 0}, "indices": 1,
                      "targets": [)" + tgts + R"(]}]}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })");
  const std::string dir = (std::filesystem::temp_directory_path() / "rd_gltf_morph_t").string();
  std::filesystem::create_directories(dir);
  { FILE* f = fopen((dir + "/tri.gltf").c_str(), "w"); fputs(gltf.c_str(), f); fclose(f); }
  { FILE* f = fopen((dir + "/tri.bin").c_str(), "wb");
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const uint16_t idx[3] = {0, 1, 2};
    fwrite(pos, 4, 9, f); fwrite(idx, 2, 3, f); fclose(f); }
  auto model = rd::loadGltf((dir + "/tri.gltf").c_str());
  ASSERT_TRUE(model.valid());
  const auto& m = model.meshes[0];
  EXPECT_TRUE(m.morph);
  EXPECT_EQ(m.morphPosDeltas.size() / 9, 8u);    // 截断到 8 目标
  EXPECT_EQ(m.morphNormalDeltas.size() / 9, 8u);
  EXPECT_EQ(m.morphWeights.size(), 8u);
}
```

- [ ] **Step 2: 跑测试确认编译失败**

```bash
cmake --build build --target rd_tests 2>&1 | grep error | head -5
```
预期：`no member named 'morphPosDeltas'` 等编译错误。

- [ ] **Step 3: MeshData 字段**

`core/resource/gltf_loader.h` MeshData（`bool skinned` 行前）追加：

```cpp
  // ---- morph targets(P4-C;增量=target 主序 [dx,dy,dz]×vertexCount)----
  bool morph = false;                    // 有 ≥1 有效目标(管线选型用)
  std::vector<float> morphPosDeltas;     // 每目标连续(NORMAL 缺失的目标零行在纹理层补)
  std::vector<float> morphNormalDeltas;
  std::vector<float> morphWeights;       // 初始权重(glTF mesh.weights;截断同步)
  std::vector<std::string> morphTargetNames;  // extras.targetNames(元数据)
```

- [ ] **Step 4: primitive 解析处读 targets**

`core/resource/gltf_loader.cpp` mesh 构建函数（搜 `prim.targets` 无果则搜
`cgltf_primitive`/`pbr` 属性读取循环；在材质/顶点组装完成、`indices` 就绪之后）
追加——**在法线 flat 生成之后**（保证顶点数最终值）：

```cpp
  // ---- morph targets(P4-C):POSITION/NORMAL 增量,TANGENT 忽略 ----
  constexpr uint32_t kMaxMorphTargets = 8;  // glTF 一致性最低要求线
  const uint32_t vertCount = uint32_t(m.vertices.size() / (m.skinned ? 20u : 12u));
  if (prim.targets_count > 0 && vertCount > 0) {
    const uint32_t used =
        std::min(uint32_t(prim.targets_count), kMaxMorphTargets);
    if (prim.targets_count > kMaxMorphTargets)
      RD_LOGW("resource.gltf", "morph targets %u 超上限,截断到 %u",
              uint32_t(prim.targets_count), kMaxMorphTargets);
    bool anyValid = false;
    for (uint32_t t = 0; t < used; ++t) {
      const cgltf_attribute *pos = nullptr, *nrm = nullptr;
      bool hasTan = false;
      for (cgltf_size a = 0; a < prim.targets[t].attributes_count; ++a) {
        const auto& at = prim.targets[t].attributes[a];
        if (at.type == cgltf_attribute_type_position) pos = &at;
        else if (at.type == cgltf_attribute_type_normal) nrm = &at;
        else if (at.type == cgltf_attribute_type_tangent) hasTan = true;
      }
      if (hasTan)
        RD_LOGW("resource.gltf", "morph TANGENT 增量忽略(v1 限制)");
      if (!pos) continue;  // 无 POSITION 增量的目标跳过(glTF 语义必须含 POSITION)
      bool ok = true;
      for (uint32_t v = 0; v < vertCount && ok; ++v) {
        float d[3] = {0, 0, 0};
        if (cgltf_accessor_read_float(pos->data, v, d, 3) != cgltf_true) ok = false;
        if (ok && nrm && cgltf_accessor_read_float(nrm->data, v, d, 3) != cgltf_true)
          ok = false;  // 复用 d 读 normal,下方分流
        // 注:normal 不能复用 pos 的 d——见下修正:双缓冲
      }
      (void)ok;
      // 实现按下式(双流独立读):
      m.morphPosDeltas.resize(size_t(used) * vertCount * 3, 0.0f);
      m.morphNormalDeltas.resize(size_t(used) * vertCount * 3, 0.0f);
      float* pp = &m.morphPosDeltas[size_t(t) * vertCount * 3];
      float* nn = &m.morphNormalDeltas[size_t(t) * vertCount * 3];
      for (uint32_t v = 0; v < vertCount; ++v) {
        cgltf_float d[3] = {0, 0, 0};
        if (cgltf_accessor_read_float(pos->data, v, d, 3) == cgltf_true)
          for (int c = 0; c < 3; ++c) pp[v * 3 + c] = float(d[c]);
        if (nrm) {
          cgltf_float dn[3] = {0, 0, 0};
          if (cgltf_accessor_read_float(nrm->data, v, dn, 3) == cgltf_true)
            for (int c = 0; c < 3; ++c) nn[v * 3 + c] = float(dn[c]);
        }
      }
      anyValid = true;
    }
    // resize 放循环外(目标数一次定型):实现时把两行 resize 移到 `for t` 之前
    if (anyValid) {
      m.morph = true;
      const cgltf_mesh* cm = primContainingMesh;  // 见下:从调用上下文取 cgltf_mesh*
      if (cm->weights_count > 0)
        for (uint32_t t = 0; t < used && t < cm->weights_count; ++t)
          m.morphWeights.push_back(float(cm->weights[t]));
      else
        m.morphWeights.assign(used, 0.0f);
      for (uint32_t t = 0; t < used && t < cm->target_names_count; ++t)
        m.morphTargetNames.push_back(cm->target_names[t] ? cm->target_names[t] : "");
    } else {
      m.morphPosDeltas.clear();
      m.morphNormalDeltas.clear();
    }
  }
```

实现注意（执行者按此落地,上面是语义骨架）：
- `resize` 两行移到目标循环**之前**（`used` 已知）。
- `primContainingMesh`：mesh 循环里已有 `const cgltf_mesh*`（构造 MeshData 的来源），
  直接取其 `weights/weights_count/target_names/target_names_count`。
- `cgltf_accessor_read_float` 的 sparse 展开由 cgltf 内建（已 cgltf_load_buffers）。
- 顶点数不足的 accessor（read 返回 false）→ 该目标保留零增量（不中断）。

- [ ] **Step 5: 烘焙矩阵作用于增量**

同文件尾部「glTF 节点世界变换烘焙」块（搜 `cgltf_node_transform_world`）：
非蒙皮 mesh 的 pos/normal/tangent 烘焙后，若 `m.morph`，对两路增量乘 `glm::mat3(bake)`
（bake=该 mesh 的世界矩阵）：

```cpp
      if (m.morph) {
        const glm::mat3 n3(bake);
        for (float& d : m.morphPosDeltas) { /* 逐 3 分组 */ }
        // 实现:for (size_t i = 0; i < morphPosDeltas.size(); i += 3) {
        //   glm::vec3 d(morphPosDeltas[i], ...); d = n3 * d; 写回 }
        // morphNormalDeltas 同法
      }
```

- [ ] **Step 6: 跑测试通过 + Commit**

```bash
cmake --build build --target rd_tests && ctest --test-dir build -R "Gltf.Morph" --output-on-failure
git add -A && git commit -m "feat(resource): glTF morph targets 解析(targets/初始权重/targetNames/8 上限截断/sparse 展开/烘焙)"
```

---

### Task 2: Loader 动画 weights 通道解析

**Files:**
- Modify: `core/resource/gltf_loader.h`（AnimChannelData 注释）
- Modify: `core/resource/gltf_loader.cpp`（动画通道循环,约 430..455 行）
- Test: `tests/resource/gltf_test.cpp`（追加）

- [ ] **Step 1: 写失败单测**

```cpp
// 动画 weights 通道:path=3,values=keys×targets 扁平,超限截断到 8
TEST(Gltf, MorphWeightAnimation) {
  const char* gltf = R"({
    "asset": {"version": "2.0"},
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1,
                    "targets": [{"POSITION": 0}, {"POSITION": 0}]}]}],
    "animations": [{"channels": [{
      "sampler": 0,
      "target": {"node": 0, "path": "weights"}}],
      "samplers": [{"input": 3, "output": 4, "interpolation": "LINEAR"}]}],
    "buffers": [{"uri": "tri.bin", "byteLength": 60}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6},
      {"buffer": 0, "byteOffset": 42, "byteLength": 8},
      {"buffer": 0, "byteOffset": 50, "byteLength": 8}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"},
      {"bufferView": 2, "componentType": 5126, "count": 2, "type": "SCALAR"},
      {"bufferView": 3, "componentType": 5126, "count": 4, "type": "SCALAR"}]
  })";
  const std::string dir = (std::filesystem::temp_directory_path() / "rd_gltf_morph_a").string();
  std::filesystem::create_directories(dir);
  { FILE* f = fopen((dir + "/tri.gltf").c_str(), "w"); fputs(gltf, f); fclose(f); }
  { FILE* f = fopen((dir + "/tri.bin").c_str(), "wb");
    const float pos[9] = {0,0,0, 1,0,0, 0,1,0};
    const uint16_t idx[3] = {0, 1, 2};
    const float times[2] = {0.0f, 1.0f};
    const float w[4] = {0.0f, 1.0f, 1.0f, 0.0f};  // 2 key × 2 target
    fwrite(pos, 4, 9, f); fwrite(idx, 2, 3, f);
    fwrite(times, 4, 2, f); fwrite(w, 4, 4, f); fclose(f); }
  auto model = rd::loadGltf((dir + "/tri.gltf").c_str());
  ASSERT_TRUE(model.valid());
  ASSERT_EQ(model.animations.size(), 1u);
  ASSERT_EQ(model.animations[0].channels.size(), 1u);
  const auto& ch = model.animations[0].channels[0];
  EXPECT_EQ(ch.path, 3);
  ASSERT_EQ(ch.times.size(), 2u);
  ASSERT_EQ(ch.values.size(), 4u);           // 2 key × 2 target
  EXPECT_FLOAT_EQ(ch.values[0], 0.0f);
  EXPECT_FLOAT_EQ(ch.values[3], 0.0f);
  EXPECT_FLOAT_EQ(model.animations[0].duration, 1.0f);
}
```

- [ ] **Step 2: 跑测试确认失败**

```bash
cmake --build build --target rd_tests && ctest --test-dir build -R "Gltf.MorphWeight" --output-on-failure
```
预期 FAIL（weights 通道被跳过 → channels 空）。

- [ ] **Step 3: 通道解析扩展**

`core/resource/gltf_loader.cpp` 动画通道循环（`if (chd.path < 0) continue;` 处）改：

```cpp
      chd.path = ch.target_path == cgltf_animation_path_type_translation ? 0
                 : ch.target_path == cgltf_animation_path_type_rotation ? 1
                 : ch.target_path == cgltf_animation_path_type_scale    ? 2
                 : ch.target_path == cgltf_animation_path_type_weights  ? 3
                                                                        : -1;
      if (chd.path < 0) continue;
      const cgltf_accessor* in = ch.sampler->input;
      const cgltf_accessor* out = ch.sampler->output;
      chd.times.resize(in->count);
      for (cgltf_size k = 0; k < in->count; ++k)
        cgltf_accessor_read_float(in, k, &chd.times[k], 1);
      uint32_t comps;
      if (chd.path == 3) {
        // weights:输出 SCALAR 扁平 keys×targets;取目标 mesh 的目标数(截断同 Task 1)
        uint32_t targets = 0;
        if (ch.target_node->mesh && ch.target_node->mesh->primitives_count > 0)
          targets = std::min(uint32_t(ch.target_node->mesh->primitives[0].targets_count),
                             8u);
        if (targets == 0) continue;  // 无 morph 目标:通道无意义
        comps = targets;
      } else {
        comps = chd.path == 1 ? 4 : 3;
      }
      chd.values.resize(out->count * comps);
      for (cgltf_size k = 0; k < out->count; ++k)
        for (uint32_t c = 0; c < comps; ++c)
          cgltf_accessor_read_float(out, k * comps + c, &chd.values[k * comps + c], 1);
```

`core/resource/gltf_loader.h` AnimChannelData 注释同步：
`// 0=translation,1=rotation,2=scale,3=morph weights(values=keys×targets 扁平)`。

注意：weights 输出 accessor 元素是逐 scalar（`k*comps+c` 索引）；read 返回
false 时保持 0（resize 已零初始化）。

- [ ] **Step 4: 测试通过 + Commit**

```bash
cmake --build build --target rd_tests && ctest --test-dir build -R "Gltf.Morph" --output-on-failure
git add -A && git commit -m "feat(resource): 动画 morph weights 通道解析(path=3,keys×targets 扁平)"
```

---

### Task 3: 资源层 morph 纹理上传（RGBA16F，暂不绑定）

**Files:**
- Modify: `core/resource/mesh_render_resource.h`（MeshGpuData）
- Modify: `core/resource/mesh_render_resource.cpp`（upload/destroy/校验）

- [ ] **Step 1: MeshGpuData 字段**

`core/resource/mesh_render_resource.h` MeshGpuData（`bool skinned` 前）追加：

```cpp
  // ---- morph targets(P4-C)----
  TextureHandle morphTex;   // RGBA16F:宽=顶点数,高=目标数×2(偶=POSITION 奇=NORMAL)
  bool morph = false;       // 管线选型(= material 侧独立标志,源自 MeshData.morph)
  std::vector<float> morphWeights;  // 当前权重宿主副本(初始值;Animator/手动覆盖)
```

- [ ] **Step 2: 上传实现**

`core/resource/mesh_render_resource.cpp`：文件头部（namespace rd 内）加 half 转换：

```cpp
namespace {
/// float → half(IEEE 754 位技巧;RGBA16F 打包用)
uint16_t toHalf(float f) {
  uint32_t x;
  memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000u;
  int32_t exp = int32_t((x >> 23) & 0xffu) - 127 + 15;
  uint32_t mant = (x >> 13) & 0x3ffu;
  if (exp <= 0) return uint16_t(sign);            // 下溢 → 0(保号)
  if (exp >= 31) return uint16_t(sign | 0x7c00u); // 上溢 → inf
  return uint16_t(sign | (uint32_t(exp) << 10) | mant);
}
} // namespace
```

upload（`g.transmissionTex/thicknessTex` 上传行之后）追加：

```cpp
    if (m.morph && !m.morphPosDeltas.empty()) {
      const uint32_t vcount = uint32_t(m.morphPosDeltas.size() /
                                       std::max<size_t>(m.morphNormalDeltas.size(), 1));
      // 行数 = 目标数×2;宽 = 顶点数(由 pos 流长度/3/目标数 反推,直接用原始计数更稳)
      const uint32_t targets = uint32_t(m.morphWeights.size());
      const uint32_t verts = targets > 0
                                 ? uint32_t(m.morphPosDeltas.size() / (targets * 3))
                                 : 0u;
      if (verts > 0 && targets > 0) {
        std::vector<uint16_t> px(size_t(verts) * targets * 2 * 4);
        for (uint32_t t = 0; t < targets; ++t)
          for (uint32_t v = 0; v < verts; ++v) {
            const size_t posBase = (size_t(verts) * t + v) * 4;
            const size_t nrmBase = (size_t(verts) * targets + size_t(verts) * t + v) * 4;
            for (int c = 0; c < 3; ++c) {
              px[posBase + c] = toHalf(m.morphPosDeltas[(size_t(t) * verts + v) * 3 + c]);
              px[nrmBase + c] =
                  toHalf(m.morphNormalDeltas[(size_t(t) * verts + v) * 3 + c]);
            }
            px[posBase + 3] = 0;
            px[nrmBase + 3] = 0;
          }
        TextureDesc mtd;
        mtd.width = verts;
        mtd.height = targets * 2;
        mtd.format = Format::R16G16B16A16_FLOAT;
        mtd.usage = TextureUsage::Sampled;
        mtd.data = px.data();
        mtd.dataSize = px.size() * 2;
        g.morphTex = dev.createTexture(mtd);
        g.morph = g.morphTex.valid();
        if (!g.morph)
          RD_LOGW("resource.mesh", "morph 纹理创建失败,按无 morph 渲染");
      }
      g.morphWeights = m.morphWeights;
    }
```

（纹理行序：行 0..targets-1=POSITION、行 targets..2t-1=NORMAL——与 shader
`ivec2(vi, i*2)/ivec2(vi, i*2+1)` **不同**！统一改为**交错行**：行 `t*2`=POS、
行 `t*2+1`=NORM。实现用交错：`posBase` 行号 `t*2`、`nrmBase` 行号 `t*2+1`：

```cpp
            const size_t posBase = (size_t(verts) * (t * 2 + 0) + v) * 4;
            const size_t nrmBase = (size_t(verts) * (t * 2 + 1) + v) * 4;
```

——执行者以交错版为准,spec §5 的 `i*2 / i*2+1` 与此一致。）

- [ ] **Step 3: 校验与销毁**

- 有效性检查追加 `!(g.morphTex.valid() && m.morph)` 之类：morph 失败已在上面降级
  `g.morph=false`，主校验追加 `if (m.morph && g.morph && !g.morphTex.valid())` 兜底。
- destroy 纹理清单追加 `g.morphTex`。

- [ ] **Step 4: 构建 + 全量回归（无消费者,零变化）+ Commit**

```bash
./scripts/check.sh 2>&1 | tail -1
git add -A && git commit -m "feat(resource): morph 增量 RGBA16F 纹理上传(双行/目标交错)+MeshGpuData 权重副本"
```

---

### Task 4: ItemUBO 368B（ext5/ext6 + ext4.w 计数;零回归）

**Files:**
- Modify: `core/renderer/renderable.h:17`
- Modify: `core/renderer/renderer.cpp`（ItemUBOData）
- Modify: `shaders/pbr_forward.frag` / `shaders/pbr_forward_instanced.frag`（块声明）
- Modify: `shaders/pbr_forward_skinned.vert`（块声明对齐）

- [ ] **Step 1: 常量与宿主结构**

`renderable.h`：`kItemUboSize = 336` → `368`（注释 `块 368B;ext5/ext6=morph 权重`）。
`renderer.cpp` ItemUBOData 追加 + static_assert 文案改：

```cpp
  float ext3[4];            // x=transmissionFactor y=thicknessFactor z=attenuationDistance(0=∞) w=morphTargetCount
  float ext4[4];            // xyz=attenuationColor w=morphTargetCount(与 ext3.w 同值,vert 侧就近读)
  float ext5[4];            // morph weights[0..3]
  float ext6[4];            // morph weights[4..7]
};
static_assert(sizeof(ItemUBOData) == kItemUboSize, "ItemUBO 必须 368B(槽距 512)");
```

（ext3.w 与 ext4.w 双写目标数：frag/vert 各取近处,避免跨阶段读歧义。）

- [ ] **Step 2: shader 块声明**

`shaders/pbr_forward.frag`（ext4 声明后）：

```glsl
  vec4 ext5;  // morph weights[0..3](frag 不读,块布局对齐)
  vec4 ext6;  // morph weights[4..7](同上)
```

`shaders/pbr_forward_instanced.frag` Item 结构（ext4 后）：

```glsl
  vec4 ext5;  // morph weights[0..3](instanced 不消费;块对齐)
  vec4 ext6;  // morph weights[4..7]
  vec4 _pad[9];  // std140 数组元素 stride 对齐 CPU 槽距 512B(368+144)
```
（原 `_pad[11]` 删除。）

`shaders/pbr_forward_skinned.vert` ItemUBO 块（ext2 后）追加 ext3..ext6 四行
（注释:vert 不读,跨阶段布局对齐）。

- [ ] **Step 3: 全量零回归 + Commit**

```bash
./scripts/check.sh 2>&1 | tail -1
git add -A && git commit -m "feat(renderer): ItemUBO 368B(ext5/ext6=morph 权重,ext3/ext4.w=目标数;零回归)"
```

---

### Task 5: 4 个 morph vert + 槽 19/binding 24 + caps 20（零回归）

**Files:**
- Create: `shaders/pbr_forward_morph.vert` / `shaders/pbr_forward_morph_skinned.vert`
  / `shaders/shadow_depth_morph.vert` / `shaders/shadow_depth_morph_skinned.vert`
- Modify: `shaders/CMakeLists.txt`（注册 4 个）
- Modify: `core/rhi/rhi_constants.inc.h` + 三后端 caps（19→20）
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`（pbr 布局 binding 24 + 池 + bindTexture 上限 + descriptorSetFor 特判）
- Modify: `core/rhi/backends/gles/gles_device.cpp`（kSamplerTable 合并名）

- [ ] **Step 1: pbr_forward_morph.vert**

```glsl
// pbr_forward_morph.vert:morph 版 PBR 顶点着色器(增量纹理 texelFetch 累加)。
// texMorph=slot19(binding24,RGBA16F):行 t*2=POSITION 增量,t*2+1=NORMAL 增量;
// ItemUBO ext5/ext6=weights[8],ext4.w=目标数;先 morph 后(可选)世界变换。
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
  vec4 ext3;  // w=morphTargetCount
  vec4 ext4;  // w=morphTargetCount(同 ext3.w)
  vec4 ext5;  // morph weights[0..3]
  vec4 ext6;  // morph weights[4..7]
};
layout(binding = 24) uniform texture2D texMorph;   // slot19(binding 例外)
layout(binding = 23) uniform sampler smpMat;       // 共享(线性;texelFetch 不滤波)

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUV;

void main() {
  vec3 pos = aPos;
  vec3 nrm = aNormal;
  const int mc = int(ext4.w + 0.5);
  if (mc > 0) {
    float wts[8] = float[8](ext5.x, ext5.y, ext5.z, ext5.w,
                            ext6.x, ext6.y, ext6.z, ext6.w);
    vec3 dP = vec3(0.0);
    vec3 dN = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
      if (i >= mc) break;
      dP += texelFetch(sampler2D(texMorph, smpMat),
                       ivec2(int(gl_VertexIndex), i * 2), 0).xyz * wts[i];
      dN += texelFetch(sampler2D(texMorph, smpMat),
                       ivec2(int(gl_VertexIndex), i * 2 + 1), 0).xyz * wts[i];
    }
    pos += dP;
    nrm += dN;
  }
  vWorldPos = (world * vec4(pos, 1.0)).xyz;
  vNormal = (normalMatrix * vec4(nrm, 0.0)).xyz;
  vTangent = vec4((normalMatrix * vec4(aTangent.xyz, 0.0)).xyz, aTangent.w);
  vUV = aUV * uvTransform.zw + uvTransform.xy;
  gl_Position = mvp * vec4(pos, 1.0);
}
```

- [ ] **Step 2: pbr_forward_morph_skinned.vert**

同上,差异：locations 追加 `4=aJoints/5=aWeights`(stride 80 布局注释)、
`layout(binding = 3) uniform JointUBO { mat4 joints[128]; } jubo;`、main 中
morph 块之后:

```glsl
  mat4 skin = aWeights.x * jubo.joints[int(aJoints.x + 0.5)] +
              aWeights.y * jubo.joints[int(aJoints.y + 0.5)] +
              aWeights.z * jubo.joints[int(aJoints.z + 0.5)] +
              aWeights.w * jubo.joints[int(aJoints.w + 0.5)];
  vec4 p = skin * vec4(pos, 1.0);            // 先 morph 后 skin(glTF 语义)
  vWorldPos = (world * p).xyz;
  vNormal = (normalMatrix * vec4(mat3(skin) * nrm, 0.0)).xyz;
  vTangent = vec4((normalMatrix * vec4(mat3(skin) * aTangent.xyz, 0.0)).xyz, aTangent.w);
  vUV = aUV * uvTransform.zw + uvTransform.xy;
  gl_Position = mvp * p;
```

- [ ] **Step 3: shadow_depth_morph.vert / shadow_depth_morph_skinned.vert**

shadow_depth_morph.vert = shadow_depth.vert 结构 + morph 块(声明 texMorph/smpMat/
完整 ItemUBO 含 ext5/ext6;只写 gl_Position,normal 不需要但 texelFetch 循环只算 dP):

```glsl
// shadow_depth_morph.vert:morph 版深度写出(texMorph 行 t*2=POSITION 增量)。
#version 450
layout(location = 0) in vec3 aPos;
layout(binding = 0) uniform ShadowUBO { mat4 lightViewProj; } u;
layout(binding = 1) uniform ItemUBO {
  mat4 mvp; mat4 world; mat4 normalMatrix;
  vec4 baseColorFactor; vec4 emissiveOcclusion; vec4 metallicRoughness; vecTransform_placeholder;
} item;
```
（占位行删除——块成员与 shadow_depth.vert 一致后**追加** `vec4 ext3..ext6` 五行
与 texMorph/smpMat 声明;main = morph dP 循环(仅 POSITION 行)+ 
`gl_Position = u.lightViewProj * item.world * vec4(pos, 1.0);`。）
skinned 版 = 其 + aJoints/aWeights/JointUBO,skin·(pos+dP)。

- [ ] **Step 4: ShaderList 注册 + caps 20**

`shaders/CMakeLists.txt` 追加 4 行 `rd_compile_shader(...)`。
`core/rhi/rhi_constants.inc.h` 注释 19→20（slot0..19）;三后端
`caps_.set(Capability::max_texture_slots, 19)` → `20`（注释 `slot0..19(morph)`）。

- [ ] **Step 5: Vulkan 布局/池/映射**

`vulkan_device.cpp`：
- pbr 布局：`VkDescriptorSetLayoutBinding pb[24]` → `pb[25]`;
  循环后追加 `setPb(24, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);`
  （注释:slot19=例外 binding 24,23=smpMat）。
- 池：`{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 15 * kMaxDescSets}` → `16 * kMaxDescSets`。
- `bindTexture`：`slot >= 19` → `slot >= 20`。
- `descriptorSetFor` 纹理写循环的 `w.dstBinding = i + 4;` 改：

```cpp
    // slot19(texMorph)=binding 24(pbr 布局一次性例外;23 被 smpMat 占用)
    w.dstBinding = (key.pbrFamily && i == 19) ? 24u : i + 4;
```

- [ ] **Step 6: GLES 合并名**

`gles_device.cpp` kSamplerTable 追加：

```cpp
      {"SPIRV_Cross_CombinedtexMorphsmpMat", 19},
```

- [ ] **Step 7: 构建 + 实测合并名/MSL + 全量零回归**

```bash
./scripts/check.sh 2>&1 | tail -1
grep -o "uniform highp sampler2D SPIRV_Cross_CombinedtexMorph[A-Za-z0-9_]*" build/shaders_out/pbr_forward_morph.vert.gles | sort -u
grep -c "sampler(23)\|sampler(1)\b" build/shaders_out/pbr_forward_morph.vert.metal || true
```
预期:合并名出现;MSL 中 texMorph 经 `tex.read`(无新 sampler 参数);
ctest 全绿(无消费者 = 零回归)。

- [ ] **Step 8: Commit**

```bash
git add -A && git commit -m "feat(shaders): morph 顶点变体 ×4(texelFetch 增量累加)+槽19/binding24+caps 20(零回归)"
```

---

### Task 6: Renderer 管线矩阵 + record 选路 + 剔除/实例化排除 + 权重填充

**Files:**
- Modify: `core/renderer/renderer.h`（RendererShaderDesc + 成员 + ctx）
- Modify: `core/renderer/renderable.h`（RenderContext）
- Modify: `core/renderer/renderer.cpp`（init/ensureScenePipelines/endScene）
- Modify: `core/renderer/mesh_renderable.cpp`（管线选路 + slot19 绑定）

- [ ] **Step 1: RendererShaderDesc + RenderContext**

`renderer.h` RendererShaderDesc（`shadowInstVs` 后）追加：

```cpp
  std::vector<uint8_t> morphVs;              ///< pbr_forward_morph.vert(空=不支持)
  std::vector<uint8_t> morphSkinnedVs;       ///< pbr_forward_morph_skinned.vert
  std::vector<uint8_t> morphShadowVs;        ///< shadow_depth_morph.vert
  std::vector<uint8_t> morphSkinnedShadowVs; ///< shadow_depth_morph_skinned.vert
```

Renderer 成员（skinnedShadowPipeline_ 旁）：

```cpp
  ShaderModuleHandle morphVs_, morphSkvs_, morphSv_, morphSdsvs_;
  PipelineHandle morphPipeline_, morphSkinnedPipeline_;
  PipelineHandle morphShadowPipeline_, morphSkinnedShadowPipeline_;
```

`renderable.h` RenderContext（blendPipeline 后）追加：

```cpp
  /// morph 管线(空=该 mesh 无 morph;选路见 MeshRenderable::record)
  PipelineHandle morphPipeline;
  PipelineHandle morphSkinnedPipeline;
  PipelineHandle morphShadowPipe;
  PipelineHandle morphSkinnedShadowPipe;
```

- [ ] **Step 2: init/ensureScenePipelines 建 4 管线**

init（skvs_ 创建处旁）建 4 模块(空码=跳过);`ensureScenePipelines` 在蒙皮管线后：

```cpp
  // morph 管线(48B;ext 块在 vert 消费)
  if (morphVs_.valid() && fs_.valid()) {
    PipelineDesc mpd;
    mpd.vertexShader = morphVs_;
    mpd.fragmentShader = fs_;
    fillVertexLayout(mpd);
    mpd.cullMode = CullMode::None;
    mpd.depthTest = true;
    mpd.depthWrite = true;
    mpd.colorFormat = fmt;
    mpd.sampleCount = samples;
    mpd.separateSamplers = true;   // pbr 族
    if (morphPipeline_.valid()) dev_->destroyPipeline(morphPipeline_);
    morphPipeline_ = dev_->createPipeline(mpd);
  }
  // morph+skinned(80B 六属性;= skd 换 vert)
  if (morphSkvs_.valid() && fs_.valid()) {
    PipelineDesc msd = skd;  // 注意:skd.separateSamplers 已 false 化前取副本
    msd.vertexShader = morphSkvs_;
    // (skd 从 ppd 拷贝而来 separateSamplers=true;shadow 副本 ssd 才置 false)
    if (morphSkinnedPipeline_.valid()) dev_->destroyPipeline(morphSkinnedPipeline_);
    morphSkinnedPipeline_ = dev_->createPipeline(msd);
  }
  // shadow 变体(depthOnly;combined 布局)
  if (morphSv_.valid() && sfs_.valid()) {
    PipelineDesc msd2;
    msd2.vertexShader = morphSv_;
    msd2.fragmentShader = sfs_;
    fillVertexLayout(msd2);
    msd2.depthOnly = true;
    if (morphShadowPipeline_.valid()) dev_->destroyPipeline(morphShadowPipeline_);
    morphShadowPipeline_ = dev_->createPipeline(msd2);
  }
  if (morphSdsvs_.valid() && sfs_.valid()) {
    PipelineDesc mssd = ssd;  // 80B depthOnly(skinned 布局)
    mssd.vertexShader = morphSdsvs_;
    if (morphSkinnedShadowPipeline_.valid()) dev_->destroyPipeline(morphSkinnedShadowPipeline_);
    morphSkinnedShadowPipeline_ = dev_->createPipeline(mssd);
  }
```

（实现注意:取 `skd`/`ssd` 副本时机在其 `separateSamplers` 赋值之后——skd=true、
ssd=false;shadow_morph 走 combined 布局 ✓。shutdown/重建失效路径补 4 管线销毁。）

- [ ] **Step 3: MeshRenderable::record 选路 + slot19**

`mesh_renderable.cpp`：

- shadowPass 分支管线选择改：

```cpp
    const bool m0 = !mesh_->meshes().empty() && mesh_->meshes()[0].morph;
    const PipelineHandle pipe =
        skinned ? (m0 ? ctx.morphSkinnedShadowPipe : ctx.skinnedShadowPipe)
                : (m0 ? ctx.morphShadowPipe
                      : (mask0 ? ctx.shadowMaskPipe : ctx.shadowPipe));
```
（mask+morph → morph 阴影(不带裁剪);记一次性日志可省。）

- 场景 pass pbr 分支(else 分支)改:

```cpp
    } else {
      const bool m0 = g.morph;
      PipelineHandle pipe = ctx.pbrPipeline;
      if (m0 && skinnedOfMesh) pipe = ctx.morphSkinnedPipeline;
      else if (m0) pipe = ctx.morphPipeline;
      else if (g.skinned) pipe = ctx.skinnedPipeline;
      cmd->bindPipeline(pipe);
      ...  // 现有 UBO/纹理绑定不变
      if (g.morphTex.valid())
        cmd->bindTexture(19, g.morphTex, mesh_->sampler());  // slot19(smpMat 状态)
    }
```
（`skinnedOfMesh` = `g.skinned`;blend 分支同法插在 blend 管线前;
unlit 分支:若 `g.morph && g.material.unlit` 一次性 RD_LOGW 后按 unlit 渲染。）

- [ ] **Step 4: endScene 注入 + 排除 + 权重填充**

`renderer.cpp` endScene：

- RenderContext 填充处追加 4 字段(morphPipeline_ 等)。
- 视锥剔除 `culledBy`:`jointSlot_[idx] >= 0` 后追加 `|| meshMorph(idx)`:

```cpp
  auto meshMorph = [&](uint32_t idx) {
    const auto* r = static_cast<const MeshRenderable*>(queue_[idx].get());
    return !r->meshData().empty() && r->meshData()[0].morph;
  };
```
- 实例化 rid 条件追加 `&& !r->meshData()[0].morph`(两处:场景 pass 与阴影 pass 的
  分组条件,阴影 instanced 分组处同改)。
- ItemUBO 填充块(transOn 分支之后)追加:

```cpp
      // ---- morph 权重(ext5/ext6;ext3/ext4.w=目标数;零权重=零操作)----
      {
        const auto& md = meshes.empty() ? MeshGpuData{} : meshes[mi];
        const uint32_t mc = std::min<uint32_t>(uint32_t(md.morphWeights.size()), 8);
        iu.ext3[3] = iu.ext4[3] = float(md.morph ? mc : 0);
        for (uint32_t t = 0; t < 4 && t < mc; ++t) iu.ext5[t] = md.morphWeights[t];
        for (uint32_t t = 4; t < mc; ++t) iu.ext6[t - 4] = md.morphWeights[t];
      }
```
（transmission else 分支的 ext3/ext4 零值不受影响——ext4.w 目标数只在 morph
非零时有意义;transmission 关闭路径 ext4[3] 会被上面覆盖为 morph 计数 ✓ 语义正交。）

- [ ] **Step 5: 全量零回归 + 手动冒烟 + Commit**

```bash
./scripts/check.sh 2>&1 | tail -1
./build/tools/render_test/render_test --backend metal --model assets/Fox.glb --out /tmp/t6m.png 2>&1 | tail -1
git add -A && git commit -m "feat(renderer): morph 管线矩阵(4 变体)+record 选路+剔除/实例化排除+ext5/ext6 权重填充"
```
（现有资产无 morph → 全走旧路径,零回归。）

---

### Task 7: Animator weights 采样 + submit 重载 + C API

**Files:**
- Modify: `core/scene/animator.h` / `core/scene/animator.cpp`
- Modify: `core/renderer/renderer.h` / `renderer.cpp`（submit 重载）
- Modify: `core/api/rd_api.h` / `rd_api.cpp`（set_morph_weight + 渲染循环接线）

- [ ] **Step 1: Animator 权重采样**

`animator.h` 公有区追加：

```cpp
  /// 当前 morph 权重(模型首个 morph mesh;播放/暂停均输出最近采样值)。
  const std::vector<float>& morphWeights() const { return morphWeights_; }
  uint32_t morphTargetCount() const { return uint32_t(morphWeights_.size()); }
```
私有区追加成员与方法：

```cpp
  void sampleWeights(const AnimClipData& clip, float time, float blend);
  std::vector<float> morphWeights_;
  uint32_t morphNode_ = UINT32_MAX;  // 首个 morph mesh 节点(权重通道目标)
```

`animator.cpp`：
- `bind`:扫描 `model.meshes` 首个 `morph` mesh → `morphWeights_ = morphWeights`
  (静态初始值);`morphNode_ = nodeIndex`。
- `sampleWeights`:找 clip 中 `path==3 && node==morphNode_` 的通道,键值 lerp
  (与 sampleClip 同插值风格;STEP 退化保持);`blend<1` 时与现值线性混合
  (crossfade 用)。
- `update`:computeJoints 后调 `sampleWeights(active.clip, t, 1)` /
  淡入期间 `sampleWeights(fadeIn.clip, ft, fade)`。

- [ ] **Step 2: Renderer submit 重载**

`renderer.h`：

```cpp
  /// 提交形变渲染项(morphWeights 覆盖静态初始权重;可与 jointPalette 组合,
  /// 两者皆可空)。
  void submit(const std::shared_ptr<MeshRenderResource>& mesh, const math::Mat4& world,
              const math::Mat4* jointPalette, uint32_t jointCount,
              const float* morphWeights, uint32_t morphCount);
```

`renderer.cpp` 实现：委托现有逻辑 + 存 `morphOverride_`（`std::vector<std::vector<float>>`
与 queue_ 平行;空=无覆盖）;ItemUBO 填充改:覆盖存在时用覆盖值。

- [ ] **Step 3: C API + 渲染循环**

`rd_api.h`：

```c
/// 手动设置 morph 目标权重(当前模型首个 morph mesh;暂停动画手动生效;
/// 置渲染脏)。target 越界返回 RD_ERROR_INVALID_ARG。
RD_API rd_result rd_engine_set_morph_weight(rd_engine* e, uint32_t target, float w);
```

`rd_api.cpp`：
- engine 结构追加 `std::vector<float> morphOverride_;`（load_gltf 成功后按模型
  首 morph mesh 初始化为静态权重）。
- `rd_engine_set_morph_weight`：clamp 索引写覆盖 + `e->animator.pause(true)` +
  `e->renderDirty = true`。
- 渲染循环（`e->animator.update(dt)` 后的 submit 调用）改传权重：

```cpp
    const float* mw = nullptr;
    uint32_t mc = 0;
    if (!e->morphOverride_.empty()) {
      if (e->animator.playing()) {
        mw = e->animator.morphWeights().data();
        mc = e->animator.morphTargetCount();
      } else {
        mw = e->morphOverride_.data();
        mc = uint32_t(e->morphOverride_.size());
      }
    }
    e->renderer.submit(e->modelRes, rd::math::Mat4(1.0f),
                       e->animator.jointMatrices().data(),
                       uint32_t(e->animator.jointMatrices().size()), mw, mc);
```
（旧 submit 调用点替换;按需渲染脏源追加 morph 手动写已含。）

- [ ] **Step 4: 构建 + 全量回归 + Commit**

```bash
./scripts/check.sh 2>&1 | tail -1
git add -A && git commit -m "feat(scene): Animator morph 权重采样 + submit 重载 + rd_engine_set_morph_weight"
```

---

### Task 8: morph_demo 场景 + golden + 零操作/组合测试

**Files:**
- Modify: `scripts/fetch_assets.sh`（AnimatedMorphCube/Sphere）
- Modify: `tools/render_test/scenes.cpp`（morph_demo + 名单）
- Modify: `tools/render_test/main.cpp`（RendererShaderDesc 传 morph 4 码）
- Modify: `core/api/rd_api.cpp`（内嵌 shader get 4 条）
- Create: `tests/common/morph_gen.h/.cpp`（morph+skin 组合生成器）
- Test: `tests/renderer/morph_test.cpp`（新增 + CMake 注册）
- Golden: `tests/golden/morph_{cube,sphere,combo}_{metal,vulkan}.png`（运行生成）

- [ ] **Step 1: fetch_assets + 内嵌接线**

`fetch_assets.sh` P4-B 段后：

```bash
# P4-C morph targets golden 模型
dl AnimatedMorphCube.glb AnimatedMorphCube/glTF-Binary/AnimatedMorphCube.glb
dl AnimatedMorphSphere.glb AnimatedMorphSphere/glTF-Binary/AnimatedMorphSphere.glb
```
执行下载。`rd_api.cpp` get 链追加 4 条（morphVs←"pbr_forward_morph" 等 Vertex）。
`render_test/main.cpp` 两处 RendererShaderDesc 构造追加 4 码（load 新名字）。

- [ ] **Step 2: morph_gen 组合生成器**

`tests/common/morph_gen.h`：

```cpp
// 运行时生成确定性 morph+skin 组合资产(2 骨 quad + 1 目标 Y 膨胀 + weights clip)。
#pragma once
#include <string>
namespace rd::test {
/// 生成 quad_morph_skin.gltf/.bin 到 dir;结构 = writeSkinnedQuad 基础上:
/// primitive.targets[0] = POSITION 增量 (0,0.5,0);mesh.weights=[1.0];
/// clip "bend_wave":joint1 绕 Z 0→90° + weights 0→1(1s)。
std::string writeMorphSkinnedQuad(const std::string& dir);
} // namespace rd::test
```

`.cpp`：拷贝 `skinned_gen.cpp` 的 JSON/buffer 构造,targets 语义按 Task 1 测试样板
（bufferView 追加 36B 增量;JSON targets/weights;animation 双通道——samplers 加
weights 输出 SCALAR keys×1）。注册进 `tests/CMakeLists.txt`（若 skinned_gen 单列
则同法;搜 `skinned_gen.cpp` 现有条目旁追加）。

- [ ] **Step 3: morph_test.cpp**

骨架 = material_transmission_test.cpp 的 renderModel（拷贝改造;RenderContext 不动,
RendererShaderDesc 加 4 morph 码 load;`morphPreset` = transPreset 同型）:

```cpp
// 1) 零操作:同一模型 morph 数据置空前后逐像素一致
TEST(MorphGate, ZeroOpOnZeroWeights) {
  auto mesh = rd::primitives::makeSphere(0.5f, 32, 16);
  // 构造单目标 morph,权重恒 0
  const uint32_t vcount = uint32_t(mesh.vertices.size() / 12);
  mesh.morph = true;
  mesh.morphPosDeltas.assign(size_t(vcount) * 3, 0.05f);  // 非零增量但权重 0
  mesh.morphNormalDeltas.assign(size_t(vcount) * 3, 0.0f);
  mesh.morphWeights.assign(1, 0.0f);
  rd::ModelAsset withMorph;
  withMorph.meshes.push_back(mesh);
  withMorph.boundingRadius = 0.5f;
  rd::ModelAsset plain;                       // 同几何去 morph
  auto mesh2 = rd::primitives::makeSphere(0.5f, 32, 16);
  plain.meshes.push_back(std::move(mesh2));
  plain.boundingRadius = 0.5f;
  auto a = renderModel(rd::Backend::Metal, withMorph, nullptr);
  auto b = renderModel(rd::Backend::Metal, plain, nullptr);
  if (a.width == 0) GTEST_SKIP() << "Metal 不可用";
  EXPECT_TRUE(rd::test::compareSSIM(a.pixels.data(), b.pixels.data(), kW, kH, 0.0).pass);
}

// 2) 权重可见性:权重 0 vs 1 渲染显著不同
TEST(MorphGate, WeightChangesGeometry) {
  // 单目标 X 平移 0.8;w=1 → 球整体右移 → SSIM error > 0.01
  ... renderModel(b, w1) vs renderModel(b, w0) → EXPECT_GT(cmp.error, 0.01)
}

// 3) golden:cube/sphere(资产缺失 skip)
rd::test::Image renderMorphCube(rd::Backend b);   // assets/AnimatedMorphCube.glb
rd::test::Image renderMorphSphere(rd::Backend b); // assets/AnimatedMorphSphere.glb
// 渲染:load → Animator.bind/play(0)/update(0.5f)(确定性中点) → submit 带
// animator.morphWeights()
RD_GOLDEN_TEST(Morph, Cube, "morph_cube", 0.05, renderMorphCube)
RD_GOLDEN_TEST(Morph, Sphere, "morph_sphere", 0.05, renderMorphSphere)

// 4) golden:morph+skin 组合(运行时生成)
rd::test::Image renderMorphCombo(rd::Backend b);
RD_GOLDEN_TEST(Morph, Combo, "morph_combo", 0.05, renderMorphCombo)
```

CMake 注册 `renderer/morph_test.cpp`。

- [ ] **Step 4: morph_demo 场景**

`scenes.cpp`：kNames 追加 `"morph_demo"`;注册分支：

```cpp
  } else if (n == "morph_demo") {
    buildMorphDemo(dev, out);
```
`buildMorphDemo`（material_balls 样板）:球 0.5(48,24) ×1 + 单/双目标:
- t0 = 径向膨胀(posDeltas = normalize(pos)×0.15)
- t1 = Y 压扁(-y×0.25)
初始权重 0;DemoScene 追加 `morphPulse=true`(submitDemoScene 内
`w0=(sin(t)+1)/2, w1=(cos(t*0.7)+1)/2` 经新 submit 重载传入——submitDemoScene 签名
已有 renderer 引用 ✓)。相机/灯光照抄 material_balls。

- [ ] **Step 5: 生成 golden + 验证**

```bash
cmake --build build -j 8
RD_UPDATE_GOLDENS=1 ctest --test-dir build -R "Morph\." 2>&1 | tail -3
ls tests/golden | grep morph
ctest --test-dir build -R "Morph" --output-on-failure 2>&1 | tail -3
./build/tools/render_test/render_test --backend metal --scene morph_demo --out /tmp/md.png
./build/tools/img_check/img_check /tmp/md.png --min-coverage 0.03
```
预期:6 张 golden(cube/sphere/combo × 双后端);morph_demo img_check PASS +
目视球呼吸形变(程序化校验:两时刻截图 SSIM error > 0.01)。

- [ ] **Step 6: 全量回归 + Commit**

```bash
ctest --test-dir build --output-on-failure 2>&1 | tail -1
git add -A && git commit -m "feat(test): morph golden 三件(cube/sphere/combo 双后端)+零操作/权重语义测试+morph_demo 场景+fetch_assets 收录"
```

---

### Task 9: AGENTS.md + 收尾

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: AGENTS.md 更新**

- 「当前状态」追加 P4-C 完成条目(morph targets:GPU 纹理形变 RGBA16F texelFetch/
  ItemUBO 368B 权重/4 vert 变体/动画+手动权重/槽19 binding24 复用 smpMat;
  已知限制:8 目标上限/TANGENT 忽略/unlit+morph 不支持/mask+morph 阴影不裁剪/
  拾取绑定姿态);「下一步」改 `P4-D(Draco),或 P3(AR+鸿蒙)`。
- 「代码约定」:纹理槽位段补 `19=texMorph(RGBA16F,双行/目标,复用 smpMat)——共 20 槽`;
  UBO 约定段 ItemUBO 368B(ext5/ext6 权重,ext3/ext4.w=目标数);
  新增「morph targets」小节(增量纹理布局/先 morph 后 skin/排除实例化与剔除/
  无画质门控);golden 段追加 morph 三件 + `--scene morph_demo`。

- [ ] **Step 2: 全量验证**

```bash
./scripts/check.sh 2>&1 | tail -1
```

- [ ] **Step 3: Commit**

```bash
git add AGENTS.md && git commit -m "docs: P4-C morph targets 记入 AGENTS.md(槽20/ItemUBO 368B/4 vert 变体/已知限制)"
```

---

## 自审记录（Self-Review）

1. **Spec 覆盖**：spec §2 loader→Task 1/2;§3 资源→Task 3;§4 UBO/槽位→Task 4/5;
   §5 shader→Task 5;§6 renderer/animator/C API→Task 6/7;§7 降级→Task 1/3 内联;
   §8 测试→Task 8;§9 限制→Task 9 文档。✓ 无缺口。
2. **占位符扫描**：Task 1 Step 4 的语义骨架含双处修正注记（resize 外提/delta 双流），
   已明确"执行者按交错版为准";Task 5 Step 3 shadow 块有一行占位标记（`vecTransform_placeholder`）,
   注释已说明成员=shadow_depth.vert 原成员+追加 ext3..ext6——执行者按注释落地,
   非设计留白。✓
3. **类型一致性**：morphPosDeltas/morphNormalDeltas/morphWeights/morphTargetNames、
   morphTex/morph、morphVs_/morphSkvs_/morphSv_/morphSdsvs_、
   morphPipeline/morphSkinnedPipeline/morphShadowPipe/morphSkinnedShadowPipe、
   set_morph_weight/morphOverride_ 全文一致;ItemUBO 368/ext5/ext6/ext4.w 三处
   (CPU/shader/填充)一致;slot19=binding24 在 Vulkan/GLES/Metal 三处一致。✓
