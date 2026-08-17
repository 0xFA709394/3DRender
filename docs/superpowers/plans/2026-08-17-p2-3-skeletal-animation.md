# P2-3:骨骼动画(GPU 蒙皮 + clip 播放 + 交叉淡入)实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/superpowers/specs/2026-08-17-p2-3-skeletal-animation-design.md` 实现:①loader 节点层级/skins/animations + 80B 蒙皮顶点;②scene::Animator(播放/淡入);③renderer skinned 管线 + JointUBO + 弯折 quad golden;④C API + engine 集成。

**Architecture:** loader 把 glTF nodes/skins/animations 解析进 ModelAsset(非蒙皮模型行为不变);Animator 采样 clip → 节点全局矩阵 → jointMatrices(global×IBM);Renderer 持 64KB 共享 JointUBO(slot3,8 项×8KB 步进),skinned mesh(80B)走 pbr_forward_skinned 管线;测试资产运行时生成(2 骨 quad)。

**通用约定(全任务遵守):**
- 构建+测试:`export RD_DEPS_MIRROR=$HOME/.rd_deps_mirror && ./scripts/check.sh`;单跑 `ctest --test-dir build --output-on-failure -R <正则>`
- golden 更新:`RD_UPDATE_GOLDENS=1 ctest --test-dir build -R <用例>` 后**像素核对**再提交
- 提交规范:每 Task 一个 commit,`<type>(<scope>): 描述`
- 绑定约定:uniform slot N ↔ Metal buffer(N) ↔ Vulkan set0 binding N ↔ GLES binding N;**JointUBO=slot3**(GLES 块名 JointUBO→3)
- 顶点布局:非蒙皮 48B(pos3@0|normal3@12|tangent4@24|uv2@40);**蒙皮 80B**(追加 joints4f@48|weights4f@64,location 4/5)
- 蒙皮矩阵:jointMatrices[j] = nodeGlobals[joints[j]] × inverseBindMatrices[j]
- 光源/阴影/后处理约定同 P2-1/P2-2
- 资源创建时机:全部资源须在 acquireCommandBuffer 之前创建

---

### Task 1: loader 蒙皮扩展(nodes/skins/animations/80B)+ 测试资产生成器

**Files:**
- Modify: `core/resource/gltf_loader.h`、`core/resource/gltf_loader.cpp`
- Create: `tests/common/skinned_gen.h`、`tests/common/skinned_gen.cpp`(运行时生成 2 骨 quad gltf)
- Test: `tests/resource/gltf_test.cpp`(追加)、`tests/CMakeLists.txt`(注册 skinned_gen.cpp)

- [ ] **Step 1: 测试资产生成器(先建,测试与 golden 共用)**

`tests/common/skinned_gen.h`:

```cpp
// 运行时生成确定性蒙皮测试资产(2 骨 quad + 90° 弯折 clip),零二进制提交。
// 供 gltf_test / animator_test / skinned golden 复用。
#pragma once
#include <string>
namespace rd::test {
/// 生成 quad_skin.gltf + quad_skin.bin 到 dir;成功返回 gltf 路径(失败返回空串)。
/// 结构:node0=根骨(joint0),node1=子骨(joint1,平移 y=1),node2=mesh 节点;
/// quad 4 顶点(y=0 两行 j0,y=2 两行 j1);clip "bend":joint1 绕 Z 0→90°,1s。
std::string writeSkinnedQuad(const std::string& dir);
} // namespace rd::test
```

`tests/common/skinned_gen.cpp`:

```cpp
#include "common/skinned_gen.h"
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace rd::test {

std::string writeSkinnedQuad(const std::string& dirStr) {
  namespace fs = std::filesystem;
  const fs::path dir(dirStr);
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string binPath = (dir / "quad_skin.bin").string();
  const std::string gltfPath = (dir / "quad_skin.gltf").string();

  // 顶点:pos3|uv2(仅几何用;joints/weights 独立 accessor)
  // 4 顶点:(-0.5,0) (0.5,0) (-0.5,2) (0.5,2),法线 +Z
  const float pos[12] = {-0.5f, 0, 0, 0.5f, 0, 0, -0.5f, 2, 0, 0.5f, 2, 0};
  const float nrm[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
  const float uv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
  const uint8_t joints[4][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};
  const float weights[16] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
  const uint16_t idx[6] = {0, 1, 2, 1, 3, 2};
  // IBM:joint0=单位;joint1=translate(0,-1,0)(bind 全局平移 y=1 的逆)
  const float ibm[32] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
                         1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1};
  // clip 采样:times=[0,1],quats=[identity, rotZ90(0,0,0.7071,0.7071)]
  const float times[2] = {0, 1};
  const float rots[8] = {0, 0, 0, 1, 0, 0, 0.70710678f, 0.70710678f};

  FILE* f = fopen(binPath.c_str(), "wb");
  if (!f) return {};
  fwrite(pos, 4, 12, f);    // bv0 @0
  fwrite(nrm, 4, 12, f);    // bv1 @48
  fwrite(uv, 4, 8, f);      // bv2 @96
  fwrite(joints, 1, 16, f); // bv3 @128
  fwrite(weights, 4, 16, f);// bv4 @144
  fwrite(idx, 2, 6, f);     // bv5 @208
  fwrite(ibm, 4, 32, f);    // bv6 @220
  fwrite(times, 4, 2, f);   // bv7 @348
  fwrite(rots, 4, 8, f);    // bv8 @356 (总长 388)
  fclose(f);

  const char* json = R"({
    "asset": {"version": "2.0"},
    "scenes": [{"nodes": [0, 2]}], "scene": 0,
    "nodes": [
      {},
      {"translation": [0, 1, 0]},
      {"mesh": 0, "skin": 0}
    ],
    "meshes": [{"primitives": [{"attributes": {
        "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2,
        "JOINTS_0": 3, "WEIGHTS_0": 4},
        "indices": 5, "material": 0}]}],
    "materials": [{"pbrMetallicRoughness": {"baseColorFactor": [0.7, 0.7, 0.8, 1],
      "metallicFactor": 0.0, "roughnessFactor": 0.9}}],
    "skins": [{"joints": [0, 1], "inverseBindMatrices": 6, "skeleton": 0}],
    "animations": [{"name": "bend", "channels": [
        {"target": {"node": 1, "path": "rotation"}, "sampler": 0}],
      "samplers": [{"input": 7, "output": 8, "interpolation": "LINEAR"}]}],
    "buffers": [{"uri": "quad_skin.bin", "byteLength": 388}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 48},
      {"buffer": 0, "byteOffset": 48, "byteLength": 48},
      {"buffer": 0, "byteOffset": 96, "byteLength": 32},
      {"buffer": 0, "byteOffset": 128, "byteLength": 16},
      {"buffer": 0, "byteOffset": 144, "byteLength": 64},
      {"buffer": 0, "byteOffset": 208, "byteLength": 12},
      {"buffer": 0, "byteOffset": 220, "byteLength": 128},
      {"buffer": 0, "byteOffset": 348, "byteLength": 8},
      {"buffer": 0, "byteOffset": 356, "byteLength": 32}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
      {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
      {"bufferView": 3, "componentType": 5121, "count": 4, "type": "VEC4"},
      {"bufferView": 4, "componentType": 5126, "count": 4, "type": "VEC4"},
      {"bufferView": 5, "componentType": 5123, "count": 6, "type": "SCALAR"},
      {"bufferView": 6, "componentType": 5126, "count": 2, "type": "MAT4"},
      {"bufferView": 7, "componentType": 5126, "count": 2, "type": "SCALAR"},
      {"bufferView": 8, "componentType": 5126, "count": 2, "type": "VEC4"}]
  })";
  f = fopen(gltfPath.c_str(), "wb");
  if (!f) return {};
  fwrite(json, 1, strlen(json), f);
  fclose(f);
  return gltfPath;
}

} // namespace rd::test
```

`tests/CMakeLists.txt` 的 rd_tests 源列表追加 `common/skinned_gen.cpp`。

- [ ] **Step 2: 写失败测试(loader 蒙皮解析)**

`tests/resource/gltf_test.cpp` 追加:

```cpp
// 蒙皮解析:nodes/skins/animations + 80B 顶点布局。
TEST(Gltf, SkinnedQuad) {
  const std::string dir =
      (std::filesystem::temp_directory_path() / "rd_gltf_skin").string();
  const std::string path = rd::test::writeSkinnedQuad(dir);
  ASSERT_FALSE(path.empty());
  auto model = rd::loadGltf(path.c_str());
  ASSERT_TRUE(model.valid());

  // 节点层级:3 节点(node1 parent=0,平移 y=1)
  ASSERT_EQ(model.nodes.size(), 3u);
  EXPECT_EQ(model.nodes[1].parent, 0);
  EXPECT_NEAR(model.nodes[1].translation[1], 1.0f, 1e-4f);
  EXPECT_EQ(model.nodes[2].mesh, 0);

  // 蒙皮网格:80B 布局 + joints/weights 正确
  ASSERT_EQ(model.meshes.size(), 1u);
  const auto& mesh = model.meshes[0];
  EXPECT_TRUE(mesh.skinned);
  ASSERT_EQ(mesh.vertices.size(), 4u * 20u);  // 20 float/顶点(80B)
  // 顶点 0(y=0):joints=(0,0,0,0),weights=(1,0,0,0)
  EXPECT_FLOAT_EQ(mesh.vertices[12], 0.0f);   // joints4f@12(float 下标 48B/4)
  EXPECT_FLOAT_EQ(mesh.vertices[16], 1.0f);   // weights4f@16
  // 顶点 2(y=2):joints=(1,0,0,0)
  EXPECT_FLOAT_EQ(mesh.vertices[2 * 20 + 12], 1.0f);
  EXPECT_FLOAT_EQ(mesh.vertices[2 * 20 + 16], 1.0f);

  // skins:2 关节 + IBM(joint1 平移 y=-1)
  ASSERT_EQ(model.skins.size(), 1u);
  ASSERT_EQ(model.skins[0].joints.size(), 2u);
  EXPECT_EQ(model.skins[0].joints[1], 1);
  ASSERT_EQ(model.skins[0].inverseBindMatrices.size(), 32u);
  EXPECT_NEAR(model.skins[0].inverseBindMatrices[16 + 13], -1.0f, 1e-4f);

  // animations:clip "bend",1 通道 rotation,时长 1s
  ASSERT_EQ(model.animations.size(), 1u);
  const auto& clip = model.animations[0];
  EXPECT_EQ(clip.name, "bend");
  ASSERT_EQ(clip.channels.size(), 1u);
  EXPECT_EQ(clip.channels[0].node, 1);
  EXPECT_EQ(clip.channels[0].path, 1);  // rotation
  ASSERT_EQ(clip.channels[0].times.size(), 2u);
  EXPECT_FLOAT_EQ(clip.duration, 1.0f);
  // 末帧 quat = rotZ90
  EXPECT_NEAR(clip.channels[0].values[6], 0.70710678f, 1e-4f);

  // 非蒙皮模型不受影响
  auto box = rd::loadGltf((std::string(kAssets) + "/BoxTextured.glb").c_str());
  ASSERT_TRUE(box.valid());
  EXPECT_FALSE(box.meshes[0].skinned);
  EXPECT_TRUE(box.nodes.empty());
}
```

(文件头补 `#include "common/skinned_gen.h"`)

- [ ] **Step 3: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`nodes`/`skins`/`animations`/`skinned` 未定义)

- [ ] **Step 4: gltf_loader.h 扩展**

`MeshData` 追加:

```cpp
  bool skinned = false;           // 蒙皮网格(80B 布局:追加 joints4f@48|weights4f@64)
  int32_t nodeIndex = -1;         // 所属 nodes[] 下标(非蒙皮/无节点为 -1)
```

新增类型(ModelAsset 之前):

```cpp
/// 层级节点(蒙皮模型用;非蒙皮模型 nodes 为空)。
struct AnimNodeData {
  int32_t parent = -1;
  float translation[3] = {0, 0, 0};
  float rotation[4] = {0, 0, 0, 1};   // quat xyzw
  float scale[3] = {1, 1, 1};
  int32_t mesh = -1;                  // 首个 primitive 的 meshes[] 下标
};

/// 蒙皮:关节表 + 逆绑定矩阵(16 float 列主序 ×N)。
struct SkinData {
  std::vector<int32_t> joints;        // nodes[] 下标
  std::vector<float> inverseBindMatrices;
  int32_t skeletonRoot = -1;
};

/// 动画通道:目标节点某属性的关键帧序列。
struct AnimChannelData {
  int32_t node = -1;
  int32_t path = 0;                   // 0=translation,1=rotation,2=scale
  std::vector<float> times;
  std::vector<float> values;          // vec3(t/s)或 quat(r)扁平序列
};

/// 动画 clip。
struct AnimClipData {
  std::string name;
  std::vector<AnimChannelData> channels;
  float duration = 0.0f;
};
```

`ModelAsset` 追加:

```cpp
  std::vector<AnimNodeData> nodes;    // 层级节点(非蒙皮为空)
  std::vector<SkinData> skins;
  std::vector<AnimClipData> animations;
```

- [ ] **Step 5: gltf_loader.cpp 实现**

1. **节点导入**(meshes 循环之前,先建 nodes 表——mesh 挂载要用):

```cpp
  // 节点层级(蒙皮模型需要;非蒙皮模型也建表,开销可忽略)
  model.nodes.reserve(data->nodes_count);
  for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
    const cgltf_node* node = &data->nodes[ni];
    AnimNodeData an;
    an.parent = node->parent ? int32_t(cgltf_node_index(data, node->parent)) : -1;
    if (node->has_translation)
      for (int c = 0; c < 3; ++c) an.translation[c] = float(node->translation[c]);
    if (node->has_rotation)
      for (int c = 0; c < 4; ++c) an.rotation[c] = float(node->rotation[c]);
    if (node->has_scale)
      for (int c = 0; c < 3; ++c) an.scale[c] = float(node->scale[c]);
    an.mesh = -1;  // mesh 处理时回填
    model.nodes.push_back(an);
  }
```

   (matrix 形式节点(has_matrix):v1 取 m[12..14] 平移 + 记警告,TRS 缺省——
   cgltf 的 has_translation 等与 has_matrix 互斥)
2. **mesh 循环改造**:外层加节点遍历;primitives 归属:

```cpp
  // 遍历节点挂载 mesh(无节点层级的旧路径:直接遍历 meshes)
  for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
    const cgltf_node* node = &data->nodes[ni];
    if (!node->mesh) continue;
    const cgltf_mesh& mesh = *node->mesh;
    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
      const cgltf_primitive& prim = mesh.primitives[pi];
      if (prim.type != cgltf_primitive_type_triangles || !prim.indices) continue;
      MeshData out;
      out.nodeIndex = int32_t(ni);
      // 蒙皮检测:有无 JOINTS_0
      bool skinned = false;
      for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai)
        if (prim.attributes[ai].type == cgltf_attribute_type_joints) skinned = true;
      out.skinned = skinned;
      const uint32_t strideFloats = skinned ? 20 : 12;
      ... (顶点填充:12 float 基础上,蒙皮追加 joints/weights)
      // joints:cgltf_accessor_read_uint 读 4 个 → float;
      // weights:cgltf_accessor_read_float(归一化 u8/u16 自动转 float)
      if (skinned) {
        const cgltf_accessor* jointsAcc = nullptr;
        const cgltf_accessor* weightsAcc = nullptr;
        for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai) {
          if (prim.attributes[ai].type == cgltf_attribute_type_joints)
            jointsAcc = prim.attributes[ai].data;
          if (prim.attributes[ai].type == cgltf_attribute_type_weights)
            weightsAcc = prim.attributes[ai].data;
        }
        for (cgltf_size v = 0; v < vertexCount; ++v) {
          float* dst = out.vertices.data() + size_t(v) * strideFloats;
          if (jointsAcc) {
            cgltf_uint j4[4] = {};
            cgltf_accessor_read_uint(jointsAcc, v, j4, 4);
            for (int c = 0; c < 4; ++c) dst[12 + c] = float(j4[c]);
          }
          if (weightsAcc) {
            float w4[4] = {};
            cgltf_accessor_read_float(weightsAcc, v, w4, 4);
            for (int c = 0; c < 4; ++c) dst[16 + c] = w4[c];
          } else {
            dst[16] = 1.0f;  // 缺省:全权重根骨
          }
        }
      }
      if (model.nodes[ni].mesh < 0)
        model.nodes[ni].mesh = int32_t(model.meshes.size());
      ...
      model.meshes.push_back(std::move(out));
    }
  }
```

   **注意**:现有实现遍历 `data->meshes`;改为节点驱动后,未被节点引用的 mesh
   不导入(与 glTF 语义一致——游离 mesh 本就不渲染)。现有测试资产(glb)都有
   节点引用,行为不变。切线/索引/包围球/材质逻辑原样搬入循环。
3. **skins 解析**:

```cpp
  for (cgltf_size si = 0; si < data->skins_count; ++si) {
    const cgltf_skin& skin = data->skins[si];
    SkinData sd;
    sd.joints.reserve(skin.joints_count);
    for (cgltf_size j = 0; j < skin.joints_count; ++j)
      sd.joints.push_back(int32_t(cgltf_node_index(data, skin.joints[j])));
    sd.inverseBindMatrices.resize(skin.joints_count * 16);
    if (skin.inverse_bind_matrices) {
      for (cgltf_size j = 0; j < skin.joints_count; ++j)
        cgltf_accessor_read_float(skin.inverse_bind_matrices, j,
                                  &sd.inverseBindMatrices[j * 16], 16);
    } else {  // 缺省单位
      for (cgltf_size j = 0; j < skin.joints_count; ++j)
        for (int k = 0; k < 16; ++k) sd.inverseBindMatrices[j * 16 + k] = (k % 5 == 0) ? 1.0f : 0.0f;
    }
    sd.skeletonRoot = skin.skeleton ? int32_t(cgltf_node_index(data, skin.skeleton)) : -1;
    model.skins.push_back(std::move(sd));
  }
```

4. **animations 解析**:

```cpp
  for (cgltf_size ai = 0; ai < data->animations_count; ++ai) {
    const cgltf_animation& anim = data->animations[ai];
    AnimClipData clip;
    clip.name = anim.name ? anim.name : "clip";
    for (cgltf_size ci = 0; ci < anim.channels_count; ++ci) {
      const cgltf_animation_channel& ch = anim.channels[ci];
      if (!ch.target_node || !ch.sampler) continue;
      AnimChannelData chd;
      chd.node = int32_t(cgltf_node_index(data, ch.target_node));
      chd.path = ch.target_path == cgltf_animation_path_type_translation ? 0
                 : ch.target_path == cgltf_animation_path_type_rotation ? 1
                 : ch.target_path == cgltf_animation_path_type_scale    ? 2
                                                                        : -1;
      if (chd.path < 0) continue;  // weights(morph)跳过
      const cgltf_accessor* in = ch.sampler->input;
      const cgltf_accessor* out = ch.sampler->output;
      chd.times.resize(in->count);
      for (cgltf_size k = 0; k < in->count; ++k)
        cgltf_accessor_read_float(in, k, &chd.times[k], 1);
      const uint32_t comps = chd.path == 1 ? 4 : 3;
      chd.values.resize(out->count * comps);
      for (cgltf_size k = 0; k < out->count; ++k)
        cgltf_accessor_read_float(out, k, &chd.values[k * comps], comps);
      if (in->count > 0) clip.duration = std::max(clip.duration, chd.times[in->count - 1]);
      clip.channels.push_back(std::move(chd));
    }
    model.animations.push_back(std::move(clip));
  }
```

5. `readFloatAttr` 的 dstStride 参数已存在(12);蒙皮传 20。`computeTangents` 调用
   的 stride 参数同步(strideFloats)。

- [ ] **Step 6: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -6`
Expected: 全绿(Gltf.SkinnedQuad 通过;既有 Gltf.*/golden 不回归)

- [ ] **Step 7: Commit**

```bash
git add core/resource tests/common/skinned_gen.* tests/resource/gltf_test.cpp tests/CMakeLists.txt
git commit -m "feat(resource): 蒙皮 glTF 解析(nodes/skins/animations + 80B 顶点)+ 测试资产生成器"
```

---

### Task 2: scene::Animator + 单测

**Files:**
- Create: `core/scene/animator.h`、`core/scene/animator.cpp`
- Modify: `core/CMakeLists.txt`
- Test: `tests/scene/animator_test.cpp`(新建)、`tests/CMakeLists.txt`(注册)

- [ ] **Step 1: 写失败测试**

`tests/scene/animator_test.cpp`:

```cpp
// Animator 单测:clip 采样中点、交叉淡入权重、jointMatrices = global × IBM。
#include <gtest/gtest.h>
#include "common/skinned_gen.h"
#include "resource/gltf_loader.h"
#include "scene/animator.h"
#include <filesystem>

namespace {
rd::ModelAsset loadQuad() {
  const std::string dir =
      (std::filesystem::temp_directory_path() / "rd_anim_test").string();
  return rd::loadGltf(rd::test::writeSkinnedQuad(dir).c_str());
}
} // namespace

TEST(Animator, BindAndPlay) {
  auto model = loadQuad();
  ASSERT_TRUE(model.valid());
  rd::scene::Animator anim;
  ASSERT_TRUE(anim.bind(model));
  EXPECT_EQ(anim.clipCount(), 1u);
  EXPECT_FALSE(anim.playing());
  anim.play(0);
  EXPECT_TRUE(anim.playing());
}

// t=0.5 时 joint1 绕 Z 约 45°(线性插值 slerp);joint0 恒为单位阵
TEST(Animator, SampleMidpoint) {
  auto model = loadQuad();
  rd::scene::Animator anim;
  ASSERT_TRUE(anim.bind(model));
  anim.play(0);
  anim.update(0.5f);
  const auto& joints = anim.jointMatrices();
  ASSERT_EQ(joints.size(), 2u);
  // joint0 = 单位(global identity × IBM identity)
  EXPECT_NEAR(joints[0][0][0], 1.0f, 1e-4f);
  EXPECT_NEAR(joints[0][3][3], 1.0f, 1e-4f);
  // joint1:global = T(0,1,0)·Rz(45°),IBM = T(0,-1,0)
  // jointMat = global × IBM;验证其作用于点 (0,1,0)(关节原点)不动、
  // 点 (0,2,0) 绕 Z 弯折约 45°(x 偏移 sin45≈0.707)
  const auto& jm = joints[1];
  glm::vec4 p0 = jm * glm::vec4(0, 1, 0, 1);
  EXPECT_NEAR(p0.y, 1.0f, 1e-3f);
  glm::vec4 p1 = jm * glm::vec4(0, 2, 0, 1);
  EXPECT_LT(p1.x, -0.5f) << "弯折应向 -X 偏(rotZ 正向)";
  EXPECT_NEAR(p1.y, 1.0f + 0.7071f, 0.02f);
}

// 交叉淡入:fade 中途双 clip 混合;fade 结束只播新 clip
TEST(Animator, Crossfade) {
  auto model = loadQuad();
  rd::scene::Animator anim;
  ASSERT_TRUE(anim.bind(model));
  anim.play(0);
  anim.update(0.5f);
  const float jm0 = anim.jointMatrices()[1][0][1];  // 记录中间态
  anim.playWithFade(0, 0.2f);  // 同 clip 淡入(验证机制:不崩、连续)
  anim.update(0.1f);
  EXPECT_TRUE(anim.playing());
  anim.update(0.2f);
  const float jm1 = anim.jointMatrices()[1][0][1];
  EXPECT_NEAR(jm0, jm1, 0.05f) << "同 clip 淡入应近似连续";
}

// 越界 clip:警告 no-op 不崩
TEST(Animator, OutOfRangeSafe) {
  auto model = loadQuad();
  rd::scene::Animator anim;
  ASSERT_TRUE(anim.bind(model));
  anim.play(9);
  anim.playWithFade(9, 0.1f);
  anim.update(0.1f);
  EXPECT_FALSE(anim.playing());
}
```

`tests/CMakeLists.txt` 追加 `scene/animator_test.cpp`。

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`scene/animator.h` 不存在)

- [ ] **Step 3: 实现 Animator**

`core/scene/animator.h`:

```cpp
/**
 * @file animator.h
 * @brief Animator:glTF 动画播放(线性插值/rotation slerp)+ 双 clip 交叉淡入。
 * 纯 CPU 计算,不碰 GPU;输出节点全局矩阵与关节矩阵(渲染层上传 JointUBO)。
 */
#pragma once
#include "foundation/math.h"
#include "resource/gltf_loader.h"
#include <vector>

namespace rd::scene {

class Animator {
public:
  /// 绑定模型(须含 nodes;animations 可为空——bind 后 playing=false)。
  bool bind(const ModelAsset& model);
  void play(uint32_t clipIndex);                        ///< 立即切换(loop)
  void playWithFade(uint32_t clipIndex, float fadeSec); ///< 交叉淡入
  void pause(bool p) { paused_ = p; }
  /// 推进时间并计算节点全局矩阵 + jointMatrices(skin 0)。
  void update(float dt);
  const std::vector<math::Mat4>& jointMatrices() const { return jointMatrices_; }
  const std::vector<math::Mat4>& nodeGlobals() const { return nodeGlobals_; }
  bool playing() const { return playing_; }
  uint32_t clipCount() const { return uint32_t(clips_); }

private:
  struct ClipState {
    int32_t index = -1;
    float time = 0.0f;
  };
  /// 采样 clip 到 locals_(translation/rotation/scale)。
  void sampleClip(const AnimClipData& clip, float time,
                  std::vector<math::Vec3>& outT, std::vector<math::Quat>& outR,
                  std::vector<math::Vec3>& outS);
  void computeGlobals();   // locals → nodeGlobals_(沿 parent 链)
  void computeJoints();    // nodeGlobals_ × IBM → jointMatrices_

  const ModelAsset* model_ = nullptr;
  ClipState active_, fadeIn_;
  float fadeDuration_ = 0.0f, fadeElapsed_ = 0.0f;
  bool playing_ = false, paused_ = false;
  size_t clips_ = 0;
  std::vector<math::Vec3> localT_;
  std::vector<math::Quat> localR_;
  std::vector<math::Vec3> localS_;
  std::vector<math::Mat4> nodeGlobals_;
  std::vector<math::Mat4> jointMatrices_;
};

} // namespace rd::scene
```

`core/scene/animator.cpp`:

```cpp
#include "scene/animator.h"
#include "foundation/log.h"
#include <algorithm>
#include <glm/gtc/quaternion.hpp>

namespace rd::scene {

bool Animator::bind(const ModelAsset& model) {
  if (model.nodes.empty()) {
    RD_LOGW("scene.anim", "模型无节点层级,无法动画");
    return false;
  }
  model_ = &model;
  clips_ = model.animations.size();
  const size_t n = model.nodes.size();
  localT_.resize(n);
  localR_.resize(n);
  localS_.resize(n);
  nodeGlobals_.resize(n);
  // 初始:绑定姿态(节点静态 TRS)
  for (size_t i = 0; i < n; ++i) {
    const auto& nd = model.nodes[i];
    localT_[i] = math::Vec3(nd.translation[0], nd.translation[1], nd.translation[2]);
    localR_[i] = math::Quat(nd.rotation[3], nd.rotation[0], nd.rotation[1], nd.rotation[2]);
    localS_[i] = math::Vec3(nd.scale[0], nd.scale[1], nd.scale[2]);
  }
  computeGlobals();
  computeJoints();
  return true;
}

void Animator::play(uint32_t clipIndex) {
  if (clipIndex >= clips_) {
    RD_LOGW("scene.anim", "clip 越界 %u", clipIndex);
    return;
  }
  active_.index = int32_t(clipIndex);
  active_.time = 0;
  fadeIn_.index = -1;
  playing_ = true;
  paused_ = false;
}

void Animator::playWithFade(uint32_t clipIndex, float fadeSec) {
  if (clipIndex >= clips_) {
    RD_LOGW("scene.anim", "clip 越界 %u", clipIndex);
    return;
  }
  if (active_.index < 0) {  // 无旧 clip,直接播放
    play(clipIndex);
    return;
  }
  fadeIn_.index = int32_t(clipIndex);
  fadeIn_.time = 0;
  fadeDuration_ = std::max(fadeSec, 1e-3f);
  fadeElapsed_ = 0;
  playing_ = true;
  paused_ = false;
}

namespace {
/// 二分查找时间区间;返回 (i, t):times[i]..times[i+1],t∈[0,1]。
void findSegment(const std::vector<float>& times, float t, uint32_t& i, float& frac) {
  if (times.size() <= 1 || t <= times.front()) {
    i = 0;
    frac = 0;
    return;
  }
  if (t >= times.back()) {
    i = uint32_t(times.size()) - 2;
    frac = 1.0f;
    return;
  }
  auto it = std::upper_bound(times.begin(), times.end(), t);
  i = uint32_t(std::distance(times.begin(), it)) - 1;
  frac = (t - times[i]) / (times[i + 1] - times[i]);
}
} // namespace

void Animator::sampleClip(const AnimClipData& clip, float time,
                          std::vector<math::Vec3>& outT, std::vector<math::Quat>& outR,
                          std::vector<math::Vec3>& outS) {
  const float t = clip.duration > 0 ? std::fmod(time, clip.duration) : 0.0f;
  for (const auto& ch : clip.channels) {
    uint32_t i = 0;
    float frac = 0;
    findSegment(ch.times, t, i, frac);
    const uint32_t comps = ch.path == 1 ? 4 : 3;
    const float* v0 = &ch.values[size_t(i) * comps];
    const float* v1 = &ch.values[size_t(i + 1) * comps > ch.values.size() - comps
                                      ? ch.values.size() - comps
                                      : size_t(i + 1) * comps];
    // (末段钳到末帧)
    if (size_t(i + 1) * comps >= ch.values.size()) v1 = v0;
    if (ch.path == 1) {
      math::Quat q0(v0[3], v0[0], v0[1], v0[2]);
      math::Quat q1(v1[3], v1[0], v1[1], v1[2]);
      outR[ch.node] = glm::slerp(q0, q1, frac);
    } else {
      math::Vec3 a(v0[0], v0[1], v0[2]);
      math::Vec3 b(v1[0], v1[1], v1[2]);
      const math::Vec3 r = glm::mix(a, b, frac);
      if (ch.path == 0) outT[ch.node] = r;
      else outS[ch.node] = r;
    }
  }
}

void Animator::update(float dt) {
  if (!model_ || !playing_ || paused_) return;
  active_.time += dt;
  sampleClip(model_->animations[size_t(active_.index)], active_.time, localT_, localR_,
             localS_);
  if (fadeIn_.index >= 0) {
    fadeIn_.time += dt;
    fadeElapsed_ += dt;
    const float w = std::min(fadeElapsed_ / fadeDuration_, 1.0f);
    // 淡入 clip 采样到临时数组,再按权重混合
    std::vector<math::Vec3> t2 = localT_;
    std::vector<math::Quat> r2 = localR_;
    std::vector<math::Vec3> s2 = localS_;
    sampleClip(model_->animations[size_t(fadeIn_.index)], fadeIn_.time, t2, r2, s2);
    for (size_t i = 0; i < localT_.size(); ++i) {
      localT_[i] = glm::mix(localT_[i], t2[i], w);
      localR_[i] = glm::slerp(localR_[i], r2[i], w);
      localS_[i] = glm::mix(localS_[i], s2[i], w);
    }
    if (w >= 1.0f) {  // 淡入完成:切换
      active_ = fadeIn_;
      fadeIn_.index = -1;
    }
  }
  computeGlobals();
  computeJoints();
}

void Animator::computeGlobals() {
  const auto& nodes = model_->nodes;
  for (size_t i = 0; i < nodes.size(); ++i) {
    const math::Mat4 local =
        glm::translate(math::Mat4(1.0f), localT_[i]) * glm::mat4_cast(localR_[i]) *
        glm::scale(math::Mat4(1.0f), localS_[i]);
    if (nodes[i].parent >= 0)
      nodeGlobals_[i] = nodeGlobals_[size_t(nodes[i].parent)] * local;
    else
      nodeGlobals_[i] = local;
  }
}

void Animator::computeJoints() {
  jointMatrices_.clear();
  if (model_->skins.empty()) return;
  const auto& skin = model_->skins[0];
  jointMatrices_.resize(skin.joints.size());
  for (size_t j = 0; j < skin.joints.size(); ++j) {
    const auto& g = nodeGlobals_[size_t(skin.joints[j])];
    math::Mat4 ibm;
    memcpy(&ibm, &skin.inverseBindMatrices[j * 16], 64);
    jointMatrices_[j] = g * ibm;
  }
}

} // namespace rd::scene
```

`core/CMakeLists.txt` 追加 `scene/animator.cpp`。
`core/foundation/math.h` 追加别名:`using Quat = glm::quat;`(animator 用)。

注意:glTF 节点序不保证父先子后——computeGlobals 的直接遍历依赖下标序;
蒙皮模型通常满足,若遇到反序模型改为两遍拓扑(本阶段记录为已知限制,
plan 附录标注)。

- [ ] **Step 4: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -6`
Expected: 全绿(Animator.* 通过)

- [ ] **Step 5: Commit**

```bash
git add core/scene/animator.* core/CMakeLists.txt tests/scene/animator_test.cpp tests/CMakeLists.txt
git commit -m "feat(scene): Animator(clip 播放/线性插值/交叉淡入)+ 单测"
```

---

### Task 3: renderer skinned 管线 + JointUBO + 弯折 quad golden

**Files:**
- Create: `shaders/pbr_forward_skinned.vert`、`shaders/shadow_depth_skinned.vert`
- Modify: `shaders/CMakeLists.txt`、`cmake/GenEmbedded.cmake`(名表 +2)
- Modify: `core/renderer/renderer.h`、`core/renderer/renderer.cpp`(skinned 管线 + JointUBO + submit 重载)
- Modify: `core/renderer/renderable.h`(RenderContext 加 jointUbo/jointOffset)、`core/renderer/mesh_renderable.cpp`(蒙皮路径)
- Modify: `core/resource/mesh_render_resource.h`(MeshGpuData.skinned)
- Modify: `core/rhi/backends/gles/gles_device.cpp`(块名表 JointUBO→3)
- Test: `tests/renderer/skinned_test.cpp`(新建)、`tests/CMakeLists.txt`(注册)
- Golden: 新增 `tests/golden/skinned_quad_metal.png`、`skinned_quad_vulkan.png`
- Modify(调用点): 7 处 RendererShaderDesc + rd_api.cpp(get 链)

- [ ] **Step 1: skinned shader**

`shaders/pbr_forward_skinned.vert`:

```glsl
// pbr_forward_skinned.vert:蒙皮版 PBR 顶点着色器。
// location 4=joints4f@48,5=weights4f@64(stride 80);JointUBO(b3)=joints[128]。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aUV;
layout(location = 4) in vec4 aJoints;
layout(location = 5) in vec4 aWeights;

layout(binding = 1) uniform ItemUBO {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;
  vec4 uvTransform;
};
layout(binding = 3) uniform JointUBO { mat4 joints[128]; } jubo;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec4 vTangent;
layout(location = 3) out vec2 vUV;

void main() {
  mat4 skin = aWeights.x * jubo.joints[int(aJoints.x + 0.5)] +
              aWeights.y * jubo.joints[int(aJoints.y + 0.5)] +
              aWeights.z * jubo.joints[int(aJoints.z + 0.5)] +
              aWeights.w * jubo.joints[int(aJoints.w + 0.5)];
  // 蒙皮在模型空间,再乘 world/normalMatrix
  vec4 p = skin * vec4(aPos, 1.0);
  mat4 m = skin;
  vWorldPos = (world * p).xyz;
  vNormal = (normalMatrix * vec4(mat3(m) * aNormal, 0.0)).xyz;
  vTangent = vec4((normalMatrix * vec4(mat3(m) * aTangent.xyz, 0.0)).xyz, aTangent.w);
  vUV = aUV * uvTransform.zw + uvTransform.xy;
  gl_Position = mvp * p;
}
```

注意:mvp 含 viewProj×world,故 gl_Position = mvp × (skin·pos)(skin 先于 world)。
法线精确做法应取 skin 逆转置,v1 用 mat3(m) 近似(关节含均匀旋转为主,差异可忽略)。

`shaders/shadow_depth_skinned.vert`:

```glsl
// shadow_depth_skinned.vert:蒙皮版深度写出(ShadowUBO=lightViewProj,ItemUBO=world,
// JointUBO(b3)=joints)。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 4) in vec4 aJoints;
layout(location = 5) in vec4 aWeights;
layout(binding = 0) uniform ShadowUBO { mat4 lightViewProj; } u;
layout(binding = 1) uniform ItemUBO {
  mat4 mvp; mat4 world; mat4 normalMatrix;
  vec4 baseColorFactor; vec4 emissiveOcclusion; vec4 metallicRoughness; vec4 uvTransform;
} item;
layout(binding = 3) uniform JointUBO { mat4 joints[128]; } jubo;
void main() {
  mat4 skin = aWeights.x * jubo.joints[int(aJoints.x + 0.5)] +
              aWeights.y * jubo.joints[int(aJoints.y + 0.5)] +
              aWeights.z * jubo.joints[int(aJoints.z + 0.5)] +
              aWeights.w * jubo.joints[int(aJoints.w + 0.5)];
  gl_Position = u.lightViewProj * item.world * skin * vec4(aPos, 1.0);
}
```

`shaders/CMakeLists.txt` 追加两个;`cmake/GenEmbedded.cmake` SHADERS 追加
`pbr_forward_skinned shadow_depth_skinned`。

- [ ] **Step 2: 写失败测试**

`tests/renderer/skinned_test.cpp`:

```cpp
// 蒙皮 golden:运行时生成 2 骨 quad,弯折中点(t=0.5)渲染,双后端 golden。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "common/skinned_gen.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/animator.h"
#include "scene/camera.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <cstdlib>
#include <filesystem>

namespace {
constexpr uint32_t kW = 256, kH = 256;

void runSkinnedGolden(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  ASSERT_NE(device, nullptr);
  const std::string dir =
      (std::filesystem::temp_directory_path() / "rd_skin_golden").string();
  const std::string path = rd::test::writeSkinnedQuad(dir);
  auto model = rd::loadGltf(path.c_str());
  ASSERT_TRUE(model.valid());

  auto load = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
  auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
  auto skVs = load("pbr_forward_skinned.vert");
  auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
  auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
  auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
  auto sdsVs = load("shadow_depth_skinned.vert");
  auto exFs = load("bloom_extract.frag");
  auto bbFs = load("bloom_blur.frag");
  auto cpFs = load("composite.frag");
  auto fxFs = load("fxaa.frag");
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   exFs.code,   bbFs.code,
                            cpFs.code,   fxFs.code,   unlitVs.entry,
                            rd::Format::RGBA8_UNORM};
  // skinned shader 字节经独立字段传入
  sd.skinnedVs = skVs.code;
  sd.skinnedShadowVs = sdsVs.code;
  ASSERT_TRUE(renderer.init(*device, sd));

  auto res = rd::MeshRenderResource::upload(*device, model);
  ASSERT_NE(res, nullptr);
  rd::scene::Animator anim;
  ASSERT_TRUE(anim.bind(model));
  anim.play(0);
  anim.update(0.5f);  // 弯折中点(确定性)

  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  ASSERT_TRUE(target.valid());
  rd::scene::Camera cam;
  cam.lookAt({0, 1.2f, 3}, {0, 1, 0}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);

  device->beginFrame();
  renderer.beginScene(cam, {0.05f, 0.05f, 0.06f, 1.0f});
  renderer.submit(res, math::Mat4(1.0f), anim.jointMatrices().data(),
                  uint32_t(anim.jointMatrices().size()));
  auto* cmd = device->acquireCommandBuffer();
  renderer.endScene(cmd, target);
  device->submit(cmd);
  device->waitIdle();
  device->endFrame();

  std::vector<uint8_t> px(size_t(kW) * kH * 4);
  ASSERT_TRUE(device->readbackTarget(target, px.data(), px.size()));
  res->destroy(*device);
  renderer.shutdown();
  const std::string name =
      b == rd::Backend::Metal ? "skinned_quad_metal.png" : "skinned_quad_vulkan.png";
  const std::string path = std::string(RD_TEST_DATA_DIR) + "/golden/" + name;
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(rd::test::savePNG(path, kW, kH, px.data()));
    return;
  }
  auto golden = rd::test::loadPNG(path);
  ASSERT_EQ(golden.pixels.size(), px.size()) << "golden 缺失: " << path;
  auto cmp = rd::test::compareRGBA8(px.data(), golden.pixels.data(), kW, kH, 3, 0.02);
  EXPECT_TRUE(cmp.pass) << "diffRatio=" << cmp.diffRatio;
}
} // namespace

TEST(Skinned, MetalGolden) {
#if defined(__APPLE__)
  runSkinnedGolden(rd::Backend::Metal);
#endif
}
TEST(Skinned, VulkanGolden) {
#if defined(RD_WITH_VULKAN)
  runSkinnedGolden(rd::Backend::Vulkan);
#endif
}
```

`tests/CMakeLists.txt` 追加 `renderer/skinned_test.cpp`。

- [ ] **Step 3: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`skinnedVs`/`submit` 重载未定义)

- [ ] **Step 4: RendererShaderDesc + Renderer**

`renderer.h`:
1. RendererShaderDesc 追加(fxaaFs 后):

```cpp
  std::vector<uint8_t> skinnedVs;        ///< pbr_forward_skinned.vert(蒙皮管线)
  std::vector<uint8_t> skinnedShadowVs;  ///< shadow_depth_skinned.vert(蒙皮阴影)
```

2. 公开方法:

```cpp
  /// 提交蒙皮渲染项(jointPalette 为 joints 数组,jointCount ≤128;帧内拷贝)。
  void submit(const std::shared_ptr<MeshRenderResource>& mesh, const math::Mat4& world,
              const math::Mat4* jointPalette, uint32_t jointCount);
```

3. 成员:`PipelineHandle skinnedPipeline_; PipelineHandle skinnedShadowPipeline_; BufferHandle jointUbo_;  // 64KB,8 项×8192B`
   `std::vector<int32_t> jointSlot_;  // 与 queue_ 平行:JointUBO 槽位(-1=非蒙皮)`;
   `static constexpr uint32_t kJointItemStride = 8192; static constexpr uint32_t kMaxJointItems = 8;`

`renderer.cpp`:
1. init:jointUbo_ 创建(64KB hostWrite);skinned 管线创建移入 ensureScenePipelines
   (与 pbr/unlit 同 key 重建;顶点布局 80B 六属性):

```cpp
  // ensureScenePipelines 内追加(pbr/unlit 重建之后):
  PipelineDesc skd;
  skd.vertexShader = skvs_;   // skinned 模块持有(init 创建)
  skd.fragmentShader = fs_;   // frag 复用 pbr
  skd.vertexBindings = {{0, 80}};
  skd.attributes = {{0, Format::R32G32B32_FLOAT, 0, 0},
                    {1, Format::R32G32B32_FLOAT, 12, 0},
                    {2, Format::R32G32B32A32_FLOAT, 24, 0},
                    {3, Format::R32G32_FLOAT, 40, 0},
                    {4, Format::R32G32B32A32_FLOAT, 48, 0},
                    {5, Format::R32G32B32A32_FLOAT, 64, 0}};
  skd.cullMode = CullMode::None;
  skd.depthTest = true;
  skd.depthWrite = true;
  skd.colorFormat = fmt;
  skd.sampleCount = samples;
  if (skinnedPipeline_.valid()) dev_->destroyPipeline(skinnedPipeline_);
  skinnedPipeline_ = dev_->createPipeline(skd);
  // 蒙皮阴影管线(depthOnly 80B):
  PipelineDesc ssd = skd;
  ssd.vertexShader = sdsvs_;
  ssd.fragmentShader = sfs_;  // shadow_depth.frag
  ssd.depthOnly = true;
  if (skinnedShadowPipeline_.valid()) dev_->destroyPipeline(skinnedShadowPipeline_);
  skinnedShadowPipeline_ = dev_->createPipeline(ssd);
```

   (init:skinned 模块创建 skvs_/sdsvs_ 持有;首次创建在 ensureScenePipelines)
2. submit 重载:

```cpp
void Renderer::submit(const std::shared_ptr<MeshRenderResource>& mesh,
                      const math::Mat4& world, const math::Mat4* jointPalette,
                      uint32_t jointCount) {
  if (queue_.size() >= kMaxItems || jointSlot_.size() >= kMaxJointItems) {
    RD_LOGW("renderer", "蒙皮项超出上限,截断");
    return;
  }
  const int32_t slot = int32_t(jointSlot_.size());
  const uint32_t n = std::min(jointCount, 128u);
  dev_->updateBuffer(jointUbo_, jointPalette, uint64_t(n) * 64,
                     uint64_t(slot) * kJointItemStride);
  queue_.push_back(std::make_unique<MeshRenderable>(mesh));
  worldStack_.push_back(world);
  jointSlot_.push_back(slot);
}
```

   **jointSlot_ 对齐与槽位约定(以此为准)**:jointSlot_ 与 queue_ 平行
   (非蒙皮 submit 末尾 `jointSlot_.push_back(-1)`);蒙皮项的 JointUBO 槽 =
   `queue_ 当前下标 i`(jointSlot_.push_back(i));上传偏移与绑定偏移同为
   `i * kJointItemStride`;蒙皮项数超 kMaxJointItems(8) 记警告并截断
   (先按 jointSlot 计数检查再 push)。endScene 里
   `ctx.jointOffset = jointSlot_[i] >= 0 ? uint64_t(jointSlot_[i]) * kJointItemStride : 0`。
3. MeshGpuData.skinned:mesh_render_resource.h 加 `bool skinned = false;`;,upload 时
   从 MeshData.skinned 拷贝。
4. RenderContext 加:`BufferHandle jointUbo; uint64_t jointOffset = 0;`
5. MeshRenderable::record:蒙皮分支(mesh.skinned && !shadowPass):

```cpp
    if (g.skinned) {
      cmd->bindPipeline(ctx.skinnedPipeline);  // RenderContext 加此字段
      cmd->bindUniformBuffer(0, ctx.frameUbo, 0, 256);
      cmd->bindUniformBuffer(1, ctx.itemUbo, ctx.itemOffset, 256);
      cmd->bindUniformBuffer(2, ctx.lightUbo, 0, 352);
      cmd->bindUniformBuffer(3, ctx.jointUbo, ctx.jointOffset, 8192);
      // 纹理绑定同 pbr 路径
      ...
    }
```

   shadowPass 蒙皮分支:绑 skinnedShadowPipeline_ + slot3 joints。
   (RenderContext 追加 `PipelineHandle skinnedPipeline, skinnedShadowPipe;`)
6. endScene:per-item jointOffset 计算:`jointSlot_[i] >= 0 ? jointSlot_[i]*8192 : 0`
   写入 ctx.jointOffset;蒙皮项在 shadowPass 同路径(skinnedShadowPipe)。
7. endScene 帧末:`jointSlot_.clear()`(与 queue_/worldStack_ 同步)。

- [ ] **Step 5: GLES 块名表**

`gles_device.cpp` 名表追加 `{"JointUBO", 3}`。

- [ ] **Step 6: 调用点适配(7 处 + rd_api)**

7 处 RendererShaderDesc 聚合追加 `skVs.code, sdsVs.code,`(fxFs 后、entry 前);
`rd_api.cpp` get 链追加 `get("pbr_forward_skinned", Vertex, sd.skinnedVs)` 与
`get("shadow_depth_skinned", Vertex, sd.skinnedShadowVs)`。

- [ ] **Step 7: 跑测试 + 生成 golden**

Run: `./scripts/check.sh 2>&1 | tail -6`
Expected: 编译通过;Skinned.*Golden 报 golden 缺失;**既有 golden 不回归**
(非蒙皮路径 48B 不变)
Run: `RD_UPDATE_GOLDENS=1 ctest --test-dir build --output-on-failure -R "Skinned.*Golden"`
**像素核对** `tests/golden/skinned_quad_*.png`:quad 上半弯折约 45°、双后端一致。
再 `./scripts/check.sh 2>&1 | tail -4` 全绿。

- [ ] **Step 8: Commit**

```bash
git add core shaders cmake tests tools/render_test
git commit -m "feat(renderer): 蒙皮管线(pbr_forward_skinned)+ JointUBO 调色板 + 弯折 quad golden"
```

---

### Task 4: C API 动画 + engine 集成

**Files:**
- Modify: `core/api/rd_api.h`、`core/api/rd_api.cpp`
- Test: `tests/api/api_test.cpp`(追加)

- [ ] **Step 1: 写失败测试**

`tests/api/api_test.cpp` 追加:

```cpp
// 动画 API:无动画模型安全;空引擎不崩
TEST(Api, AnimationSafe) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  // 无模型/无动画:安全 no-op
  rd_engine_play_animation(e, 0);
  rd_engine_crossfade_animation(e, 0, 0.2f);
  rd_engine_pause_animation(e, 1);
  rd_engine_pause_animation(e, 0);
  EXPECT_EQ(rd_engine_load_gltf(e, RD_TEST_DATA_DIR "/assets/TetraU32.glb"), RD_OK);
  rd_engine_play_animation(e, 0);  // 模型无动画:警告 no-op
  rd_engine_render_frame(e, 0.016f);
  rd_engine_destroy(e);
  rd_engine_play_animation(nullptr, 0);  // 不崩
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(API 未声明)

- [ ] **Step 3: rd_api.h**

```c
/// 播放动画 clip(立即切换,loop)。越界/无动画记警告 no-op。
void rd_engine_play_animation(rd_engine* engine, int32_t clip_index);
/// 交叉淡入到目标 clip(fade_seconds 秒过渡)。
void rd_engine_crossfade_animation(rd_engine* engine, int32_t clip_index,
                                   float fade_seconds);
/// 暂停/继续动画。
void rd_engine_pause_animation(rd_engine* engine, int32_t paused);
```

- [ ] **Step 4: rd_api.cpp 集成**

1. 结构体追加:

```cpp
#include "scene/animator.h"
  rd::scene::Animator animator;
  bool hasAnimation = false;
```

2. load_gltf 成功路径:

```cpp
  e->hasAnimation = !model.animations.empty() && !model.skins.empty();
  if (e->hasAnimation) {
    e->animator.bind(model);
    e->animator.play(0);  // 自动播放 clip 0
  }
```

   注意:bind 持有 model 指针——ModelAsset 是局部变量!**engine 须持久持有
   ModelAsset 副本**:`e->modelAsset = std::move(model);`(结构体加成员),
   `animator.bind(e->modelAsset)`。
3. render_frame:动画驱动 + 蒙皮提交:

```cpp
  if (e->hasAnimation && e->model) {
    e->animator.update(dt);
    e->renderer.beginScene(e->camera, {0.05f, 0.05f, 0.06f, 1.0f});
    e->renderer.submit(e->model, math::Mat4(1.0f), e->animator.jointMatrices().data(),
                       uint32_t(e->animator.jointMatrices().size()));
    auto* cmd = e->device->acquireCommandBuffer();
    e->renderer.endScene(cmd, target);
    ...
    return;  // 蒙皮路径不走 scene->collect
  }
  // 现有静态路径
```

4. API 实现(空引擎/无动画安全,play 越界由 Animator 记警告):

```cpp
void rd_engine_play_animation(rd_engine* e, int32_t clip) {
  if (!e || !e->hasAnimation || clip < 0) return;
  e->animator.play(uint32_t(clip));
}
void rd_engine_crossfade_animation(rd_engine* e, int32_t clip, float fade) {
  if (!e || !e->hasAnimation || clip < 0) return;
  e->animator.playWithFade(uint32_t(clip), fade);
}
void rd_engine_pause_animation(rd_engine* e, int32_t paused) {
  if (!e || !e->hasAnimation) return;
  e->animator.pause(paused != 0);
}
```

- [ ] **Step 5: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 全绿(Api.AnimationSafe 通过)

- [ ] **Step 6: Commit**

```bash
git add core/api tests/api
git commit -m "feat(api): 动画 C API(play/crossfade/pause)+ engine Animator 集成(自动播放 clip0)"
```

---

### Task 5: AGENTS.md 收尾 + 全量回归

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: AGENTS.md 更新**

- 「当前状态」追加:

```markdown
- P2-3 完成:骨骼动画(节点层级/skins/animations 解析 + GPU 蒙皮
  pbr_forward_skinned + JointUBO slot3 调色板 + Animator 播放/交叉淡入
  + C API play/crossfade/pause,load_gltf 自动播放 clip0)
- 下一步:P2 余下(拾取/性能基准)
```

- 「代码约定」追加:

```markdown
- 蒙皮:顶点布局 80B(48B + joints4f@48|weights4f@64,location 4/5);
  JointUBO=slot3(64KB 共享,8 项×8192B 步进,超 128 骨截断告警);
  jointMatrices[j] = nodeGlobals[joints[j]] × IBM[j];
  蒙皮阴影用 shadow_depth_skinned;法线蒙皮用 mat3(skin) 近似
- Animator:clip 线性插值(rotation slerp),STEP 退化保持;节点父先子后序依赖
  (反序模型已知限制)
```

- [ ] **Step 2: 全量回归**

Run: `./scripts/check.sh 2>&1 | tail -4`(全绿)
Run: Android assembleDebug / iOS build 均成功
Run: `RD_INTERACTIVE_FRAMES=30 ./build/tools/render_test/render_test --interactive --model tests/assets/DamagedHelmet.glb`(exit 0)

- [ ] **Step 3: Commit**

```bash
git add AGENTS.md
git commit -m "docs: P2-3 收尾(AGENTS.md 新约定)"
```

---

## 附:已知风险与决策记录

| 风险/决策 | 处理 |
|---|---|
| 关节 >128 | 截断+警告(拆 drawcall 归后续) |
| 蒙皮项/帧上限 8 | 64KB 共享 JointUBO;超出告警截断(数字人单模型足够) |
| 节点反序(子先于父) | computeGlobals 依赖下标序;glTF 导出器一般父先子后,反序模型记录为已知限制(P4 拓扑排序) |
| 蒙皮法线近似 | mat3(skin) 而非逆转置;关节旋转为主差异可忽略 |
| 蒙皮+unlit 组合 | skinned 恒走 PBR 路径(罕见组合不支持,记警告) |
| morph target | 不支持(weights 通道解析时已跳过) |
| glTF matrix 形式节点 | v1 取平移+警告(TRS 形式为绝对主流) |
