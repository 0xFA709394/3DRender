# 场景示例集合 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/superpowers/specs/2026-08-18-demo-scenes-design.md` 实现:①primitives 几何生成器;②scenes 模块 + render_test --scene(5 程序场景)+ golden/冒烟;③fetch_assets.sh + sponza/cesium_man;④interactive --scene + 移动 demo 切换。

**Architecture:** primitives 进内核 resource 层(球/平面/盒,48B MeshData);scenes 模块在 tools/render_test(直接驱动 Renderer,不经 engine);golden 测试经共享 scenes.cpp 编译进 rd_tests;知名 glb 经 curl 下载脚本,缺失自动 skip。

**通用约定:**
- 构建+测试:`export RD_DEPS_MIRROR=$HOME/.rd_deps_mirror && ./scripts/check.sh`;单跑 `ctest --test-dir build --output-on-failure -R <正则>`
- golden 更新:`RD_UPDATE_GOLDENS=1 ctest --test-dir build -R <用例>` 后**像素核对**再提交
- 提交规范:每 Task 一个 commit,`<type>(<scope>): 描述`
- 顶点布局 48B 四属性(非蒙皮);资源创建须在 acquireCommandBuffer 之前
- RendererShaderDesc 16 字段聚合顺序:unlitVs,unlitFs,pbrVs,pbrFs,prefilterVs,prefilterFs,blitVs,blitFs,shadowVs,shadowFs,extractFs,blurFs,compositeFs,fxaaFs,skinnedVs,skinnedShadowVs,entry,colorFormat

---

### Task 1: primitives 几何生成器 + 单测

**Files:**
- Create: `core/resource/primitives.h`、`core/resource/primitives.cpp`
- Modify: `core/CMakeLists.txt`
- Test: `tests/resource/primitives_test.cpp`(新建)、`tests/CMakeLists.txt`(注册)

- [ ] **Step 1: 写失败测试**

`tests/resource/primitives_test.cpp`:

```cpp
// primitives 单测:球/平面/盒的拓扑、法线、切线与尺寸。
#include <gtest/gtest.h>
#include "resource/primitives.h"
#include <cmath>

TEST(Primitives, Sphere) {
  auto s = rd::primitives::makeSphere(0.5f, 16, 8);
  // 顶点数:(rings+1)×(segments+1);索引:rings×segments×2 三角形
  EXPECT_EQ(s.vertices.size(), size_t(9 * 17) * 12u);
  EXPECT_EQ(s.indexCount, 8u * 16u * 2u * 3u);
  EXPECT_FALSE(s.skinned);
  // 法线单位长度且外向(球心→顶点 与法线同向)
  const float* v0 = s.vertices.data();  // 顶点 0(极点)
  const float r = std::sqrt(v0[0] * v0[0] + v0[1] * v0[1] + v0[2] * v0[2]);
  EXPECT_NEAR(r, 0.5f, 1e-4f);
  const float nl = std::sqrt(v0[3] * v0[3] + v0[4] * v0[4] + v0[5] * v0[5]);
  EXPECT_NEAR(nl, 1.0f, 1e-3f);
}

TEST(Primitives, Plane) {
  auto p = rd::primitives::makePlane(2.0f, 1.0f);
  EXPECT_EQ(p.vertices.size(), 4u * 12u);
  EXPECT_EQ(p.indexCount, 6u);
  // 法线全 +Y
  for (int i = 0; i < 4; ++i) {
    EXPECT_FLOAT_EQ(p.vertices[size_t(i) * 12 + 3], 0.0f);
    EXPECT_FLOAT_EQ(p.vertices[size_t(i) * 12 + 4], 1.0f);
    EXPECT_FLOAT_EQ(p.vertices[size_t(i) * 12 + 5], 0.0f);
  }
}

TEST(Primitives, Box) {
  auto b = rd::primitives::makeBox(1, 2, 3);
  EXPECT_EQ(b.vertices.size(), 24u * 12u);  // 6 面 × 4 顶点
  EXPECT_EQ(b.indexCount, 36u);             // 12 三角形
  // 尺寸:顶点范围 ±0.5/±1/±1.5
  float maxY = 0;
  for (size_t i = 0; i < 24; ++i)
    maxY = std::max(maxY, std::abs(b.vertices[i * 12 + 1]));
  EXPECT_FLOAT_EQ(maxY, 1.0f);
}
```

`tests/CMakeLists.txt` 追加 `resource/primitives_test.cpp`。

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`resource/primitives.h` 不存在)

- [ ] **Step 3: 实现 primitives**

`core/resource/primitives.h`:

```cpp
/**
 * @file primitives.h
 * @brief 程序化几何生成器(球/平面/盒):输出 48B 交错 MeshData(含切线)。
 * demo 场景与测试共用;内核级复用(勿手写零散几何)。
 */
#pragma once
#include "resource/gltf_loader.h"

namespace rd::primitives {

/// UV 球:radius 半径,segments 经向分段(≥8),rings 纬向分段(≥4)。
MeshData makeSphere(float radius, uint32_t segments, uint32_t rings);
/// 平面:y 朝上,中心原点,边长 size;uv 平铺 repeat 次。
MeshData makePlane(float size, float repeat = 1.0f);
/// 盒:中心原点,三边长;逐面法线。
MeshData makeBox(float sx, float sy, float sz);

} // namespace rd::primitives
```

`core/resource/primitives.cpp`:

```cpp
#include "resource/primitives.h"
#include "resource/mesh_utils.h"
#include <cmath>

namespace rd::primitives {

MeshData makeSphere(float radius, uint32_t segments, uint32_t rings) {
  MeshData m;
  m.name = "sphere";
  const uint32_t cols = segments + 1, rows = rings + 1;
  m.vertices.resize(size_t(cols) * rows * 12, 0.0f);
  for (uint32_t y = 0; y < rows; ++y) {
    const float v = float(y) / float(rings);
    const float phi = v * 3.14159265f;  // 0..π(顶到底)
    for (uint32_t x = 0; x < cols; ++x) {
      const float u = float(x) / float(segments);
      const float theta = u * 6.2831853f;
      const float sx = std::sin(phi) * std::cos(theta);
      const float sy = std::cos(phi);
      const float sz = std::sin(phi) * std::sin(theta);
      float* d = m.vertices.data() + size_t(y * cols + x) * 12;
      d[0] = sx * radius;
      d[1] = sy * radius;
      d[2] = sz * radius;
      d[3] = sx;
      d[4] = sy;
      d[5] = sz;  // 法线=单位方向
      d[10] = u;
      d[11] = v;
    }
  }
  // 索引(绕开极点退化:三角形全部合法,极点重顶点无妨)
  m.indices.resize(size_t(rings) * segments * 6 * 2);
  auto* idx = reinterpret_cast<uint16_t*>(m.indices.data());
  uint32_t w = 0;
  for (uint32_t y = 0; y < rings; ++y)
    for (uint32_t x = 0; x < segments; ++x) {
      const uint16_t a = uint16_t(y * cols + x);
      const uint16_t b = uint16_t(y * cols + x + 1);
      const uint16_t c = uint16_t((y + 1) * cols + x);
      const uint16_t dd = uint16_t((y + 1) * cols + x + 1);
      idx[w++] = a; idx[w++] = c; idx[w++] = b;
      idx[w++] = b; idx[w++] = c; idx[w++] = dd;
    }
  m.indexType = IndexType::UInt16;
  m.indexCount = w;
  computeTangents(m.vertices.data(), uint32_t(cols * rows), m.indices.data(), w,
                  IndexType::UInt16, 12);
  return m;
}

MeshData makePlane(float size, float repeat) {
  MeshData m;
  m.name = "plane";
  const float h = size * 0.5f;
  const float v[4][12] = {
      {-h, 0, -h, 0, 1, 0, 1, 0, 0, 1, 0, 0},
      { h, 0, -h, 0, 1, 0, 1, 0, 0, 1, repeat, 0},
      { h, 0,  h, 0, 1, 0, 1, 0, 0, 1, repeat, repeat},
      {-h, 0,  h, 0, 1, 0, 1, 0, 0, 1, 0, repeat},
  };
  m.vertices.assign(&v[0][0], &v[0][0] + 48);
  const uint16_t idx[6] = {0, 2, 1, 0, 3, 2};
  m.indices.resize(12);
  memcpy(m.indices.data(), idx, 12);
  m.indexType = IndexType::UInt16;
  m.indexCount = 6;
  return m;
}

MeshData makeBox(float sx, float sy, float sz) {
  MeshData m;
  m.name = "box";
  const float x = sx * 0.5f, y = sy * 0.5f, z = sz * 0.5f;
  // 6 面 × 4 顶点:法线逐面;uv 简单 0..1
  const float n[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
  const float corners[8][3] = {{-x,-y,-z},{x,-y,-z},{x,y,-z},{-x,y,-z},
                               {-x,-y,z},{x,-y,z},{x,y,z},{-x,y,z}};
  const int faceV[6][4] = {{1,5,6,2},{4,0,3,7},{3,2,6,7},{4,5,1,0},
                           {5,4,7,6},{0,1,2,3}};
  m.vertices.resize(24 * 12, 0.0f);
  for (int f = 0; f < 6; ++f)
    for (int k = 0; k < 4; ++k) {
      float* d = m.vertices.data() + size_t(f * 4 + k) * 12;
      const float* p = corners[faceV[f][k]];
      d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
      d[3] = n[f][0]; d[4] = n[f][1]; d[5] = n[f][2];
      d[10] = (k == 1 || k == 2) ? 1.0f : 0.0f;
      d[11] = (k >= 2) ? 1.0f : 0.0f;
    }
  m.indices.resize(36 * 2);
  auto* idx = reinterpret_cast<uint16_t*>(m.indices.data());
  for (uint16_t f = 0; f < 6; ++f) {
    const uint16_t b = f * 4;
    idx[f * 6 + 0] = b; idx[f * 6 + 1] = uint16_t(b + 1); idx[f * 6 + 2] = uint16_t(b + 2);
    idx[f * 6 + 3] = b; idx[f * 6 + 4] = uint16_t(b + 2); idx[f * 6 + 5] = uint16_t(b + 3);
  }
  m.indexType = IndexType::UInt16;
  m.indexCount = 36;
  computeTangents(m.vertices.data(), 24, m.indices.data(), 36, IndexType::UInt16, 12);
  return m;
}

} // namespace rd::primitives
```

`core/CMakeLists.txt` 追加 `resource/primitives.cpp`。

- [ ] **Step 4: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 全绿(Primitives.* 通过)

- [ ] **Step 5: Commit**

```bash
git add core/resource/primitives.* core/CMakeLists.txt tests/resource/primitives_test.cpp tests/CMakeLists.txt
git commit -m "feat(resource): primitives 几何生成器(球/平面/盒,48B)+ 单测"
```

---

### Task 2: scenes 模块 + render_test --scene + golden/冒烟

**Files:**
- Create: `tools/render_test/scenes.h`、`tools/render_test/scenes.cpp`
- Modify: `tools/render_test/main.cpp`(--scene 分支)、`tools/render_test/CMakeLists.txt`
- Test: `tests/renderer/demo_scenes_test.cpp`(新建)、`tests/CMakeLists.txt`(注册 + scenes.cpp 编译进 rd_tests)
- Golden: 新增 `demo_material_metal/vulkan.png`、`demo_cornell_metal/vulkan.png`、`demo_lights_metal/vulkan.png`

- [ ] **Step 1: scenes 模块**

`tools/render_test/scenes.h`:

```cpp
// demo 场景构建与渲染(render_test/测试共用;直接驱动 Renderer,不经 engine)。
#pragma once
#include "renderer/renderer.h"
#include "resource/mesh_render_resource.h"
#include "scene/animator.h"
#include "scene/camera.h"
#include <memory>
#include <string>
#include <vector>

namespace rd::tool {

/// demo 场景上下文(资源持有)。
struct DemoScene {
  std::vector<std::shared_ptr<MeshRenderResource>> resources;
  std::vector<math::Mat4> worlds;              // 与 resources 平行(蒙皮项除外)
  scene::Camera camera;
  std::vector<LightData> lights;
  scene::Animator animator;
  bool animated = false;
  const ModelAsset* animModel = nullptr;       // animator 绑定源(调用方持有)
  float framingCenter[3] = {0, 0, 0};
  float framingRadius = 1.0f;
};

/// 按名构建场景(5 程序场景 + sponza/cesium_man 知名 glb;资产缺失返回 false 并记日志)。
/// modelAssets 为调用方持有的 ModelAsset 存储(动画/取景生命周期)。
bool buildDemoScene(const char* name, Device& dev, Renderer& renderer, DemoScene& out,
                    ModelAsset& modelStorage);

/// 场景名表(轮转/帮助用)。
const char* const* demoSceneNames(uint32_t& count);

/// 提交场景到渲染队列(动画项带关节调色板)。
void submitDemoScene(DemoScene& s, Renderer& renderer, float dt);

/// 释放全部 GPU 资源。
void destroyDemoScene(DemoScene& s, Device& dev);

} // namespace rd::tool
```

`tools/render_test/scenes.cpp`(实现要点——完整代码):

1. **material_balls**:5×5 球(primitives::makeSphere(0.4, 32, 16));mesh 级材质:
   ball(i,j) metallic=i/4, roughness=j/4;间距 1.0 居中;相机 (0,2.5,7) 看原点;
   无显式灯(默认灯)+ 无阴影(legacy 档)。
   注意:每球一个 ModelAsset(材质不同)→ 25 资源;submit 逐个 submit(res, world)。
2. **cornell_box**:5 墙(makeBox 薄板)+ 2 内盒(一高一矮旋转 15°);墙白/左红/右绿
   (baseColorFactor);方向光 (0.3,1,0.4) color 4.0;setQuality(High 取阴影)
   ——golden 走 High?为隔离 post 变量:**Mid 档**(post=1?——统一用 High,
   golden 即 High 输出)。setLightFraming(场景包围球手工: center(0,1,0), radius 2)。
3. **light_playground**:平面 + 3 球;点光×2(红左/蓝右,range 8)+ 聚光×1(顶下);
   Mid 档。
4. **skinned_demo**:`rd::test::writeSkinnedQuad`(temp 目录)+ loadGltf 入
   modelStorage;animator bind/play(0);submit 走关节重载。Mid 档。
5. **instanced_field**:256 实例(球 mesh 1 份 + instance-rate 顶点缓冲
   (16×float 变换... 简化:每实例 3 float 偏移,shader 内组装)。
   **v1 简化决策**:instancing 演示经 N submit(与 perf items_64 同构)——
   **不做自定义 Renderable**(YAGNI;RHI instancing 已有契约测试覆盖)。
   场景内容:16×16 球阵,正弦相位动画(dt 驱动 world 更新)。
6. **sponza / cesium_man**:`assets/Sponza.glb` / `assets/CesiumMan.glb`;
   不存在 → RD_LOGW + return false。包围球取景 + Mid 档;cesium_man 自动播 clip0。

`demoSceneNames` 返回 7 名表。`submitDemoScene`:动画项
`animator.update(dt)` + submit(palette);其余逐资源 submit(worlds[i]);
instanced_field 的 worlds 每帧正弦更新(y = sin(t + i) 微幅)。

- [ ] **Step 2: render_test --scene 分支**

`main.cpp`:`--scene <name>` 参数;分支在 --model 之前(scene 优先):

```cpp
  if (!sceneName.empty()) {
    // 初始化设备/渲染器/离屏目标(同 --model 模式);buildDemoScene 构建;
    // beginFrame/beginScene/submitDemoScene/endScene/submit/waitIdle/endFrame;
    // readback → savePNG(out);destroyDemoScene
  }
```

interactive 分支:`--interactive --scene <name>` 传 scene 名(interactive.mm Task 4)。

`tools/render_test/CMakeLists.txt`:render_test 源列表加 `scenes.cpp` +
`${CMAKE_SOURCE_DIR}/tests/common/skinned_gen.cpp`(链接 ktx 已有?render_test
链接 rd_core 即可,skinned_gen 需 ktx include——加
`${ktx_SOURCE_DIR}/include`)。

- [ ] **Step 3: golden + 冒烟测试**

`tests/renderer/demo_scenes_test.cpp`:

```cpp
// demo 场景 golden(material_balls/cornell_box/light_playground 双后端)
// + 全程序场景冒烟(渲染不崩 + 非背景覆盖率 >3%)。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "tools/render_test/scenes.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <cstdlib>

namespace {
constexpr uint32_t kW = 512, kH = 512;

// 渲染指定场景 → RGBA8 图像(失败返回空)。
rd::test::Image renderScene(rd::Backend b, const char* name, rd::Renderer& renderer,
                            rd::ModelAsset& storage) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto load = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  // ... 16 字段 RendererShaderDesc 聚合(与 pbr_test 同模式) ...
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  if (!target.valid() || !renderer.init(*device, sd)) return {};
  rd::tool::DemoScene scene;
  if (!rd::tool::buildDemoScene(name, *device, renderer, scene, storage)) return {};
  device->beginFrame();
  renderer.beginScene(scene.camera, {0.05f, 0.05f, 0.06f, 1.0f});
  rd::tool::submitDemoScene(scene, renderer, 1.0f / 60.0f);
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
  rd::tool::destroyDemoScene(scene, *device);
  renderer.shutdown();
  return img;
}

void runGolden(rd::Backend b, const char* scene, const char* goldenName) {
  rd::ModelAsset storage;
  rd::Renderer renderer;  // 注意:renderer 生命周期在 renderScene 内,storage 须长
  auto img = renderScene(b, scene, renderer, storage);
  ASSERT_FALSE(img.pixels.empty());
  const std::string path = std::string(RD_TEST_DATA_DIR) + "/golden/" + goldenName;
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(rd::test::savePNG(path, img.width, img.height, img.pixels.data()));
    return;
  }
  auto golden = rd::test::loadPNG(path);
  ASSERT_EQ(golden.pixels.size(), img.pixels.size()) << "golden 缺失: " << path;
  auto cmp = rd::test::compareRGBA8(img.pixels.data(), golden.pixels.data(), kW, kH, 3, 0.02);
  EXPECT_TRUE(cmp.pass) << "diffRatio=" << cmp.diffRatio;
}

// 冒烟:非背景像素占比(采样 16 步进)
double coverage(const rd::test::Image& img) {
  const uint8_t bg[3] = {13, 13, 15};
  uint32_t hit = 0, total = 0;
  for (uint32_t y = 0; y < img.height; y += 16)
    for (uint32_t x = 0; x < img.width; x += 16) {
      const uint8_t* p = img.pixels.data() + (size_t(y) * img.width + x) * 4;
      if (abs(p[0] - bg[0]) + abs(p[1] - bg[1]) + abs(p[2] - bg[2]) > 12) ++hit;
      ++total;
    }
  return double(hit) / double(total);
}
} // namespace

TEST(DemoScenes, MaterialBallsMetal) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal, "material_balls", "demo_material_metal.png");
#endif
}
TEST(DemoScenes, MaterialBallsVulkan) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan, "material_balls", "demo_material_vulkan.png");
#endif
}
TEST(DemoScenes, CornellMetal) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal, "cornell_box", "demo_cornell_metal.png");
#endif
}
TEST(DemoScenes, CornellVulkan) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan, "cornell_box", "demo_cornell_vulkan.png");
#endif
}
TEST(DemoScenes, LightsMetal) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal, "light_playground", "demo_lights_metal.png");
#endif
}
TEST(DemoScenes, LightsVulkan) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan, "light_playground", "demo_lights_vulkan.png");
#endif
}

// 全程序场景冒烟(含 skinned_demo/instanced_field;知名 glb 缺失自动 skip)
TEST(DemoScenes, SmokeAll) {
#if defined(__APPLE__)
  uint32_t count = 0;
  const char* const* names = rd::tool::demoSceneNames(count);
  for (uint32_t i = 0; i < count; ++i) {
    rd::ModelAsset storage;
    rd::Renderer renderer;
    auto img = renderScene(rd::Backend::Metal, names[i], renderer, storage);
    if (img.pixels.empty()) continue;  // 资产缺失 skip
    EXPECT_GT(coverage(img), 0.03) << names[i] << " 覆盖率不足";
  }
#endif
}
```

`tests/CMakeLists.txt`:rd_tests 源列表追加 `renderer/demo_scenes_test.cpp` 与
`../tools/render_test/scenes.cpp`(共享编译;注意 include 根已有项目根)。

- [ ] **Step 4: 跑测试 + 生成 golden**

Run: `./scripts/check.sh 2>&1 | tail -6`(golden 缺失报错)
Run: `RD_UPDATE_GOLDENS=1 ctest --test-dir build --output-on-failure -R "DemoScenes"`
**像素核对** 6 张新 golden:material 球阵渐变清晰、cornell 阴影/彩墙、lights 多光源着色;
双后端 direct≈0。再 `./scripts/check.sh 2>&1 | tail -4` 全绿。

- [ ] **Step 5: Commit**

```bash
git add tools/render_test tests/renderer/demo_scenes_test.cpp tests/CMakeLists.txt tests/golden/demo_*.png
git commit -m "feat(tools): demo 场景模块(5 程序场景)+ render_test --scene + golden/冒烟"
```

---

### Task 3: fetch_assets.sh + sponza/cesium_man 场景

**Files:**
- Create: `scripts/fetch_assets.sh`
- Modify: `tools/render_test/scenes.cpp`(知名场景接入)

- [ ] **Step 1: 下载脚本**

`scripts/fetch_assets.sh`:

```bash
#!/bin/bash
# 知名示例资产下载(Khronos glTF-Sample-Assets;失败不阻塞,可重跑)。
set -u
cd "$(dirname "$0")/.."
mkdir -p assets
BASE=https://github.com/KhronosGroup/glTF-Sample-Assets/raw/main/Models
dl() {  # dl <本地名> <远端路径>
  if [ -f "assets/$1" ]; then echo "已存在 assets/$1"; return 0; fi
  echo "下载 $1 ..."
  curl -fL --retry 2 --connect-timeout 15 -o "assets/$1" "$BASE/$2" \
    && echo "OK $1" || { echo "失败 $1(弱网可重跑)"; return 1; }
}
dl CesiumMan.glb CesiumMan/glTF-Binary/CesiumMan.glb
dl Sponza.glb Sponza/glTF-Binary/Sponza.glb
echo "完成(缺失项可稍后重跑本脚本)"
```

`chmod +x scripts/fetch_assets.sh`。

- [ ] **Step 2: 下载 + 验证加载**

Run: `./scripts/fetch_assets.sh`
Expected: 两个 glb 下载完成(弱网失败则记录,场景自动 skip)
Run: `./build/tools/render_test/render_test --scene cesium_man --out /tmp/cm.png`
Expected: 渲染成功(蒙皮动画模型);`img_check /tmp/cm.png --min-coverage 0.03` PASS
Run(若 Sponza 下载成功):`--scene sponza --out /tmp/sp.png` + img_check PASS

- [ ] **Step 3: Commit**

```bash
git add scripts/fetch_assets.sh tools/render_test/scenes.cpp assets/
git commit -m "feat(tools): fetch_assets.sh + sponza/cesium_man 知名场景接入"
```

(assets/ 大二进制入库:Sponza ~25MB 视下载结果;若用户介意体积,仅提交 CesiumMan
并把 Sponza 列入 .gitignore 例外——**执行时按下载结果与用户偏好处理**)

---

### Task 4: interactive --scene + 移动 demo 切换

**Files:**
- Modify: `tools/render_test/interactive.mm`(--scene 直驱 Renderer + OrbitController)
- Modify: `tools/render_test/interactive.h`、`tools/render_test/main.cpp`(传参)
- Modify: `samples/ios/RdDemo/AppDelegate.swift`(画质档+模型切换按钮)
- Modify: `samples/android/app/src/main/java/com/rd/sample/MainActivity.kt`(同)

**移动 demo 范围说明(与 spec 的偏差,执行确认)**:程序场景(material_balls 等)
是 tools 层(Renderer 直驱),移动端 engine 只经 C API——为保分层,移动 demo
切换**画质档(High→Mid→Low)+ 模型(helmet ↔ cesium_man 若已下载)**;
程序场景为 host 工具侧专属。spec §1「移动 demo 场景切换按钮」按此落地。

- [ ] **Step 1: interactive --scene**

`interactive.h`:`runInteractive(const char* modelPath, const char* sceneName)`。
`interactive.mm`:sceneName 非空时走**直驱路径**:

```cpp
// 直驱:Renderer + DemoScene + OrbitController(GLFW 回调喂 C++ 控制器,不经 C API)
// - 设备/渲染器/SceneTarget 由 Renderer 管;swapchain 用 rd_device C++ createSwapChain
//   ——CAMetalLayer 指针与 engine 路径同款
// - 回调:onPointerDown/Move/Up/Scroll/DoubleTap → orbit 直接调用
// - 帧循环:orbit.update(dt) → applyTo(cam) → beginScene → submitDemoScene → endScene
```

(`rd_engine` 路径保持 --model/--空 不变)
`main.cpp`:`--scene` 名传给 runInteractive。

- [ ] **Step 2: iOS demo 切换按钮**

`AppDelegate.swift`:右下角 UIButton「切换」;循环状态机:
`High+helmet → Mid+helmet → Low+helmet → High+cesium_man(存在时)`;
按钮回调经 RenderView 调 `rd_engine_set_quality` / 重新 `loadModel`。

- [ ] **Step 3: Android demo 切换按钮**

`MainActivity.kt`:FrameLayout 右下角 Button「切换」;同状态机(RenderView 加
`setQuality(tier)` 公开方法包装 nativeSetQuality——JNI 需补
`nativeSetQuality`:`rd_engine_set_quality` 桥接)。

`platform/android/jni/rd_jni.cpp` 追加:

```cpp
JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeSetQuality(JNIEnv*, jobject,
                                                                        jlong ptr, jint q) {
  rd_engine_set_quality(reinterpret_cast<rd_engine*>(ptr),
                        static_cast<rd_quality_t>(q));
}
```

(RenderView.kt 加 external 声明 + setQuality 公开方法)

- [ ] **Step 4: 验证**

Run: `./scripts/check.sh 2>&1 | tail -4`(全绿)
Run: `RD_INTERACTIVE_FRAMES=30 ./build/tools/render_test/render_test --interactive --scene cornell_box`(exit 0)
Run: iOS build / Android assembleDebug 成功。

- [ ] **Step 5: Commit**

```bash
git add tools/render_test samples platform/android
git commit -m "feat(tools+samples): interactive --scene 直驱 + 移动 demo 画质/模型切换"
```

---

### Task 5: AGENTS.md 收尾 + 全量回归

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: AGENTS.md 更新**

- 「当前状态」追加:

```markdown
- 场景示例集合完成:primitives 几何生成器(球/平面/盒)+ 5 程序场景
  (material_balls/cornell_box/light_playground/skinned_demo/instanced_field)
  + 知名场景(sponza/cesium_man,fetch_assets.sh 下载)+ render_test --scene
  + interactive --scene + 移动 demo 画质/模型切换
- 下一步:P3(AR + 鸿蒙)或 P4(打磨)
```

- 「构建与测试」追加:

```markdown
- 场景示例:`./build/tools/render_test/render_test --scene <name> --out x.png`;
  交互 `--interactive --scene <name>`;知名资产 `./scripts/fetch_assets.sh` 下载
```

- [ ] **Step 2: 全量回归**(check.sh 全绿 + Android/iOS 构建成功)

- [ ] **Step 3: Commit**

```bash
git add AGENTS.md
git commit -m "docs: 场景示例集合收尾(AGENTS.md)"
```

---

## 附:已知风险与决策记录

| 风险/决策 | 处理 |
|---|---|
| 移动 demo 范围偏差 | 程序场景属 tools 层;移动端经 C API 切画质档+模型(分层不破),spec §1 按此落地 |
| instanced_field 真 instancing | v1 走 N submit(RHI instancing 已有契约覆盖;Renderer 级 instancing 归 P4) |
| Sponza 体积(~25MB) | Task 3 Step 3 按下载结果与用户偏好决定入库或仅脚本 |
| sponza 取景 | 包围球自动取景;大场景漫游手感归 Orbit 现有手势 |
