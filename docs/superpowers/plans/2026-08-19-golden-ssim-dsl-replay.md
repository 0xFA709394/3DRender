# golden 测试三件套(SSIM + DSL + 输入注入回放)实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/superpowers/specs/2026-08-19-golden-ssim-dsl-replay-design.md` 实现:①SSIM 比较 + 全 golden 迁移;②声明式用例 DSL 宏;③输入注入 host 回放 + ctest 手势回归;④移动端录制闭环。

**通用约定:**
- 构建+测试:`export RD_DEPS_MIRROR=$HOME/.rd_deps_mirror && ./scripts/check.sh`;单跑 `ctest --test-dir build --output-on-failure -R <正则>`
- golden 更新:`RD_UPDATE_GOLDENS=1 ctest --test-dir build -R <用例>` 后**像素核对**再提交
- 提交规范:每 Task 一个 commit,`<type>(<scope>): 描述`

---

### Task 1: SSIM 比较 + 单测 + 全 golden 迁移

**Files:**
- Modify: `tests/common/image.h`、`tests/common/image.cpp`(compareSSIM)
- Test: `tests/common/image_test.cpp`(SSIM 用例)
- Modify: 全部 golden 用例的主判据(pbr_test/box_render_test/quality_test/ktx2_render_test/shadow_test/post_test/skinned_test/demo_scenes_test)

- [ ] **Step 1: 写失败测试**

`tests/common/image_test.cpp` 追加:

```cpp
// SSIM:同图 error=0;微噪声 <阈值;结构差异 >阈值
TEST(Image, SsimIdentical) {
  const uint32_t W = 64, H = 64;
  std::vector<uint8_t> a(W * H * 4), b(W * H * 4);
  for (auto& p : a) p = 128;
  b = a;
  auto r = rd::test::compareSSIM(a.data(), b.data(), W, H);
  EXPECT_TRUE(r.pass);
  EXPECT_NEAR(r.error, 0.0, 1e-9);
}

TEST(Image, SsimNoiseTolerance) {
  const uint32_t W = 64, H = 64;
  std::vector<uint8_t> a(W * H * 4), b(W * H * 4);
  for (uint32_t i = 0; i < W * H; ++i) {
    const uint8_t v = uint8_t((i * 7) & 0xFF);
    for (int c = 0; c < 4; ++c) {
      a[i * 4 + c] = v;
      b[i * 4 + c] = uint8_t(v + ((i + c) % 3) - 1);  // ±1 微噪声
    }
  }
  auto r = rd::test::compareSSIM(a.data(), b.data(), W, H, 0.05);
  EXPECT_TRUE(r.pass) << "error=" << r.error;
}

TEST(Image, SsimStructuralFail) {
  const uint32_t W = 64, H = 64;
  std::vector<uint8_t> a(W * H * 4, 0), b(W * H * 4, 0);
  // b 左半全白:结构差异
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W / 2; ++x)
      for (int c = 0; c < 4; ++c) b[(y * W + x) * 4 + c] = 255;
  auto r = rd::test::compareSSIM(a.data(), b.data(), W, H, 0.05);
  EXPECT_FALSE(r.pass) << "error=" << r.error;
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`compareSSIM` 未声明)

- [ ] **Step 3: 实现 compareSSIM**

`tests/common/image.h` 追加声明:

```cpp
/// SSIM 比较结果:error = 1 - 平均 SSIM(0=完全一致)。
struct SsimResult {
  bool pass = false;
  double error = 0;
};
/// 亮度域 SSIM(8x8 窗口逐块,均值);errTol 为 error 上限(默认 0.05)。
SsimResult compareSSIM(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h,
                       double errTol = 0.05);
```

`tests/common/image.cpp` 实现:

```cpp
SsimResult compareSSIM(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h,
                       double errTol) {
  // 亮度域
  auto luma = [](const uint8_t* p) {
    return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
  };
  const double c1 = (0.01 * 255) * (0.01 * 255);
  const double c2 = (0.03 * 255) * (0.03 * 255);
  double sum = 0;
  uint32_t blocks = 0;
  for (uint32_t by = 0; by < h; by += 8)
    for (uint32_t bx = 0; bx < w; bx += 8) {
      const uint32_t bw = std::min(8u, w - bx), bh = std::min(8u, h - by);
      const uint32_t n = bw * bh;
      double mux = 0, muy = 0;
      for (uint32_t y = 0; y < bh; ++y)
        for (uint32_t x = 0; x < bw; ++x) {
          mux += luma(a + ((by + y) * w + bx + x) * 4);
          muy += luma(b + ((by + y) * w + bx + x) * 4);
        }
      mux /= n;
      muy /= n;
      double vx = 0, vy = 0, cxy = 0;
      for (uint32_t y = 0; y < bh; ++y)
        for (uint32_t x = 0; x < bw; ++x) {
          const double lx = luma(a + ((by + y) * w + bx + x) * 4) - mux;
          const double ly = luma(b + ((by + y) * w + bx + x) * 4) - muy;
          vx += lx * lx;
          vy += ly * ly;
          cxy += lx * ly;
        }
      vx /= n;
      vy /= n;
      cxy /= n;
      sum += ((2 * mux * muy + c1) * (2 * cxy + c2)) /
             ((mux * mux + muy * muy + c1) * (vx + vy + c2));
      ++blocks;
    }
  const double err = 1.0 - sum / double(blocks);
  return {err <= errTol, err};
}
```

(`<cmath>`/`<algorithm>` 如缺则补)

- [ ] **Step 4: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 全绿(Image.Ssim* 通过)

- [ ] **Step 5: 全 golden 迁移主判据**

全部 golden 用例的判据从
`compareRGBA8(..., 3, 0.02).pass` 改为 `compareSSIM(...).pass`,失败时附加打印
pixel diffRatio 供定位:

```cpp
  auto cmp = rd::test::compareSSIM(img.pixels.data(), golden.pixels.data(), kW, kH);
  auto pix = rd::test::compareRGBA8(img.pixels.data(), golden.pixels.data(), kW, kH, 3, 1.0);
  EXPECT_TRUE(cmp.pass) << "ssimError=" << cmp.error << " pixelDiffRatio=" << pix.diffRatio;
```

涉及文件:pbr_test/box_render_test/quality_test/ktx2_render_test/shadow_test/
post_test/skinned_test/demo_scenes_test。

- [ ] **Step 6: 迁移验证(零回归)**

Run: `./scripts/check.sh 2>&1 | tail -4`
Expected: 全绿——**若某 golden SSIM 误判,先查实现再考虑放宽该用例阈值并注明原因**
Commit: `feat(test): golden 比较升级 SSIM(自实现)+ 全量迁移`

---

### Task 2: 声明式用例 DSL 宏

**Files:**
- Create: `tests/common/golden_test.h`
- Modify: 全部 golden 用例改造为宏(pbr/box/quality/ktx2/shadow/post/skinned/demo_scenes)

- [ ] **Step 1: DSL 宏**

`tests/common/golden_test.h`:

```cpp
/**
 * @file golden_test.h
 * @brief golden 用例声明式宏:RD_GOLDEN_TEST 展开 Metal+Vulkan 双用例,
 * 消灭 twin 样板。runner 由调用方提供(签名见下)。
 */
#pragma once
#include "rhi/rhi_types.h"
#include <string>

namespace rd::test {
/// golden 渲染函数原型:按后端渲染 → RGBA8 图像;失败返回空。
class Image;
using GoldenRenderFn = Image (*)(Backend, const char*);
/// 公共比较流程:渲染 → RD_UPDATE_GOLDENS 落盘 → SSIM 断言。
void runGoldenPair(Backend b, const char* scene, const char* goldenName,
                   GoldenRenderFn fn, double tol);
} // namespace rd::test

/// 声明双后端 golden 用例:RD_GOLDEN_TEST(Suite, Name, scene, golden, tol, renderFn)
/// renderFn 签名:rd::test::Image( rd::Backend, const char* scene)
#define RD_GOLDEN_TEST(Suite, Name, Scene, Golden, Tol, Fn)                       \
  TEST(Suite, Name##Metal) {                                                      \
    _Pragma("applec")                                                             \
    rd::test::runGoldenPair(rd::Backend::Metal, Scene, Golden, Fn, Tol);          \
  }                                                                               \
  TEST(Suite, Name##Vulkan) {                                                     \
    _Pragma("vulkanc")                                                            \
    rd::test::runGoldenPair(rd::Backend::Vulkan, Scene, Golden, Fn, Tol);         \
  }
```

(_Pragma 行换成 `#if defined(__APPLE__)` / `#if defined(RD_WITH_VULKAN)` 的真实
门控——宏体内不能直接 #if,改为:展开为函数体含 `#if` 的 TEST)

修正写法(宏内合法门控):

```cpp
#define RD_GOLDEN_TEST(Suite, Name, Scene, Golden, Tol, Fn)                       \
  TEST(Suite, Name##Metal) {                                                      \
    if (Fn) {}                                                                    \
    rd::test::runGoldenPair(rd::Backend::Metal, Scene, Golden, Fn, Tol);          \
  }                                                                               \
  TEST(Suite, Name##Vulkan) {                                                     \
    rd::test::runGoldenPair(rd::Backend::Vulkan, Scene, Golden, Fn, Tol);         \
  }
```

(runGoldenPair 内部按后端可用性自行 skip——`if constexpr` 不可用宏检测;
实现:runGoldenPair 里 `if (b==Vulkan && !RD_WITH_VULKAN 编译期)`——
**最终决定**:门控留在 runGoldenPair 的运行时:后端设备创建失败则
GTEST_SKIP,宏体无需 #if。)

- [ ] **Step 2: runGoldenPair 实现 + 改造**

`tests/common/golden_test.cpp`(新建,入 rd_tests):

```cpp
#include "common/golden_test.h"
#include "common/image.h"
#include <cstdlib>

namespace rd::test {
void runGoldenPair(Backend b, const char* scene, const char* goldenName,
                   GoldenRenderFn fn, double tol) {
  auto img = fn(b, scene);
  ASSERT_FALSE(img.pixels.empty()) << scene;
  const std::string path = std::string(RD_TEST_DATA_DIR) + "/golden/" + goldenName;
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(savePNG(path, img.width, img.height, img.pixels.data()));
    return;
  }
  auto golden = loadPNG(path);
  ASSERT_EQ(golden.pixels.size(), img.pixels.size()) << "golden 缺失: " << path;
  auto cmp = compareSSIM(img.pixels.data(), golden.pixels.data(), img.width, img.height, tol);
  auto pix = compareRGBA8(img.pixels.data(), golden.pixels.data(), img.width, img.height, 3, 1.0);
  EXPECT_TRUE(cmp.pass) << "ssimError=" << cmp.error << " pixelDiffRatio=" << pix.diffRatio;
}
} // namespace rd::test
```

改造:每个 golden 用例文件把 twin TEST 换成宏,render 函数抽出为
`static rd::test::Image renderXxx(rd::Backend b, const char* scene)`(scene 参数
即 golden 名复用或场景名——按现状;无场景参数的可忽略)。

`tests/CMakeLists.txt` 追加 `common/golden_test.cpp`。

- [ ] **Step 3: 跑测试 + 提交**

Run: `./scripts/check.sh 2>&1 | tail -4`(全绿)
Commit: `refactor(test): golden 用例 DSL 化(RD_GOLDEN_TEST)+ 双后端 twin 消灭`

---

### Task 3: host 输入注入回放 + ctest 手势回归

**Files:**
- Modify: `tools/render_test/interactive.h/.mm`(--record/--play)
- Create: `tests/recordings/orbit_drag.log`(工具生成后提交)、`tests/golden/replay_orbit_metal.png`、`replay_orbit_vulkan.png`
- Test: `tests/api/replay_test.cpp`(新建)、`tests/CMakeLists.txt`(注册)

- [ ] **Step 1: 录制/回放**

`interactive.h` 签名扩展:

```cpp
/// record 非空:录制指针事件到该路径;play 非空:按日志回放(确定性 dt)。
int runInteractive(const char* modelPath, const char* sceneName,
                   const char* recordPath, const char* playPath);
```

`interactive.mm`:
- 录制:GLFW 回调里把事件写文本行 `t action id x y`(t=ms,x/y 归一化 0..1);
  scroll 记 `t scroll dy`;double-tap 记 `t dtap x y`。glfwGetTime 毫秒。
- 回放:读全部行;逐行 `glfwWaitEventsTimeout`?——不:回放按日志时间戳推进:
  每帧处理 t ≤ 当前虚拟时间的全部事件,dt = 相邻时间戳差;经 C API
  rd_engine_on_pointer/on_scroll/on_double_tap 喂给引擎;渲到日志结束+
  额外 5 帧收尾(惯性);末帧 readback...——**engine 路径离屏不可 readback**;
  简化:回放走离屏渲染循环?回放目的是测 Orbit 路径:engine 无 surface 时
  render_frame no-op……

  **修正方案(以代码为准)**:回放不走 engine;走**直驱**:
  Renderer+Scene+OrbitController 离屏,事件喂 OrbitController,固定 dt 推进,
  末帧 offscreen readback。复用 interactive.mm 的 runSceneInteractive 骨架,
  加 playPath 参数;窗口可建(不展示也行)——为 CI 无窗口兼容,直接离屏渲
  (无 GLFW):回放完全在离屏路径,**不经 GLFW**。

- [ ] **Step 2: orbit_drag.log 录制(手工合成,确定性)**

`tests/recordings/orbit_drag.log`(文本,首行视口尺寸):

```
# viewport 512 512
0 down 0 0.50 0.50
16 move 0 0.55 0.50
32 move 0 0.60 0.50
48 move 0 0.65 0.50
64 move 0 0.70 0.50
80 up 0 0.70 0.50
120 scroll 120.0
200 dtap 0.50 0.50
```

(录制脚本可由工具生成;首份手工写)

- [ ] **Step 3: 回放测试 + golden**

`tests/api/replay_test.cpp`:

```cpp
// 输入回放回归:orbit_drag.log → 离屏渲染 → 末帧 SSIM golden。
// 路径:直接驱动 Renderer+OrbitController(不经 engine;确定性固定 dt)。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
... (helmet 场景渲染骨架,复用 pbr_test 的 build 段)
// 读 log:逐行 t action id x y;按 t 排序;虚拟时间从 0 推进,
// 每个事件调到 OrbitController;帧间隔=相邻事件 t 差(末事件后补 5 帧惯性)。
// 末帧 compareSSIM golden(tol 0.05)。
```

- [ ] **Step 4: 跑测试 + 生成 golden + Commit**

Run: `./scripts/check.sh`(replay 缺失报错)
Run: `RD_UPDATE_GOLDENS=1 ctest --test-dir build -R Replay` 生成 + 核对
Commit: `feat(test): 输入注入回放(Orbit 手势回归)+ replay golden`

---

### Task 4: 移动端录制闭环

**Files:**
- Modify: `platform/ios/RenderView.swift`、`samples/ios/RdDemo/AppDelegate.swift`(录制开关)
- Modify: `platform/android/.../RenderView.kt`、`samples/android/.../MainActivity.kt`(同)

- [ ] **Step 1: iOS 录制**

RenderView.swift 加 `recordingPath: String?` + 事件写入(与 host 同格式,
坐标归一化):touchesBegan/Moved/Ended/Cancelled 追加行;
AppDelegate:长按「切换」按钮 2s 切换录制(开始/结束);
文件写 `Documents/rd_input.log`。

- [ ] **Step 2: Android 录制**

RenderView.kt 同:MotionEvent 写入 `filesDir/rd_input.log`;
MainActivity 长按切录制。

- [ ] **Step 3: 闭环验证**

iOS 模拟器:开录制 → 拖几下 → 关录制 → `xcrun simctl get_app_container <sim> com.rd.demo data` 取 log → host `render_test --play` 回放任一 golden 场景末帧一致。
(本步手动验证,构建/测试自动化不依赖)

- [ ] **Step 4: Commit**

Commit: `feat(platform): 移动端输入录制(与 host 回放同格式)`

---

### Task 5: AGENTS.md 收尾 + 全量回归

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: AGENTS.md 更新**

- 「当前状态」追加三件套。
- 「构建与测试」追加:
  - golden 判据 = SSIM(compareSSIM,阈值默认 0.05);pixel diffRatio 为辅助日志
  - golden 用例一律 `RD_GOLDEN_TEST` 宏;新增场景只写 renderFn
  - 输入回放:`render_test --interactive --play tests/recordings/orbit_drag.log`;
    移动端录制:demo 长按「切换」起停,日志在 app 文档目录

- [ ] **Step 2: 全量回归**(check.sh 全绿 + Android/iOS 构建成功)

- [ ] **Step 3: Commit**

Commit: `docs: golden 三件套收尾(AGENTS.md)`

---

## 附:已知风险与决策记录

| 风险/决策 | 处理 |
|---|---|
| SSIM 比逐像素宽松 | 主判据 SSIM + pixel diffRatio 辅助日志;单用例可单独放宽并注明 |
| 回放不经 engine(engine 无 surface 无渲染) | 回放走 Renderer+OrbitController 直驱离屏;C API 输入路径已被 api_test 覆盖 |
| 宏内 #if 不合法 | 门控移到 runGoldenPair 运行时(后端不可用 → GTEST_SKIP) |
| 移动端回放不在真机跑 | 回放只在 host;移动端只负责录制同格式日志 |
