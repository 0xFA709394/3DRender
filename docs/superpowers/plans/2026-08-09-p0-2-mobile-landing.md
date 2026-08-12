# P0-2 移动端落地：GLES 后端 + SwapChain + C API + 双端 RenderView Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 P0-1 内核之上完成移动端闭环：GLES3 后端实现、三后端 SwapChain 支持、统一 C API（engine）、Android RenderView+sample（Vulkan/GLES 双后端模拟器截图验证）、iOS RenderView+sample（模拟器截图验证），达成 spec P0 验收"双端 demo 旋转立方体，三后端 golden image 一致"。

**Architecture:** RHI 增加 SwapChain 抽象（`createSwapChain/acquireSwapChainTarget/present/resize`）；cube 渲染迁入 `core/scene`（CubeScene，shader 字节由调用方注入）；C API `rd_engine_*` 持有 Device+SwapChain+CubeScene，约定**所有 engine 调用在同一个"指定渲染线程"**（Android=专用 HandlerThread，iOS=主线程，与 MTKView 惯例一致）；shader 字节通过生成的 `embedded_shaders.cpp` 内嵌（Android 用 spv/gles，iOS 用 iphoneos 编译的 metallib）。

**Tech Stack:** 既有内核 + Android SDK 34/NDK 26/Gradle 8.9/AGP 8.5.2/Kotlin 2.0.20、Xcode 26（iOS 模拟器）、EGL/GLES3、CAMetalLayer、vkCreateAndroidSurfaceKHR

**前置：**
- P0-1 已完成且 `./scripts/check.sh` 全绿（25/25）
- 设计文档：`docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md`
- 本机已有 Xcode 26.5 + iPhone 模拟器（已确认）

**关键环境事实（P0-1 执行中确认，勿再踩坑）：**
- brew 公式名是 `molten-vk`；本机 arm64，须用 `/opt/homebrew` 的 ARM 版
- glslang 14 的可执行目标是 `glslang-standalone`
- SPIR-V 入口名为 `main`；spirv-cross 产物（MSL/GLSL）为 `main0`/GLSL 保留 `main`；Metal 用 `main0`，Vulkan/GLES 用 `main`
- Vulkan 直连 MoltenVK ICD 时不要请求 `VK_KHR_portability_enumeration`

---

## 文件结构（本计划产出/变更）

```
cmake/EmbedShaders.cmake                 # configure 期内嵌生成（平台构建用）（Task 4）
cmake/GenEmbedded.cmake                  # build 期内嵌生成脚本（host 用）（Task 4）
cmake/ios.toolchain.cmake                # iOS 交叉编译 toolchain（Task 13）
core/CMakeLists.txt                      # 增加 scene/api 源、Android 链接、vulkan 条件修正
core/rhi/rhi_types.h                     # + SwapChainHandle（Task 2）
core/rhi/rhi_device.h                    # + SwapChain 方法（Task 2）
core/rhi/backends/metal/metal_device.mm  # + Metal swapchain（Task 5）
core/rhi/backends/vulkan/vulkan_device.cpp # + Android surface/swapchain（Task 8）
core/rhi/backends/gles/gles_device.cpp   # GLES3 完整实现（Task 6）
core/scene/cube_scene.{h,cpp}            # 从 tests/common 迁入并泛化（Task 3）
core/api/rd_api.h, rd_api.cpp            # C API engine（Task 9）
core/api/embedded_shaders.h              # 内嵌 shader 访问声明（Task 4）
tests/common/cube_renderer.*             # 删除，由 shader_code.* 替代（Task 3）
tests/common/shader_code.{h,cpp}         # 从文件加载 shader 字节的测试辅助（Task 3）
tests/api/api_test.cpp                   # C API host 测试（Task 9）
tools/img_check/main.cpp + CMakeLists    # 截图结构校验工具（Task 7）
platform/android/CMakeLists.txt          # rd_jni 目标（Task 10）
platform/android/build.gradle            # renderer library 模块（Task 10）
platform/android/src/main/AndroidManifest.xml
platform/android/src/main/java/com/rd/renderer/RenderView.kt
platform/android/jni/rd_jni.cpp
samples/android/settings.gradle, build.gradle, gradle.properties
samples/android/app/build.gradle, src/main/AndroidManifest.xml
samples/android/app/src/main/java/com/rd/sample/MainActivity.kt
samples/ios/CMakeLists.txt               # RdDemo app target（Task 13）
samples/ios/RdDemo/Info.plist, RdDemo-Bridging-Header.h, AppDelegate.swift
platform/ios/RenderView.swift
.github/workflows/ci.yml                 # 追加 Android 构建 job（Task 12）
AGENTS.md                                # 状态更新（Task 14）
```

---

### Task 1: Android 工具链安装与验证

**Files:** 无（环境准备）

- [ ] **Step 1: 安装 JDK 与 Gradle（免 sudo）**

```bash
brew install openjdk@21 gradle   # AGP 8.5 + Gradle 8.9 需要 JDK 17~22，锁 21 避免新版不兼容
```
记录 JAVA_HOME 供后续所有 gradle 调用使用：
```bash
echo 'export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home' >> /tmp/rd_env.sh
export JAVA_HOME=/opt/homebrew/opt/openjdk@21/libexec/openjdk.jdk/Contents/Home
java -version 2>&1 | head -1   # 预期 openjdk 21
gradle --version 2>&1 | grep Gradle  # 预期 Gradle 8.x/9.x
```

- [ ] **Step 2: 安装 Android cmdline-tools**

```bash
mkdir -p ~/Library/Android/sdk/cmdline-tools
curl -L -o /tmp/cmdtools.zip https://dl.google.com/android/repository/commandlinetools-mac-11076708_latest.zip
unzip -q /tmp/cmdtools.zip -d ~/Library/Android/sdk/cmdline-tools
mv ~/Library/Android/sdk/cmdline-tools/cmdline-tools ~/Library/Android/sdk/cmdline-tools/latest
export ANDROID_HOME=~/Library/Android/sdk
export PATH=$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH
echo "export ANDROID_HOME=~/Library/Android/sdk" >> /tmp/rd_env.sh
echo 'export PATH=$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH' >> /tmp/rd_env.sh
```

- [ ] **Step 3: 接受许可并安装 SDK 组件（下载约 3GB，耗时）**

```bash
source /tmp/rd_env.sh
yes | sdkmanager --licenses > /dev/null
sdkmanager "platform-tools" "platforms;android-34" "build-tools;34.0.0" "ndk;26.3.11579264" "emulator" "system-images;android-34;google_apis;arm64-v8a"
```
预期：结尾 `100%` 无错误。

- [ ] **Step 4: 创建模拟器并验证**

```bash
source /tmp/rd_env.sh
avdmanager create avd -n rdtest -k "system-images;android-34;google_apis;arm64-v8a" -d pixel_6 --force
emulator -list-avds    # 预期输出含 rdtest
adb --version | head -1
```

- [ ] **Step 5: Commit（环境说明记录到计划勾选即可，无代码变更）**

无提交。在会话中记录：Android 环境就绪。

---

### Task 2: RHI SwapChain 抽象扩展

**Files:**
- Modify: `core/rhi/rhi_types.h`
- Modify: `core/rhi/rhi_device.h`
- Modify: `core/rhi/backends/metal/metal_device.mm`（仅加未实现的虚函数桩，Task 5 填充）
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`（同，Task 8 填充）
- Test: `tests/rhi/swapchain_test.cpp`

- [ ] **Step 1: 写失败测试**

`tests/rhi/swapchain_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"

// host 无窗口系统：非法 native window 应返回无效句柄而非崩溃
TEST(SwapChain, NullNativeWindowReturnsInvalid) {
#if defined(__APPLE__)
  rd::DeviceDesc desc;
  desc.backend = rd::Backend::Metal;
  auto device = rd::createDevice(desc);
  ASSERT_NE(device, nullptr);
  auto sc = device->createSwapChain(nullptr, 512, 512);
  EXPECT_FALSE(sc.valid());
#endif
}
```

`tests/CMakeLists.txt` 源列表追加 `rhi/swapchain_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

```bash
cmake -S . -B build > /dev/null 2>&1 && cmake --build build -j8 2>&1 | grep -E "error" | head -3
```
预期：`createSwapChain` 不是 Device 成员等编译错误。

- [ ] **Step 3: 扩展 rhi_types.h 与 rhi_device.h**

`core/rhi/rhi_types.h` 句柄区追加：
```cpp
struct SwapChainTag;
using SwapChainHandle = Handle<SwapChainTag>;
```
（加在 `using TargetHandle = Handle<TargetTag>;` 之后）

`core/rhi/rhi_device.h` Device 类追加纯虚函数（加在 `waitIdle` 之前）：
```cpp
  // ---- SwapChain（上屏渲染）----
  // nativeWindow：Android=ANativeWindow*，iOS=CAMetalLayer*。nullptr/非法返回无效句柄。
  virtual SwapChainHandle createSwapChain(void* nativeWindow, uint32_t width, uint32_t height) = 0;
  virtual void resizeSwapChain(SwapChainHandle swapChain, uint32_t width, uint32_t height) = 0;
  // 获取当前帧可渲染目标（Metal: nextDrawable；Vulkan: acquireNextImage；GLES: 默认帧缓冲）。
  // 失败（如表面丢失中）返回无效 TargetHandle，调用方应跳过本帧。
  virtual TargetHandle acquireSwapChainTarget(SwapChainHandle swapChain) = 0;
  virtual void present(SwapChainHandle swapChain) = 0;
  virtual void destroySwapChain(SwapChainHandle swapChain) = 0;
```

- [ ] **Step 4: 两后端补桩（暂时返回无效）**

`core/rhi/backends/metal/metal_device.mm` 的 `MetalDevice` 类 public 区追加：
```objc
  SwapChainHandle createSwapChain(void*, uint32_t, uint32_t) override { return {}; }
  void resizeSwapChain(SwapChainHandle, uint32_t, uint32_t) override {}
  TargetHandle acquireSwapChainTarget(SwapChainHandle) override { return {}; }
  void present(SwapChainHandle) override {}
  void destroySwapChain(SwapChainHandle) override {}
```

`core/rhi/backends/vulkan/vulkan_device.cpp` 的 `VulkanDevice` 类声明区（`void waitIdle() override;` 之后）追加：
```cpp
  SwapChainHandle createSwapChain(void* nativeWindow, uint32_t width, uint32_t height) override;
  void resizeSwapChain(SwapChainHandle swapChain, uint32_t width, uint32_t height) override;
  TargetHandle acquireSwapChainTarget(SwapChainHandle swapChain) override;
  void present(SwapChainHandle swapChain) override;
  void destroySwapChain(SwapChainHandle swapChain) override;
```
文件尾部 `createVulkanDevice` 之前追加实现（Task 8 前为桩）：
```cpp
SwapChainHandle VulkanDevice::createSwapChain(void*, uint32_t, uint32_t) { return {}; }
void VulkanDevice::resizeSwapChain(SwapChainHandle, uint32_t, uint32_t) {}
TargetHandle VulkanDevice::acquireSwapChainTarget(SwapChainHandle) { return {}; }
void VulkanDevice::present(SwapChainHandle) {}
void VulkanDevice::destroySwapChain(SwapChainHandle) {}
```

- [ ] **Step 5: 跑测试确认通过**

```bash
cmake --build build -j8 2>&1 | grep error | head -3; ctest --test-dir build --output-on-failure
```
预期：全绿（含新测试：null window → 无效句柄 PASS）。

- [ ] **Step 6: Commit**

```bash
git add core/rhi/ tests/rhi/swapchain_test.cpp tests/CMakeLists.txt
git commit -m "feat(rhi): SwapChain 抽象接口 + 后端桩"
```

---

### Task 3: CubeScene 迁入 core（重构）

**Files:**
- Create: `core/scene/cube_scene.h`
- Create: `core/scene/cube_scene.cpp`
- Create: `tests/common/shader_code.h`
- Create: `tests/common/shader_code.cpp`
- Delete: `tests/common/cube_renderer.h`、`tests/common/cube_renderer.cpp`
- Modify: `tests/rhi/cube_test.cpp`
- Modify: `tools/render_test/main.cpp`
- Modify: `core/CMakeLists.txt`、`tests/CMakeLists.txt`、`tools/render_test/CMakeLists.txt`

**说明**：纯重构，把 cube 渲染器从"测试辅助"提升为内核演示场景，shader 字节改为调用方注入（为内嵌 shader 做准备）。行为不变，全部测试保持绿。

- [ ] **Step 1: 写 core/scene/cube_scene.h**

```cpp
#pragma once
#include "rhi/rhi_device.h"
#include <cstddef>
#include <cstdint>

namespace rd::demo {

// 顶点色立方体演示场景：8 顶点 / 36 索引，凸体 + 背面剔除（P0 无需深度缓冲）。
// shader 字节由调用方注入（测试从文件读，engine 用内嵌字节）。
class CubeScene {
public:
  bool init(Device& device, const uint8_t* vsCode, size_t vsSize, const uint8_t* fsCode,
            size_t fsSize, const char* entryPoint);
  void render(Device& device, TargetHandle target, uint32_t width, uint32_t height, float angleRad);
  void shutdown(Device& device);

private:
  BufferHandle vbo_, ibo_, ubo_;
  ShaderModuleHandle vs_, fs_;
  PipelineHandle pipeline_;
};

} // namespace rd::demo
```

- [ ] **Step 2: 写 core/scene/cube_scene.cpp**

```cpp
#include "scene/cube_scene.h"
#include "foundation/math.h"
#include <glm/glm.hpp>

namespace rd::demo {
namespace {

const float kVertices[] = {
    // pos(x,y,z)          color(r,g,b)
    -1, -1, -1,           1.0f, 0.0f, 0.0f, // 0
     1, -1, -1,           0.0f, 1.0f, 0.0f, // 1
     1,  1, -1,           0.0f, 0.0f, 1.0f, // 2
    -1,  1, -1,           1.0f, 1.0f, 0.0f, // 3
    -1, -1,  1,           1.0f, 0.0f, 1.0f, // 4
     1, -1,  1,           0.0f, 1.0f, 1.0f, // 5
     1,  1,  1,           1.0f, 1.0f, 1.0f, // 6
    -1,  1,  1,           0.2f, 0.2f, 0.2f, // 7
};

// 全部外侧 CCW（front-face = CCW）
const uint16_t kIndices[] = {
    4, 5, 6,  6, 7, 4,  // front (+z)
    1, 0, 3,  1, 3, 2,  // back  (-z)
    1, 2, 6,  1, 6, 5,  // right (+x)
    0, 4, 7,  0, 7, 3,  // left  (-x)
    3, 7, 6,  3, 6, 2,  // top   (+y)
    0, 1, 5,  0, 5, 4,  // bottom(-y)
};

} // namespace

bool CubeScene::init(Device& device, const uint8_t* vsCode, size_t vsSize, const uint8_t* fsCode,
                     size_t fsSize, const char* entryPoint) {
  vbo_ = device.createBuffer({sizeof(kVertices), BufferUsage::Vertex, kVertices});
  ibo_ = device.createBuffer({sizeof(kIndices), BufferUsage::Index, kIndices});
  ubo_ = device.createBuffer({64, BufferUsage::Uniform, nullptr});

  ShaderModuleDesc vsd;
  vsd.stage = ShaderStage::Vertex;
  vsd.code.assign(vsCode, vsCode + vsSize);
  vsd.entryPoint = entryPoint;
  vs_ = device.createShaderModule(vsd);

  ShaderModuleDesc fsd;
  fsd.stage = ShaderStage::Fragment;
  fsd.code.assign(fsCode, fsCode + fsSize);
  fsd.entryPoint = entryPoint;
  fs_ = device.createShaderModule(fsd);

  PipelineDesc pd;
  pd.vertexShader = vs_;
  pd.fragmentShader = fs_;
  pd.vertexBindings = {{0, 24}};
  pd.attributes = {{0, Format::R32G32B32_FLOAT, 0, 0}, {1, Format::R32G32B32_FLOAT, 12, 0}};
  pd.cullMode = CullMode::Back;
  pipeline_ = device.createPipeline(pd);

  return vbo_.valid() && ibo_.valid() && ubo_.valid() && vs_.valid() && fs_.valid() &&
         pipeline_.valid();
}

void CubeScene::render(Device& device, TargetHandle target, uint32_t width, uint32_t height,
                       float angleRad) {
  using namespace rd::math;
  Mat4 mvp = perspective(radians(45.0f), float(width) / float(height), 0.1f, 100.0f) *
             lookAt(Vec3(0, 0, 4), Vec3(0, 0, 0), Vec3(0, 1, 0)) *
             rotate(Mat4(1.0f), angleRad, glm::normalize(Vec3(1, 1, 0)));
  device.updateBuffer(ubo_, &mvp, sizeof(mvp), 0);

  CommandBuffer* cmd = device.acquireCommandBuffer();
  cmd->beginRenderPass(target, {0.1f, 0.1f, 0.12f, 1.0f});
  cmd->bindPipeline(pipeline_);
  cmd->bindUniformBuffer(0, ubo_, 0, 64);
  cmd->bindVertexBuffer(0, vbo_, 0);
  cmd->bindIndexBuffer(ibo_, 0);
  cmd->drawIndexed(36, 0, 0);
  cmd->endRenderPass();
  device.submit(cmd);
  device.waitIdle();
}

void CubeScene::shutdown(Device& device) {
  device.destroyPipeline(pipeline_);
  device.destroyShaderModule(vs_);
  device.destroyShaderModule(fs_);
  device.destroyBuffer(vbo_);
  device.destroyBuffer(ibo_);
  device.destroyBuffer(ubo_);
}

} // namespace rd::demo
```

`core/CMakeLists.txt` 源列表追加 `scene/cube_scene.cpp`。

- [ ] **Step 3: 写测试辅助 shader_code 并删除旧 cube_renderer**

`tests/common/shader_code.h`:
```cpp
#pragma once
#include "rhi/rhi_types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace rd::test {

struct ShaderCode {
  std::vector<uint8_t> vs;
  std::vector<uint8_t> fs;
  std::string entry; // Metal="main0"，其他="main"
};

// 从 shader 产物目录加载 cube shader（Metal→metallib，Vulkan→spv，GLES→gles）
ShaderCode loadCubeShaderCode(Backend backend, const std::string& shaderDir);

} // namespace rd::test
```

`tests/common/shader_code.cpp`:
```cpp
#include "common/shader_code.h"
#include <fstream>

namespace rd::test {
namespace {
std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  auto size = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  f.read(reinterpret_cast<char*>(data.data()), size);
  return data;
}
} // namespace

ShaderCode loadCubeShaderCode(Backend backend, const std::string& shaderDir) {
  ShaderCode out;
  const char* ext = ".spv";
  if (backend == Backend::Metal) ext = ".metallib";
  if (backend == Backend::GLES) ext = ".gles";
  out.vs = readFile(shaderDir + "/cube.vert" + ext);
  out.fs = readFile(shaderDir + "/cube.frag" + ext);
  out.entry = (backend == Backend::Metal) ? "main0" : "main";
  return out;
}

} // namespace rd::test
```

- [ ] **Step 4: 适配 cube_test.cpp 与 render_test/main.cpp**

`tests/rhi/cube_test.cpp` 的匿名命名空间内，`renderCube` 函数替换为：
```cpp
rd::test::Image renderCube(rd::Backend b) {
  rd::DeviceDesc desc;
  desc.backend = b;
  auto device = rd::createDevice(desc);
  if (!device) return {};
  auto target = device->createOffscreenTarget({kW, kH});
  rd::demo::CubeScene cube;
  auto code = rd::test::loadCubeShaderCode(b, RD_SHADER_DIR);
  bool ready = target.valid() &&
               cube.init(*device, code.vs.data(), code.vs.size(), code.fs.data(),
                         code.fs.size(), code.entry.c_str());
  if (!ready) return {};
  cube.render(*device, target, kW, kH, kAngle);
  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(static_cast<size_t>(kW) * kH * 4);
  bool ok = device->readbackTarget(target, img.pixels.data(), img.pixels.size());
  cube.shutdown(*device);
  device->destroyTarget(target);
  if (!ok) return {};
  return img;
}
```
头部 include 块替换：
```cpp
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "scene/cube_scene.h"
```
（移除 `#include "common/cube_renderer.h"`）

`tools/render_test/main.cpp` 同样适配：include 改为
```cpp
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "scene/cube_scene.h"
#include "rd_shader_dir.h"
```
中间渲染段替换为：
```cpp
  constexpr uint32_t kW = 512, kH = 512;
  auto target = device->createOffscreenTarget({kW, kH});
  rd::demo::CubeScene cube;
  auto code = rd::test::loadCubeShaderCode(backend, RD_SHADER_DIR);
  if (!target.valid() || !cube.init(*device, code.vs.data(), code.vs.size(), code.fs.data(),
                                    code.fs.size(), code.entry.c_str())) {
    fprintf(stderr, "初始化失败\n");
    return 1;
  }
  cube.render(*device, target, kW, kH, angleDeg * 0.0174532925f);
```

- [ ] **Step 5: 更新 CMake 并确认全绿**

`tests/CMakeLists.txt`：源列表中 `common/cube_renderer.cpp` 替换为 `common/shader_code.cpp`。
`tools/render_test/CMakeLists.txt`：`${CMAKE_SOURCE_DIR}/tests/common/cube_renderer.cpp` 替换为 `${CMAKE_SOURCE_DIR}/tests/common/shader_code.cpp`。

```bash
git rm tests/common/cube_renderer.h tests/common/cube_renderer.cpp
cmake -S . -B build > /dev/null 2>&1 && cmake --build build -j8 2>&1 | grep error | head -3
ctest --test-dir build --output-on-failure 2>&1 | grep -E "tests passed"
./build/tools/render_test/render_test --backend metal --out /tmp/refactor_check.png
```
预期：25/25 通过；render_test 正常出图。

- [ ] **Step 6: Commit**

```bash
git add core/scene/ core/CMakeLists.txt tests/ tools/
git commit -m "refactor(scene): CubeScene 迁入 core，shader 字节改为调用方注入"
```

---

### Task 4: 内嵌 shader 生成

**Files:**
- Create: `core/api/embedded_shaders.h`
- Create: `cmake/GenEmbedded.cmake`（build 期生成脚本）
- Create: `cmake/EmbedShaders.cmake`（configure 期生成函数，平台构建用）
- Modify: `core/CMakeLists.txt`（rd_core 挂接生成源）
- Modify: `cmake/ShaderCompile.cmake`（追加 iphoneos metallib 产物，供 iOS 内嵌）

**背景**：移动端无文件系统路径概念，shader 字节内嵌进二进制。host 构建时 shaders_out 产物在 build 期生成 → 用 add_custom_command 生成 cpp；Android/iOS 交叉构建时不能跑 host 工具链产物 → configure 期从已存在的目录读十六进制生成（Android 读 `build/shaders_out`，iOS 读 `build/shaders_out_ios`）。

- [ ] **Step 1: 写 embedded_shaders.h（稳定声明，签入仓库）**

`core/api/embedded_shaders.h`:
```cpp
#pragma once
#include "rhi/rhi_types.h"
#include <cstddef>
#include <cstdint>

namespace rd {
// 返回内嵌的 cube shader 字节；该后端无内嵌产物时返回 false。
// data/size 指向静态存储，调用方勿释放。
bool embeddedCubeShader(Backend backend, ShaderStage stage, const uint8_t** data, size_t* size);
} // namespace rd
```

- [ ] **Step 2: 写 cmake/GenEmbedded.cmake（build 期脚本）**

```cmake
# 用法: cmake -DOUT=<输出cpp> -DDIR=<shaders_out> -DDIR_IOS=<shaders_out_ios> -P GenEmbedded.cmake
# 不存在的产物生成占位空数组（sizeof==1），embeddedCubeShader 据此返回 false。
function(embed_file VAR_NAME FILE_PATH OUT_LINES)
  if(EXISTS ${FILE_PATH})
    file(READ ${FILE_PATH} hex HEX)
    string(REGEX MATCHALL ".." bytes "${hex}")
    set(body "")
    foreach(b ${bytes})
      string(APPEND body "0x${b},")
    endforeach()
    set(${OUT_LINES} "static const uint8_t ${VAR_NAME}[] = {${body}};\n" PARENT_SCOPE)
  else()
    set(${OUT_LINES} "static const uint8_t ${VAR_NAME}[] = {0};\n" PARENT_SCOPE)
  endif()
endfunction()

embed_file(kCubeVertSpv ${DIR}/cube.vert.spv L1)
embed_file(kCubeFragSpv ${DIR}/cube.frag.spv L2)
embed_file(kCubeVertGles ${DIR}/cube.vert.gles L3)
embed_file(kCubeFragGles ${DIR}/cube.frag.gles L4)
embed_file(kCubeVertMetal ${DIR}/cube.vert.metallib L5)
embed_file(kCubeFragMetal ${DIR}/cube.frag.metallib L6)
embed_file(kCubeVertMetalIos ${DIR_IOS}/cube.vert.metallib L7)
embed_file(kCubeFragMetalIos ${DIR_IOS}/cube.frag.metallib L8)

file(WRITE ${OUT} "// GENERATED FILE - 勿手改
#include \"api/embedded_shaders.h\"
namespace {
${L1}${L2}${L3}${L4}${L5}${L6}${L7}${L8}
}
namespace rd {
bool embeddedCubeShader(Backend backend, ShaderStage stage, const uint8_t** data, size_t* size) {
  const uint8_t* d = nullptr; size_t n = 0;
  const bool vs = (stage == ShaderStage::Vertex);
  switch (backend) {
    case Backend::Vulkan:
      if (vs) { d = kCubeVertSpv; n = sizeof(kCubeVertSpv); } else { d = kCubeFragSpv; n = sizeof(kCubeFragSpv); }
      break;
    case Backend::GLES:
      if (vs) { d = kCubeVertGles; n = sizeof(kCubeVertGles); } else { d = kCubeFragGles; n = sizeof(kCubeFragGles); }
      break;
    case Backend::Metal:
#if defined(RD_EMBED_IOS_METAL)
      if (vs) { d = kCubeVertMetalIos; n = sizeof(kCubeVertMetalIos); } else { d = kCubeFragMetalIos; n = sizeof(kCubeFragMetalIos); }
#else
      if (vs) { d = kCubeVertMetal; n = sizeof(kCubeVertMetal); } else { d = kCubeFragMetal; n = sizeof(kCubeFragMetal); }
#endif
      break;
  }
  if (!d || n <= 1) return false; // 占位空数组 sizeof==1
  *data = d; *size = n; return true;
}
} // namespace rd
")
```

- [ ] **Step 3: 写 cmake/EmbedShaders.cmake（configure 期函数，供 ANDROID/IOS）**

```cmake
# rd_embed_shaders_configure(<输出cpp> <spv/gles目录> <ios metallib目录>)
# configure 期直接读文件十六进制生成 cpp（交叉构建无法跑 host 工具，用此路径）
function(rd_embed_shaders_configure OUT DIR DIR_IOS)
  find_program(CMAKE_SELF cmake)
  execute_process(
    COMMAND ${CMAKE_SELF} -DOUT=${OUT} -DDIR=${DIR} -DDIR_IOS=${DIR_IOS}
            -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GenEmbedded.cmake
    RESULT_VARIABLE res)
  if(NOT res EQUAL 0)
    message(FATAL_ERROR "embedded shader 生成失败")
  endif()
endfunction()
```
（GenEmbedded.cmake 同时被 add_custom_command 与 configure 期 execute_process 复用，保证唯一生成逻辑。）

- [ ] **Step 4: ShaderCompile.cmake 追加 iOS metallib 产物**

在 `rd_compile_shader` 函数的 `if(APPLE)` 块内、`add_custom_target(shader_${F} ...)` 之前追加：
```cmake
    set(IOS_METALLIB ${RD_SHADER_OUT}_ios/${F}.metallib)
    add_custom_command(
      OUTPUT ${IOS_METALLIB}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${RD_SHADER_OUT}_ios
      COMMAND xcrun -sdk iphoneos metal -c ${MSL} -o ${RD_SHADER_OUT}_ios/${F}.air
      COMMAND xcrun -sdk iphoneos metallib ${RD_SHADER_OUT}_ios/${F}.air -o ${IOS_METALLIB}
      DEPENDS ${MSL}
      COMMENT "msl->metallib(ios) ${F}")
```
并把该 if 块内的 add_custom_target 行改为：
```cmake
    add_custom_target(shader_${F} ALL DEPENDS ${METALLIB} ${IOS_METALLIB} ${GLES} ${REFL})
```

- [ ] **Step 5: core/CMakeLists.txt 挂接生成源**

`core/CMakeLists.txt` 在 `add_library` 之后追加：
```cmake
# 内嵌 shader：host 构建 build 期生成；ANDROID/IOS configure 期生成
if(ANDROID)
  rd_embed_shaders_configure(${CMAKE_BINARY_DIR}/generated/embedded_shaders.cpp
    ${RD_HOST_SHADER_OUT} ${RD_HOST_SHADER_OUT}_ios)
  target_sources(rd_core PRIVATE ${CMAKE_BINARY_DIR}/generated/embedded_shaders.cpp)
elseif(IOS)
  rd_embed_shaders_configure(${CMAKE_BINARY_DIR}/generated/embedded_shaders.cpp
    ${RD_HOST_SHADER_OUT} ${RD_HOST_SHADER_OUT}_ios)
  target_compile_definitions(rd_core PRIVATE RD_EMBED_IOS_METAL=1)
  target_sources(rd_core PRIVATE ${CMAKE_BINARY_DIR}/generated/embedded_shaders.cpp)
else()
  set(RD_EMBEDDED_CPP ${CMAKE_BINARY_DIR}/generated/embedded_shaders.cpp)
  add_custom_command(
    OUTPUT ${RD_EMBEDDED_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${CMAKE_COMMAND} -DOUT=${RD_EMBEDDED_CPP} -DDIR=${RD_SHADER_OUT}
            -DDIR_IOS=${RD_SHADER_OUT}_ios -P ${CMAKE_SOURCE_DIR}/cmake/GenEmbedded.cmake
    DEPENDS shader_cube.vert shader_cube.frag
    COMMENT "生成 embedded_shaders.cpp")
  target_sources(rd_core PRIVATE ${RD_EMBEDDED_CPP})
endif()
```
顶层 `CMakeLists.txt` 在 `include(cmake/Deps.cmake)` 之后追加：
```cmake
if(ANDROID OR IOS)
  include(cmake/EmbedShaders.cmake)
  if(NOT DEFINED RD_HOST_SHADER_OUT)
    set(RD_HOST_SHADER_OUT ${CMAKE_SOURCE_DIR}/build/shaders_out)
  endif()
endif()
```
并给顶层 `add_subdirectory(tests)` 与 `add_subdirectory(tools/render_test)` 加保护（平台构建不含 host 测试/工具）：
```cmake
enable_testing()
add_subdirectory(core)
if(ANDROID OR IOS)
else()
  add_subdirectory(shaders)
  add_subdirectory(tests)
  add_subdirectory(tools/render_test)
endif()
```
注意：host 下 shaders 子目录定义了 RD_SHADER_OUT，core 引用它 → 需要把 shaders 的 add_subdirectory 提到 core 之前？不必：core 里仅引用变量 `${RD_SHADER_OUT}`，其定义在 include 的 ShaderCompile.cmake（由 shaders/CMakeLists include）。调整：顶层在 `add_subdirectory(core)` 前先 `include(cmake/ShaderCompile.cmake)`（提供 RD_SHADER_OUT 与函数），shaders/CMakeLists 删除自身 include 行（函数已定义，直接调用 rd_compile_shader）。

- [ ] **Step 6: 验证 host 构建与测试全绿**

```bash
cmake -S . -B build 2>&1 | tail -1 && cmake --build build -j8 2>&1 | grep -iE "error" | head -3
ctest --test-dir build 2>&1 | grep "tests passed"
ls build/shaders_out_ios/   # 预期 cube.vert.metallib cube.frag.metallib
grep -c embeddedCubeShader build/generated/embedded_shaders.cpp   # 预期 >= 2
```

- [ ] **Step 7: Commit**

```bash
git add cmake/ core/CMakeLists.txt core/api/embedded_shaders.h CMakeLists.txt shaders/CMakeLists.txt
git commit -m "build(shaders): 内嵌 shader 生成（host build 期 / 平台 configure 期）+ iOS metallib 产物"
```

---

### Task 5: Metal SwapChain 实现（CAMetalLayer）

**Files:**
- Modify: `core/rhi/backends/metal/metal_device.mm`

- [ ] **Step 1: 替换 Task 2 的 Metal 桩为完整实现**

`metal_device.mm` 头部 import 区追加（CAMetalLayer 定义在 QuartzCore）：
```objc
#import <QuartzCore/QuartzCore.h>
```
`core/CMakeLists.txt` 的 `if(APPLE)` 块中 framework 链接行改为：
```cmake
  target_link_libraries(rd_core PRIVATE "-framework Metal" "-framework Foundation" "-framework QuartzCore")
```

`metal_device.mm` 匿名命名空间内、`MetalCommandBuffer` 之前追加：
```objc
struct SwapChainRec {
  CAMetalLayer* layer = nil;
  uint32_t width = 0, height = 0;
  id<CAMetalDrawable> drawable = nil;
  TargetHandle currentTarget;
};
```

`MetalDevice` 类：桩函数删除，替换为以下声明+内联实现（放在 `waitIdle` 之后），并给私有区追加成员：
```objc
  SwapChainHandle createSwapChain(void* nativeWindow, uint32_t width, uint32_t height) override {
    if (!nativeWindow || width == 0 || height == 0) return {};
    CAMetalLayer* layer = (__bridge CAMetalLayer*)nativeWindow;
    if (![layer isKindOfClass:[CAMetalLayer class]]) return {};
    layer.device = device_;
    layer.pixelFormat = MTLPixelFormatRGBA8Unorm;
    layer.drawableSize = CGSizeMake(width, height);
    layer.framebufferOnly = YES;
    SwapChainHandle h(nextId_++);
    SwapChainRec rec;
    rec.layer = layer;
    rec.width = width;
    rec.height = height;
    swapChains_.emplace(h, rec);
    return h;
  }

  void resizeSwapChain(SwapChainHandle sc, uint32_t width, uint32_t height) override {
    auto it = swapChains_.find(sc);
    if (it == swapChains_.end() || width == 0 || height == 0) return;
    it->second.width = width;
    it->second.height = height;
    it->second.layer.drawableSize = CGSizeMake(width, height);
  }

  TargetHandle acquireSwapChainTarget(SwapChainHandle sc) override {
    auto it = swapChains_.find(sc);
    if (it == swapChains_.end()) return {};
    SwapChainRec& rec = it->second;
    rec.drawable = [rec.layer nextDrawable];
    if (!rec.drawable) return {};
    // 复用/注册当前帧 target（drawable 纹理每帧不同，内容即时更新）
    if (!rec.currentTarget.valid()) {
      rec.currentTarget = TargetHandle(nextId_++);
    }
    targets_[rec.currentTarget] = TargetRec{rec.drawable.texture, rec.width, rec.height};
    return rec.currentTarget;
  }

  void present(SwapChainHandle sc) override {
    auto it = swapChains_.find(sc);
    if (it == swapChains_.end() || !it->second.drawable) return;
    waitIdle();
    [it->second.drawable present];
    it->second.drawable = nil;
  }

  void destroySwapChain(SwapChainHandle sc) override {
    auto it = swapChains_.find(sc);
    if (it == swapChains_.end()) return;
    if (it->second.currentTarget.valid()) targets_.erase(it->second.currentTarget);
    swapChains_.erase(it);
  }
```
私有成员追加：
```objc
  std::unordered_map<SwapChainHandle, SwapChainRec> swapChains_;
```

注意：`readbackTarget` 对 swapchain target 会失败（framebufferOnly 纹理不可读）——在 `readbackTarget` 开头加防护：
```objc
    if (it->second.color.storageMode == MTLStorageModePrivate) {
      RD_LOGW("rhi.metal", "swapchain target 不支持 readback");
      return false;
    }
```
（插到 `const TargetRec& t = it->second;` 之后、`if (outSize < ...)` 之前，并把后续引用改为 `t`——现有代码已是 `it->second`，把防护里的判断改为 `it->second.color.storageMode` 即可，无需重排。）

- [ ] **Step 2: 验证 host 构建与测试**

```bash
cmake --build build -j8 2>&1 | grep error | head -3; ctest --test-dir build 2>&1 | grep "tests passed"
```
预期：编译通过，25/25 绿（swapchain 运行时验证在 iOS 任务）。

- [ ] **Step 3: Commit**

```bash
git add core/rhi/backends/metal/metal_device.mm
git commit -m "feat(rhi/metal): CAMetalLayer swapchain（acquire/present/resize）"
```

---

### Task 6: GLES 后端完整实现

**Files:**
- Modify: `core/rhi/backends/gles/gles_device.cpp`（替换桩为完整实现）
- Modify: `core/CMakeLists.txt`（Android 链接 EGL/GLESv3）
- Modify: `core/rhi/rhi_factory.cpp`（`__ANDROID__` 分支引用 createGLESDevice —— 已有，无需改）

**说明**：GLES 后端只能在 Android 编译验证（host 无 GLES 驱动），本任务交付代码，编译验证在 Task 11（首个 Android 构建），运行时验证在 Task 12（模拟器截图）。实现要点：
- EGL 上下文 + GLES3；离屏用 FBO+纹理，上屏用 ANativeWindow EGL surface
- uniform block：`glGetUniformBlockIndex(program,"UBO")` + `glUniformBlockBinding(...,slot)`（P0 硬编码块名 UBO↔slot0，P1 由反射 JSON 驱动）
- 命令立即执行（GL 调用本就进驱动队列），线程安全由"仅渲染线程调用"保证
- readback：glReadPixels 后**垂直翻转行**（GL 原点在左下，输出与 Metal/Vulkan 一致的顶向下 RGBA）

- [ ] **Step 1: 替换 gles_device.cpp 桩为完整实现**

`core/rhi/backends/gles/gles_device.cpp`:
```cpp
#include "gles_device.h"

#if defined(__ANDROID__)
#include "foundation/log.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rd {
namespace {

struct BufferRec { GLuint buffer = 0; GLenum target = GL_ARRAY_BUFFER; uint64_t size = 0; };
struct ShaderRec { GLuint shader = 0; ShaderStage stage; };
struct PipelineRec {
  GLuint program = 0;
  std::vector<VertexBinding> bindings;
  std::vector<VertexAttribute> attribs;
  GLenum topology = GL_TRIANGLES;
  CullMode cull = CullMode::None;
};
struct TargetRec {
  GLuint fbo = 0;       // 0 = 默认帧缓冲（swapchain）
  GLuint colorTex = 0;  // 离屏纹理
  uint32_t width = 0, height = 0;
  bool isSwapchain = false;
};
struct SwapChainRec {
  ANativeWindow* window = nullptr;
  EGLSurface surface = EGL_NO_SURFACE;
  uint32_t width = 0, height = 0;
  TargetHandle currentTarget;
};

GLenum toGLTopology(PrimitiveTopology t) {
  switch (t) {
    case PrimitiveTopology::TriangleList:  return GL_TRIANGLES;
    case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
    case PrimitiveTopology::LineList:      return GL_LINES;
  }
  return GL_TRIANGLES;
}

GLenum toGLAttribType(Format f) { return f == Format::RGBA8_UNORM ? GL_UNSIGNED_BYTE : GL_FLOAT; }
GLint toGLAttribSize(Format f) {
  switch (f) {
    case Format::R32G32_FLOAT:        return 2;
    case Format::R32G32B32_FLOAT:     return 3;
    case Format::R32G32B32A32_FLOAT:  return 4;
    case Format::RGBA8_UNORM:         return 4;
    default:                          return 3;
  }
}

class GLESDevice;

class GLESCommandBuffer final : public CommandBuffer {
public:
  explicit GLESCommandBuffer(GLESDevice* device) : device_(device) {}
  void beginRenderPass(TargetHandle target, const ClearColor& clear) override;
  void bindPipeline(PipelineHandle pipeline) override;
  void bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) override;
  void bindIndexBuffer(BufferHandle buffer, uint64_t offset) override;
  void bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t size) override;
  void draw(uint32_t vertexCount, uint32_t firstVertex) override;
  void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset) override;
  void endRenderPass() override {}

private:
  void applyVertexState();

  GLESDevice* device_;
  TargetRec current_{};
  PipelineRec pipeline_{};
  BufferHandle vertexBuffers_[8];
  uint64_t vertexOffsets_[8] = {};
  BufferHandle indexBuffer_;
  uint64_t indexOffset_ = 0;
};

class GLESDevice final : public Device {
public:
  ~GLESDevice() override { shutdownEGL(); }
  bool init(const DeviceDesc&);
  Backend backend() const override { return Backend::GLES; }

  BufferHandle createBuffer(const BufferDesc& desc) override;
  void updateBuffer(BufferHandle buffer, const void* data, uint64_t size, uint64_t offset) override;
  void destroyBuffer(BufferHandle buffer) override;
  ShaderModuleHandle createShaderModule(const ShaderModuleDesc& desc) override;
  void destroyShaderModule(ShaderModuleHandle module) override;
  PipelineHandle createPipeline(const PipelineDesc& desc) override;
  void destroyPipeline(PipelineHandle pipeline) override;
  TargetHandle createOffscreenTarget(const OffscreenTargetDesc& desc) override;
  void destroyTarget(TargetHandle target) override;
  bool readbackTarget(TargetHandle target, void* outRGBA8, uint64_t outSize) override;
  CommandBuffer* acquireCommandBuffer() override { return &cmdBuf_; }
  void submit(CommandBuffer* cmd) override;
  void waitIdle() override;
  SwapChainHandle createSwapChain(void* nativeWindow, uint32_t width, uint32_t height) override;
  void resizeSwapChain(SwapChainHandle swapChain, uint32_t width, uint32_t height) override;
  TargetHandle acquireSwapChainTarget(SwapChainHandle swapChain) override;
  void present(SwapChainHandle swapChain) override;
  void destroySwapChain(SwapChainHandle swapChain) override;

  // ---- CommandBuffer 访问 ----
  bool makeCurrent(EGLSurface surface);
  void ensureOffscreenCurrent();
  GLuint buffer(BufferHandle h) const {
    auto it = buffers_.find(h);
    return it == buffers_.end() ? 0 : it->second.buffer;
  }
  const BufferRec* bufferRec(BufferHandle h) const {
    auto it = buffers_.find(h);
    return it == buffers_.end() ? nullptr : &it->second;
  }
  bool pipeline(PipelineHandle h, PipelineRec& out) const {
    auto it = pipelines_.find(h);
    if (it == pipelines_.end()) return false;
    out = it->second;
    return true;
  }
  bool target(TargetHandle h, TargetRec& out) const {
    auto it = targets_.find(h);
    if (it == targets_.end()) return false;
    out = it->second;
    return true;
  }

private:
  void shutdownEGL();

  EGLDisplay display_ = EGL_NO_DISPLAY;
  EGLContext context_ = EGL_NO_CONTEXT;
  EGLConfig config_ = nullptr;
  EGLSurface pbuffer_ = EGL_NO_SURFACE; // 1x1，离屏渲染时保活 context
  uint32_t nextId_ = 1;
  std::unordered_map<BufferHandle, BufferRec> buffers_;
  std::unordered_map<ShaderModuleHandle, ShaderRec> shaders_;
  std::unordered_map<PipelineHandle, PipelineRec> pipelines_;
  std::unordered_map<TargetHandle, TargetRec> targets_;
  std::unordered_map<SwapChainHandle, SwapChainRec> swapChains_;
  GLESCommandBuffer cmdBuf_{this};
};

// ---------------- CommandBuffer 实现 ----------------

void GLESCommandBuffer::beginRenderPass(TargetHandle target, const ClearColor& clear) {
  TargetRec t;
  if (!device_->target(target, t)) return;
  current_ = t;
  if (!t.isSwapchain) {
    device_->ensureOffscreenCurrent();
  } // swapchain：acquire 时已 makeCurrent 到窗口 surface
  glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
  glViewport(0, 0, GLsizei(t.width), GLsizei(t.height));
  glClearColor(clear.r, clear.g, clear.b, clear.a);
  glClear(GL_COLOR_BUFFER_BIT);
  glDisable(GL_DEPTH_TEST);
}

void GLESCommandBuffer::bindPipeline(PipelineHandle pipeline) {
  PipelineRec rec;
  if (!device_->pipeline(pipeline, rec)) return;
  pipeline_ = rec;
  glUseProgram(rec.program);
  if (rec.cull == CullMode::None) {
    glDisable(GL_CULL_FACE);
  } else {
    glEnable(GL_CULL_FACE);
    glCullFace(rec.cull == CullMode::Back ? GL_BACK : GL_FRONT);
    glFrontFace(GL_CCW);
  }
}

void GLESCommandBuffer::bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) {
  vertexBuffers_[binding] = buffer;
  vertexOffsets_[binding] = offset;
}

void GLESCommandBuffer::bindIndexBuffer(BufferHandle buffer, uint64_t offset) {
  indexBuffer_ = buffer;
  indexOffset_ = offset;
}

void GLESCommandBuffer::bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset,
                                          uint64_t size) {
  const BufferRec* rec = device_->bufferRec(buffer);
  if (!rec) return;
  glBindBufferRange(GL_UNIFORM_BUFFER, slot, rec->buffer, GLintptr(offset), GLsizeiptr(size));
}

void GLESCommandBuffer::draw(uint32_t vertexCount, uint32_t firstVertex) {
  applyVertexState();
  glDrawArrays(pipeline_.topology, GLint(firstVertex), GLsizei(vertexCount));
}

void GLESCommandBuffer::drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t) {
  applyVertexState();
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, device_->buffer(indexBuffer_));
  glDrawElements(pipeline_.topology, GLsizei(indexCount), GL_UNSIGNED_SHORT,
                 reinterpret_cast<const void*>(uintptr_t(indexOffset_ + firstIndex * 2)));
}

void GLESCommandBuffer::applyVertexState() {
  for (const auto& a : pipeline_.attribs) {
    glEnableVertexAttribArray(a.location);
    glBindBuffer(GL_ARRAY_BUFFER, device_->buffer(vertexBuffers_[a.binding]));
    uint32_t stride = 0;
    for (const auto& b : pipeline_.bindings) {
      if (b.binding == a.binding) stride = b.stride;
    }
    glVertexAttribPointer(a.location, toGLAttribSize(a.format), toGLAttribType(a.format),
                          a.format == Format::RGBA8_UNORM ? GL_TRUE : GL_FALSE, GLsizei(stride),
                          reinterpret_cast<const void*>(uintptr_t(vertexOffsets_[a.binding] + a.offset)));
  }
}

// ---------------- EGL 生命周期 ----------------

bool GLESDevice::init(const DeviceDesc&) {
  display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
    RD_LOGE("rhi.gles", "eglInitialize 失败");
    return false;
  }
  const EGLint configAttrs[] = {EGL_RENDERABLE_TYPE,
                                EGL_OPENGL_ES3_BIT,
                                EGL_SURFACE_TYPE,
                                EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
                                EGL_RED_SIZE,
                                8,
                                EGL_GREEN_SIZE,
                                8,
                                EGL_BLUE_SIZE,
                                8,
                                EGL_ALPHA_SIZE,
                                8,
                                EGL_NONE};
  EGLint numConfigs = 0;
  if (!eglChooseConfig(display_, configAttrs, &config_, 1, &numConfigs) || numConfigs < 1) {
    RD_LOGE("rhi.gles", "eglChooseConfig 失败");
    return false;
  }
  const EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctxAttrs);
  if (context_ == EGL_NO_CONTEXT) {
    RD_LOGE("rhi.gles", "eglCreateContext 失败");
    return false;
  }
  const EGLint pbufAttrs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  pbuffer_ = eglCreatePbufferSurface(display_, config_, pbufAttrs);
  if (pbuffer_ == EGL_NO_SURFACE) {
    RD_LOGE("rhi.gles", "pbuffer 创建失败");
    return false;
  }
  return makeCurrent(pbuffer_);
}

bool GLESDevice::makeCurrent(EGLSurface surface) {
  return eglMakeCurrent(display_, surface, surface, context_) == EGL_TRUE;
}

void GLESDevice::ensureOffscreenCurrent() {
  EGLSurface current = eglGetCurrentSurface(EGL_DRAW);
  if (current != pbuffer_) makeCurrent(pbuffer_);
}

void GLESDevice::shutdownEGL() {
  if (display_ != EGL_NO_DISPLAY) {
    eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (pbuffer_ != EGL_NO_SURFACE) eglDestroySurface(display_, pbuffer_);
    if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
    eglTerminate(display_);
  }
}

// ---------------- 资源 ----------------

BufferHandle GLESDevice::createBuffer(const BufferDesc& desc) {
  ensureOffscreenCurrent();
  GLuint buf = 0;
  glGenBuffers(1, &buf);
  GLenum target = GL_ARRAY_BUFFER;
  if (hasFlag(desc.usage, BufferUsage::Index)) target = GL_ELEMENT_ARRAY_BUFFER;
  if (hasFlag(desc.usage, BufferUsage::Uniform)) target = GL_UNIFORM_BUFFER;
  glBindBuffer(target, buf);
  glBufferData(target, GLsizeiptr(desc.size), desc.data, GL_STATIC_DRAW);
  BufferHandle h(nextId_++);
  buffers_.emplace(h, BufferRec{buf, target, desc.size});
  return h;
}

void GLESDevice::updateBuffer(BufferHandle buffer, const void* data, uint64_t size,
                              uint64_t offset) {
  auto it = buffers_.find(buffer);
  if (it == buffers_.end()) return;
  ensureOffscreenCurrent();
  glBindBuffer(it->second.target, it->second.buffer);
  glBufferSubData(it->second.target, GLintptr(offset), GLsizeiptr(size), data);
}

void GLESDevice::destroyBuffer(BufferHandle buffer) {
  auto it = buffers_.find(buffer);
  if (it == buffers_.end()) return;
  ensureOffscreenCurrent();
  glDeleteBuffers(1, &it->second.buffer);
  buffers_.erase(it);
}

ShaderModuleHandle GLESDevice::createShaderModule(const ShaderModuleDesc& desc) {
  ensureOffscreenCurrent();
  GLenum type = desc.stage == ShaderStage::Vertex ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
  GLuint shader = glCreateShader(type);
  const GLchar* src = reinterpret_cast<const GLchar*>(desc.code.data());
  const GLint len = GLint(desc.code.size());
  glShaderSource(shader, 1, &src, &len);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char info[1024];
    glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
    RD_LOGE("rhi.gles", "shader 编译失败: %s", info);
    glDeleteShader(shader);
    return {};
  }
  ShaderModuleHandle h(nextId_++);
  shaders_.emplace(h, ShaderRec{shader, desc.stage});
  return h;
}

void GLESDevice::destroyShaderModule(ShaderModuleHandle module) {
  auto it = shaders_.find(module);
  if (it == shaders_.end()) return;
  ensureOffscreenCurrent();
  glDeleteShader(it->second.shader);
  shaders_.erase(it);
}

PipelineHandle GLESDevice::createPipeline(const PipelineDesc& desc) {
  if (desc.depthTest) {
    RD_LOGE("rhi.gles", "P0-2 不支持 depthTest（P1 引入深度附件）");
    return {};
  }
  auto vsIt = shaders_.find(desc.vertexShader);
  auto fsIt = shaders_.find(desc.fragmentShader);
  if (vsIt == shaders_.end() || fsIt == shaders_.end()) return {};
  ensureOffscreenCurrent();
  GLuint program = glCreateProgram();
  glAttachShader(program, vsIt->second.shader);
  glAttachShader(program, fsIt->second.shader);
  glLinkProgram(program);
  GLint ok = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char info[1024];
    glGetProgramInfoLog(program, sizeof(info), nullptr, info);
    RD_LOGE("rhi.gles", "program 链接失败: %s", info);
    glDeleteProgram(program);
    return {};
  }
  // uniform block "UBO" ↔ slot 0（P0 硬编码；P1 由反射 JSON 驱动）
  GLuint blockIndex = glGetUniformBlockIndex(program, "UBO");
  if (blockIndex != GL_INVALID_INDEX) {
    glUniformBlockBinding(program, blockIndex, 0);
  }
  PipelineRec rec;
  rec.program = program;
  rec.bindings = desc.vertexBindings;
  rec.attribs = desc.attributes;
  rec.topology = toGLTopology(desc.topology);
  rec.cull = desc.cullMode;
  PipelineHandle h(nextId_++);
  pipelines_.emplace(h, rec);
  return h;
}

void GLESDevice::destroyPipeline(PipelineHandle pipeline) {
  auto it = pipelines_.find(pipeline);
  if (it == pipelines_.end()) return;
  ensureOffscreenCurrent();
  glDeleteProgram(it->second.program);
  pipelines_.erase(it);
}

TargetHandle GLESDevice::createOffscreenTarget(const OffscreenTargetDesc& desc) {
  ensureOffscreenCurrent();
  TargetRec rec;
  rec.width = desc.width;
  rec.height = desc.height;
  glGenTextures(1, &rec.colorTex);
  glBindTexture(GL_TEXTURE_2D, rec.colorTex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(desc.width), GLsizei(desc.height), 0, GL_RGBA,
               GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glGenFramebuffers(1, &rec.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, rec.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rec.colorTex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    RD_LOGE("rhi.gles", "FBO 不完整");
    return {};
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  TargetHandle h(nextId_++);
  targets_.emplace(h, rec);
  return h;
}

void GLESDevice::destroyTarget(TargetHandle target) {
  auto it = targets_.find(target);
  if (it == targets_.end() || it->second.isSwapchain) return;
  ensureOffscreenCurrent();
  glDeleteFramebuffers(1, &it->second.fbo);
  glDeleteTextures(1, &it->second.colorTex);
  targets_.erase(it);
}

bool GLESDevice::readbackTarget(TargetHandle target, void* outRGBA8, uint64_t outSize) {
  auto it = targets_.find(target);
  if (it == targets_.end() || it->second.isSwapchain) return false;
  const TargetRec& t = it->second;
  const uint64_t rowBytes = uint64_t(t.width) * 4;
  if (outSize < rowBytes * t.height) return false;
  ensureOffscreenCurrent();
  glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
  std::vector<uint8_t> raw(rowBytes * t.height);
  glReadPixels(0, 0, GLsizei(t.width), GLsizei(t.height), GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  // GL 原点左下 → 翻转为顶向下
  uint8_t* out = static_cast<uint8_t*>(outRGBA8);
  for (uint32_t y = 0; y < t.height; ++y) {
    memcpy(out + rowBytes * y, raw.data() + rowBytes * (t.height - 1 - y), rowBytes);
  }
  return true;
}

void GLESDevice::submit(CommandBuffer*) {}
void GLESDevice::waitIdle() { glFinish(); }

// ---------------- SwapChain ----------------

SwapChainHandle GLESDevice::createSwapChain(void* nativeWindow, uint32_t width, uint32_t height) {
  if (!nativeWindow || width == 0 || height == 0) return {};
  ANativeWindow* window = static_cast<ANativeWindow*>(nativeWindow);
  ANativeWindow_acquire(window);
  EGLSurface surface = eglCreateWindowSurface(display_, config_, window, nullptr);
  if (surface == EGL_NO_SURFACE) {
    RD_LOGE("rhi.gles", "eglCreateWindowSurface 失败");
    ANativeWindow_release(window);
    return {};
  }
  SwapChainRec rec;
  rec.window = window;
  rec.surface = surface;
  rec.width = width;
  rec.height = height;
  SwapChainHandle h(nextId_++);
  swapChains_.emplace(h, rec);
  return h;
}

void GLESDevice::resizeSwapChain(SwapChainHandle swapChain, uint32_t width, uint32_t height) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return;
  it->second.width = width;
  it->second.height = height; // EGL surface 随窗口自动调整
}

TargetHandle GLESDevice::acquireSwapChainTarget(SwapChainHandle swapChain) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return {};
  SwapChainRec& rec = it->second;
  if (!makeCurrent(rec.surface)) return {};
  if (!rec.currentTarget.valid()) {
    TargetRec t;
    t.isSwapchain = true;
    t.fbo = 0;
    rec.currentTarget = TargetHandle(nextId_++);
    targets_.emplace(rec.currentTarget, t);
  }
  TargetRec& t = targets_[rec.currentTarget];
  t.width = rec.width;
  t.height = rec.height;
  return rec.currentTarget;
}

void GLESDevice::present(SwapChainHandle swapChain) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return;
  eglSwapBuffers(display_, it->second.surface);
}

void GLESDevice::destroySwapChain(SwapChainHandle swapChain) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return;
  if (it->second.currentTarget.valid()) targets_.erase(it->second.currentTarget);
  if (it->second.surface != EGL_NO_SURFACE) eglDestroySurface(display_, it->second.surface);
  if (it->second.window) ANativeWindow_release(it->second.window);
  swapChains_.erase(it);
}

} // namespace

std::unique_ptr<Device> createGLESDevice(const DeviceDesc& desc) {
  auto device = std::make_unique<GLESDevice>();
  if (!device->init(desc)) return nullptr;
  return device;
}

} // namespace rd
#endif // __ANDROID__
```

（类组织说明：`GLESCommandBuffer` 只有声明在 `GLESDevice` 之前，方法体在 `GLESDevice` 完整定义之后外联实现——与 Metal/Vulkan 后端同构；`cmdBuf_` 成员在 `GLESDevice` 私有区末尾。）

- [ ] **Step 2: core/CMakeLists.txt 的 Android 段补充链接库**

把 `if(ANDROID)` 块改为：
```cmake
if(ANDROID)
  target_sources(rd_core PRIVATE rhi/backends/gles/gles_device.cpp)
  target_link_libraries(rd_core PRIVATE EGL GLESv3 android log vulkan)
endif()
```
（`vulkan`/`android`/`log` 为 NDK 系统库；Vulkan swapchain 在 Task 8 启用源文件。）

同时修正 Vulkan host 段避免交叉构建误配（把 `if(RD_MOLTENVK_LIB)` 改为）：
```cmake
if(RD_MOLTENVK_LIB AND NOT ANDROID AND NOT IOS)
```

- [ ] **Step 3: 验证 host 不受影响**

```bash
cmake -S . -B build > /dev/null 2>&1 && cmake --build build -j8 2>&1 | grep error | head -3
ctest --test-dir build 2>&1 | grep "tests passed"
```
预期：全绿（GLES 源不参与 host 编译）。

- [ ] **Step 4: Commit**

```bash
git add core/rhi/backends/gles/ core/CMakeLists.txt
git commit -m "feat(rhi/gles): GLES3 完整后端（EGL/FBO/上屏 swapchain），Android 编译"
```

---

### Task 7: img_check 截图校验工具

**Files:**
- Create: `tools/img_check/main.cpp`
- Create: `tools/img_check/CMakeLists.txt`
- Modify: `CMakeLists.txt`（host 分支追加 `add_subdirectory(tools/img_check)`）

- [ ] **Step 1: 写 img_check**

`tools/img_check/main.cpp`:
```cpp
// 截图结构校验：验证"确实渲染出了彩色立方体"，跨设备/分辨率稳健（不逐像素比对）。
// 用法: img_check <png> [--bg r,g,b] [--min-coverage 0.05]
// 检查：1) 去重采样色数 > 32（非单色/花屏）  2) 可选：四角为背景色  3) 非背景像素占比 >= 阈值
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include "common/image.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "用法: img_check <png> [--bg r,g,b] [--min-coverage 0.05]\n");
    return 2;
  }
  const char* path = argv[1];
  bool checkBg = false;
  int bgR = 26, bgG = 26, bgB = 31;
  double minCoverage = 0.05;
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--bg") && i + 1 < argc) {
      checkBg = sscanf(argv[++i], "%d,%d,%d", &bgR, &bgG, &bgB) == 3;
    } else if (!strcmp(argv[i], "--min-coverage") && i + 1 < argc) {
      minCoverage = atof(argv[++i]);
    }
  }

  auto img = rd::test::loadPNG(path);
  if (img.pixels.empty()) {
    fprintf(stderr, "FAIL: 无法加载 %s\n", path);
    return 1;
  }

  std::set<uint32_t> colors;
  for (size_t i = 0; i + 3 < img.pixels.size(); i += 64) { // 每 16 像素采样
    colors.insert((img.pixels[i] << 16) | (img.pixels[i + 1] << 8) | img.pixels[i + 2]);
  }
  if (colors.size() <= 32) {
    fprintf(stderr, "FAIL: 颜色数 %zu <= 32（疑似单色/无渲染）\n", colors.size());
    return 1;
  }

  auto nearColor = [&](size_t idx, int r, int g, int b, int tol) {
    return abs(int(img.pixels[idx]) - r) <= tol && abs(int(img.pixels[idx + 1]) - g) <= tol &&
           abs(int(img.pixels[idx + 2]) - b) <= tol;
  };
  if (checkBg) {
    const size_t w = img.width, h = img.height;
    const size_t corners[4] = {0, (w - 1) * 4, (h - 1) * w * 4, ((h - 1) * w + w - 1) * 4};
    for (size_t c : corners) {
      if (!nearColor(c, bgR, bgG, bgB, 8)) {
        fprintf(stderr, "FAIL: 角落像素非背景色 (%d,%d,%d)\n", img.pixels[c],
                img.pixels[c + 1], img.pixels[c + 2]);
        return 1;
      }
    }
  }

  uint64_t covered = 0;
  for (size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
    if (!nearColor(i, bgR, bgG, bgB, 8)) ++covered;
  }
  double coverage = double(covered) / (double(img.width) * img.height);
  printf("img_check: %ux%u, 采样色数=%zu, 覆盖率=%.3f\n", img.width, img.height, colors.size(),
         coverage);
  if (coverage < minCoverage) {
    fprintf(stderr, "FAIL: 覆盖率 %.3f < %.3f\n", coverage, minCoverage);
    return 1;
  }
  printf("PASS\n");
  return 0;
}
```

`tools/img_check/CMakeLists.txt`:
```cmake
add_executable(img_check
  main.cpp
  ${CMAKE_SOURCE_DIR}/tests/common/image.cpp
)
target_include_directories(img_check PRIVATE
  ${CMAKE_SOURCE_DIR}/tests
  ${stb_SOURCE_DIR})
```

顶层 `CMakeLists.txt` host 分支（`add_subdirectory(tools/render_test)` 之后）追加：
```cmake
  add_subdirectory(tools/img_check)
```

- [ ] **Step 2: 构建并用 golden 验证（正/反两例）**

```bash
cmake -S . -B build > /dev/null 2>&1 && cmake --build build -j8 2>&1 | grep error | head -3
./build/tools/img_check/img_check tests/golden/cube_metal.png --bg 26,26,31; echo "exit=$?"
python3 -c "
import struct, zlib
# 生成 64x64 纯黑 PNG 作为反例
w=h=64
raw=b''.join(b'\x00'+b'\x00\x00\x00\xff'*w for _ in range(h))
def chunk(t,d):
    c=t+d; return struct.pack('>I',len(d))+c+struct.pack('>I',zlib.crc32(c))
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b'')
open('/tmp/solid.png','wb').write(png)
"
./build/tools/img_check/img_check /tmp/solid.png; echo "exit=$?"
```
预期：正例 `PASS`，exit=0；反例 `FAIL: 颜色数 ... <= 32`，exit=1。

- [ ] **Step 3: Commit**

```bash
git add tools/img_check/ CMakeLists.txt
git commit -m "test(tools): img_check 截图结构校验工具"
```

---

### Task 8: Vulkan Android SwapChain

**Files:**
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`
- Modify: `core/CMakeLists.txt`（Android Vulkan 段）
- Modify: `cmake/Deps.cmake`（平台构建只取必要依赖）

**说明**：运行时验证在 Task 12。同步策略简化：acquire 用 fence 阻塞，present 前 `waitIdle`（P0 帧串行化；P1 引入信号量并行化）。

- [ ] **Step 1: Deps.cmake 平台保护**

`cmake/Deps.cmake` 全文替换为：
```cmake
include(FetchContent)

FetchContent_Declare(glm
  URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz)
FetchContent_MakeAvailable(glm)

if(ANDROID)
  FetchContent_Declare(VulkanHeaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz)
  FetchContent_MakeAvailable(VulkanHeaders)
endif()

if(NOT ANDROID AND NOT IOS)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE) # 避免依赖 SPIRV-Tools
  set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_JS OFF CACHE BOOL "" FORCE)
  set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glslang
    URL https://github.com/KhronosGroup/glslang/archive/refs/tags/14.3.0.tar.gz)
  FetchContent_MakeAvailable(glslang)

  set(SPIRV_CROSS_CLI ON CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_MSL ON CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_CPP ON CACHE BOOL "" FORCE) # CLI 依赖
  set(SPIRV_CROSS_ENABLE_REFLECT ON CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_UTIL ON CACHE BOOL "" FORCE) # CLI 依赖
  FetchContent_Declare(spirv-cross
    URL https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz)
  FetchContent_MakeAvailable(spirv-cross)

  FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/refs/heads/master.tar.gz)
  FetchContent_MakeAvailable(stb)

  FetchContent_Declare(VulkanHeaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz)
  FetchContent_MakeAvailable(VulkanHeaders)
endif()
```

- [ ] **Step 2: core/CMakeLists.txt 追加 Android Vulkan 段**

在 `if(ANDROID)`（GLES）块之后追加：
```cmake
if(ANDROID)
  target_sources(rd_core PRIVATE rhi/backends/vulkan/vulkan_device.cpp)
  target_compile_definitions(rd_core PUBLIC RD_WITH_VULKAN=1)
  target_link_libraries(rd_core PRIVATE Vulkan::Headers)
endif()
```

- [ ] **Step 3: vulkan_device.cpp 实现 Android swapchain**

头部 include 区追加：
```cpp
#if defined(__ANDROID__)
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>
#endif
```

`init()` 中 instance 扩展段（`VkInstanceCreateFlags flags = 0;` 与直连 MoltenVK 注释之后）追加：
```cpp
#if defined(__ANDROID__)
  extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
  extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif
```

`init()` 中设备扩展段（`#if defined(__APPLE__) ... #endif` 之后）追加：
```cpp
#if defined(__ANDROID__)
  deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
#endif
```

`TargetRec` 结构体追加成员：
```cpp
  bool isSwapchain = false; // swapchain 图像：资源由 swapchain 管理
```

`SwapChainRec` 结构体（放在 `TargetRec` 之后）：
```cpp
struct SwapChainRec {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  std::vector<TargetHandle> imageTargets;
  uint32_t width = 0, height = 0;
  uint32_t currentIndex = 0;
#if defined(__ANDROID__)
  ANativeWindow* window = nullptr;
#endif
};
```

`VulkanDevice` 私有区追加成员与声明：
```cpp
  std::unordered_map<SwapChainHandle, SwapChainRec> swapChains_;
  VkFence acquireFence_ = VK_NULL_HANDLE;
  VkFormat renderPassFormat_ = VK_FORMAT_R8G8B8A8_UNORM;

  bool createRenderPass(VkFormat format);
  bool createSwapchainObject(SwapChainRec& rec, VkSwapchainKHR oldSwapchain);
  bool buildSwapChainTargets(SwapChainRec& rec);
  void destroySwapChainImages(SwapChainRec& rec);
```

`init()` 中 render pass 创建段（`// 离屏 render pass ...` 整段 `VK_CHECK(vkCreateRenderPass...)` 之前的内容保留，但把最终创建调用改为使用成员函数）：将 render pass 创建整块替换为调用：
```cpp
  if (!createRenderPass(VK_FORMAT_R8G8B8A8_UNORM)) return false;
```
并把原 render pass 创建代码移入新成员函数（`format` 参数化，`color.format = format;`，成功时记录 `renderPassFormat_ = format;`）：
```cpp
bool VulkanDevice::createRenderPass(VkFormat format) {
  VkAttachmentDescription color{};
  color.format = format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;

  VkSubpassDependency deps[2]{};
  deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  deps[0].dstSubpass = 0;
  deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].srcSubpass = 0;
  deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

  VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = 1;
  rpci.pAttachments = &color;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &subpass;
  rpci.dependencyCount = 2;
  rpci.pDependencies = deps;
  VK_CHECK(vkCreateRenderPass(device_, &rpci, nullptr, &renderPass_));
  renderPassFormat_ = format;
  return true;
}
```

`init()` 末尾（`vkAllocateDescriptorSets` 成功之后 `return true;` 之前）追加：
```cpp
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VK_CHECK(vkCreateFence(device_, &fci, nullptr, &acquireFence_));
```
析构函数在 `vkDestroyCommandPool(...)` 之后追加：
```cpp
  if (acquireFence_) vkDestroyFence(device_, acquireFence_, nullptr);
```

`readbackTarget` 在 `const TargetRec& t = it->second;` 之后追加防护：
```cpp
  if (t.isSwapchain) {
    RD_LOGW("rhi.vk", "swapchain target 不支持 readback");
    return false;
  }
```

`destroyTarget` 在 `auto it = targets_.find(target); if (it == targets_.end()) return;` 之后追加：
```cpp
  if (it->second.isSwapchain) { // 资源随 swapchain 销毁
    targets_.erase(it);
    return;
  }
```

Task 2 的桩函数整体替换为：
```cpp
// ---------------- SwapChain（Android）----------------

bool VulkanDevice::createSwapchainObject(SwapChainRec& rec, VkSwapchainKHR oldSwapchain) {
  VkSurfaceCapabilitiesKHR caps;
  VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, rec.surface, &caps));

  VkExtent2D extent = caps.currentExtent.width != UINT32_MAX
                          ? caps.currentExtent
                          : VkExtent2D{rec.width, rec.height};
  uint32_t imageCount = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

  VkSwapchainCreateInfoKHR swci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  swci.surface = rec.surface;
  swci.minImageCount = imageCount;
  swci.imageFormat = renderPassFormat_;
  swci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  swci.imageExtent = extent;
  swci.imageArrayLayers = 1;
  swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  swci.preTransform = caps.currentTransform;
  swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  swci.clipped = VK_TRUE;
  swci.oldSwapchain = oldSwapchain;
  VkSwapchainKHR swapchain;
  VK_CHECK(vkCreateSwapchainKHR(device_, &swci, nullptr, &swapchain));
  if (oldSwapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, oldSwapchain, nullptr);
  rec.swapchain = swapchain;
  rec.width = extent.width;
  rec.height = extent.height;
  return true;
}

bool VulkanDevice::buildSwapChainTargets(SwapChainRec& rec) {
  uint32_t count = 0;
  vkGetSwapchainImagesKHR(device_, rec.swapchain, &count, nullptr);
  std::vector<VkImage> images(count);
  vkGetSwapchainImagesKHR(device_, rec.swapchain, &count, images.data());
  for (auto image : images) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = renderPassFormat_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view;
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &view));
    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbci.renderPass = renderPass_;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &view;
    fbci.width = rec.width;
    fbci.height = rec.height;
    fbci.layers = 1;
    VkFramebuffer fb;
    VK_CHECK(vkCreateFramebuffer(device_, &fbci, nullptr, &fb));
    TargetRec t{};
    t.color = image;
    t.view = view;
    t.fb = fb;
    t.width = rec.width;
    t.height = rec.height;
    t.isSwapchain = true;
    TargetHandle th(nextId_++);
    targets_.emplace(th, t);
    rec.imageTargets.push_back(th);
  }
  return true;
}

void VulkanDevice::destroySwapChainImages(SwapChainRec& rec) {
  for (auto th : rec.imageTargets) {
    auto it = targets_.find(th);
    if (it == targets_.end()) continue;
    vkDestroyFramebuffer(device_, it->second.fb, nullptr);
    vkDestroyImageView(device_, it->second.view, nullptr);
    targets_.erase(it);
  }
  rec.imageTargets.clear();
}

SwapChainHandle VulkanDevice::createSwapChain(void* nativeWindow, uint32_t width, uint32_t height) {
#if defined(__ANDROID__)
  if (!nativeWindow || width == 0 || height == 0) return {};
  ANativeWindow* window = static_cast<ANativeWindow*>(nativeWindow);
  ANativeWindow_acquire(window);

  VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
  sci.window = window;
  VkSurfaceKHR surface;
  if (vkCreateAndroidSurfaceKHR(instance_, &sci, nullptr, &surface) != VK_SUCCESS) {
    RD_LOGE("rhi.vk", "Android surface 创建失败");
    ANativeWindow_release(window);
    return {};
  }
  VkBool32 presentSupport = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(phys_, queueFamily_, surface, &presentSupport);
  if (!presentSupport) {
    RD_LOGE("rhi.vk", "队列不支持 present");
    vkDestroySurfaceKHR(instance_, surface, nullptr);
    ANativeWindow_release(window);
    return {};
  }

  // 表面格式须与 render pass 一致；不一致时重建 render pass（须在创建 pipeline 之前，
  // engine 流程保证：set_surface 先于场景初始化）
  uint32_t formatCount = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface, &formatCount, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(formatCount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface, &formatCount, formats.data());
  VkFormat surfaceFormat = formats.empty() ? VK_FORMAT_R8G8B8A8_UNORM : formats[0].format;
  for (const auto& f : formats) {
    if (f.format == VK_FORMAT_R8G8B8A8_UNORM) {
      surfaceFormat = f.format;
      break;
    }
  }
  if (surfaceFormat != renderPassFormat_) {
    RD_LOGI("rhi.vk", "swapchain 格式 %d ≠ render pass 格式 %d，重建 render pass",
            int(surfaceFormat), int(renderPassFormat_));
    vkDestroyRenderPass(device_, renderPass_, nullptr);
    if (!createRenderPass(surfaceFormat)) {
      vkDestroySurfaceKHR(instance_, surface, nullptr);
      ANativeWindow_release(window);
      return {};
    }
  }

  SwapChainRec rec;
  rec.surface = surface;
  rec.width = width;
  rec.height = height;
  rec.window = window;
  if (!createSwapchainObject(rec, VK_NULL_HANDLE) || !buildSwapChainTargets(rec)) {
    vkDestroySurfaceKHR(instance_, surface, nullptr);
    ANativeWindow_release(window);
    return {};
  }
  SwapChainHandle h(nextId_++);
  swapChains_.emplace(h, rec);
  return h;
#else
  (void)nativeWindow; (void)width; (void)height;
  return {};
#endif
}

void VulkanDevice::resizeSwapChain(SwapChainHandle swapChain, uint32_t width, uint32_t height) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end() || width == 0 || height == 0) return;
  vkDeviceWaitIdle(device_);
  SwapChainRec& rec = it->second;
  destroySwapChainImages(rec);
  rec.width = width;
  rec.height = height;
  createSwapchainObject(rec, rec.swapchain); // oldSwapchain 传入并销毁
  buildSwapChainTargets(rec);
}

TargetHandle VulkanDevice::acquireSwapChainTarget(SwapChainHandle swapChain) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return {};
  SwapChainRec& rec = it->second;
  VkResult r = vkAcquireNextImageKHR(device_, rec.swapchain, UINT64_MAX, VK_NULL_HANDLE,
                                     acquireFence_, &rec.currentIndex);
  vkWaitForFences(device_, 1, &acquireFence_, VK_TRUE, UINT64_MAX);
  vkResetFences(device_, 1, &acquireFence_);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) return {}; // 等 resize 后重建
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return {};
  return rec.imageTargets[rec.currentIndex];
}

void VulkanDevice::present(SwapChainHandle swapChain) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return;
  SwapChainRec& rec = it->second;
  waitIdle(); // P0 简化：帧串行；P1 引入 acquire/present 信号量
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.swapchainCount = 1;
  pi.pSwapchains = &rec.swapchain;
  pi.pImageIndices = &rec.currentIndex;
  vkQueuePresentKHR(queue_, &pi);
}

void VulkanDevice::destroySwapChain(SwapChainHandle swapChain) {
  auto it = swapChains_.find(swapChain);
  if (it == swapChains_.end()) return;
  vkDeviceWaitIdle(device_);
  SwapChainRec& rec = it->second;
  destroySwapChainImages(rec);
  if (rec.swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(device_, rec.swapchain, nullptr);
  if (rec.surface != VK_NULL_HANDLE) vkDestroySurfaceKHR(instance_, rec.surface, nullptr);
#if defined(__ANDROID__)
  if (rec.window) ANativeWindow_release(rec.window);
#endif
  swapChains_.erase(it);
}
```

注意：`VK_CHECK` 宏在以上新函数内使用 `return false` —— `createSwapchainObject`/`buildSwapChainTargets` 返回 bool 匹配；`createRenderPass` 同理。这些函数的实现要放在 `createVulkanDevice` 之前。

- [ ] **Step 4: 验证 host 构建与测试（桩路径不受影响）**

```bash
cmake -S . -B build > /dev/null 2>&1 && cmake --build build -j8 2>&1 | grep error | head -3
ctest --test-dir build 2>&1 | grep "tests passed"
```
预期：全绿（host 无 ANDROID 宏，swapchain 函数走 `#else return {}`）。

- [ ] **Step 5: Commit**

```bash
git add core/rhi/backends/vulkan/vulkan_device.cpp core/CMakeLists.txt cmake/Deps.cmake
git commit -m "feat(rhi/vulkan): Android surface/swapchain（fence acquire + 格式自适应 render pass）"
```

---

### Task 9: C API（rd_engine）

**Files:**
- Create: `core/api/rd_api.h`
- Create: `core/api/rd_api.cpp`
- Test: `tests/api/api_test.cpp`
- Modify: `core/CMakeLists.txt`（源列表追加 `api/rd_api.cpp`）
- Modify: `tests/CMakeLists.txt`（源列表追加 `api/api_test.cpp`）

- [ ] **Step 1: 写失败测试**

`tests/api/api_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "api/rd_api.h"

TEST(Api, CreateDestroyMetal) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  rd_engine_destroy(e);
}

TEST(Api, RenderWithoutSurfaceIsSafe) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  rd_engine_render_frame(e, 0.016f); // 无 surface：安全 no-op
  rd_engine_resize(e, 100, 100);
  rd_engine_clear_surface(e);
  rd_engine_destroy(e);
}

TEST(Api, InvalidArgsRejected) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(rd_engine_set_surface(e, nullptr, 512, 512), RD_ERROR_INVALID_ARG);
  EXPECT_EQ(rd_engine_set_surface(e, reinterpret_cast<void*>(1), 0, 512), RD_ERROR_INVALID_ARG);
  rd_engine_destroy(e);
}

TEST(Api, GlesUnavailableOnHost) {
  EXPECT_EQ(rd_engine_create(RD_BACKEND_GLES), nullptr);
}
```

`tests/CMakeLists.txt` 源列表追加 `api/api_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

```bash
cmake -S . -B build > /dev/null 2>&1 && cmake --build build -j8 2>&1 | grep -E "fatal error" | head -2
```
预期：`api/rd_api.h file not found`。

- [ ] **Step 3: 实现 rd_api.h / rd_api.cpp**

`core/api/rd_api.h`:
```cpp
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 线程约定：同一 engine 的所有调用必须在同一线程（平台层的"渲染线程"）。
//   Android：RenderView 的专用渲染线程；iOS：主线程（MTKView 惯例）。
typedef struct rd_engine rd_engine;

typedef enum rd_backend {
  RD_BACKEND_VULKAN = 0,
  RD_BACKEND_METAL = 1,
  RD_BACKEND_GLES = 2,
} rd_backend_t;

typedef enum rd_result {
  RD_OK = 0,
  RD_ERROR_INVALID_ARG = 1,
  RD_ERROR_SURFACE = 2,
  RD_ERROR_SHADER = 3,
  RD_ERROR_SCENE = 4,
} rd_result_t;

// 创建失败返回 nullptr（细节见日志）。
rd_engine* rd_engine_create(rd_backend_t backend);
void rd_engine_destroy(rd_engine* engine);

// native_window：Android=ANativeWindow*，iOS=CAMetalLayer*。
// 重复调用会销毁旧 swapchain 再创建。engine 内部会 retain 窗口句柄。
rd_result_t rd_engine_set_surface(rd_engine* engine, void* native_window, uint32_t width,
                                  uint32_t height);
void rd_engine_clear_surface(rd_engine* engine);
void rd_engine_resize(rd_engine* engine, uint32_t width, uint32_t height);

// 渲染一帧。无 surface 时为安全 no-op。dt 单位秒，驱动演示旋转。
void rd_engine_render_frame(rd_engine* engine, float dt_seconds);

const char* rd_get_last_error(rd_engine* engine);

#ifdef __cplusplus
}
#endif
```

`core/api/rd_api.cpp`:
```cpp
#include "api/rd_api.h"
#include "api/embedded_shaders.h"
#include "foundation/log.h"
#include "rhi/rhi_device.h"
#include "scene/cube_scene.h"
#include <cstring>
#include <memory>

struct rd_engine {
  std::unique_ptr<rd::Device> device;
  rd::SwapChainHandle swapChain;
  rd::demo::CubeScene scene;
  bool sceneReady = false;
  uint32_t width = 0, height = 0;
  float angle = 0.0f;
  char lastError[256] = {};
};

namespace {
void setError(rd_engine* e, const char* msg) {
  std::strncpy(e->lastError, msg, sizeof(e->lastError) - 1);
  e->lastError[sizeof(e->lastError) - 1] = '\0';
  RD_LOGE("api", "%s", msg);
}
} // namespace

rd_engine* rd_engine_create(rd_backend_t backend) {
  rd::DeviceDesc desc;
  desc.backend = static_cast<rd::Backend>(backend);
  auto device = rd::createDevice(desc);
  if (!device) return nullptr;
  auto* e = new rd_engine();
  e->device = std::move(device);
  return e;
}

void rd_engine_destroy(rd_engine* e) {
  if (!e) return;
  if (e->device) {
    e->device->waitIdle();
    if (e->sceneReady) e->scene.shutdown(*e->device);
    if (e->swapChain.valid()) e->device->destroySwapChain(e->swapChain);
  }
  delete e;
}

rd_result_t rd_engine_set_surface(rd_engine* e, void* nativeWindow, uint32_t width,
                                  uint32_t height) {
  if (!e || !nativeWindow || width == 0 || height == 0) return RD_ERROR_INVALID_ARG;
  if (e->swapChain.valid()) {
    e->device->waitIdle();
    e->device->destroySwapChain(e->swapChain);
    e->swapChain = rd::SwapChainHandle();
  }
  e->swapChain = e->device->createSwapChain(nativeWindow, width, height);
  if (!e->swapChain.valid()) {
    setError(e, "createSwapChain 失败");
    return RD_ERROR_SURFACE;
  }
  e->width = width;
  e->height = height;
  if (!e->sceneReady) {
    const uint8_t* vs = nullptr;
    const uint8_t* fs = nullptr;
    size_t vsSize = 0, fsSize = 0;
    rd::Backend b = e->device->backend();
    if (!rd::embeddedCubeShader(b, rd::ShaderStage::Vertex, &vs, &vsSize) ||
        !rd::embeddedCubeShader(b, rd::ShaderStage::Fragment, &fs, &fsSize)) {
      setError(e, "内嵌 shader 缺失");
      return RD_ERROR_SHADER;
    }
    const char* entry = (b == rd::Backend::Metal) ? "main0" : "main";
    if (!e->scene.init(*e->device, vs, vsSize, fs, fsSize, entry)) {
      setError(e, "场景初始化失败");
      return RD_ERROR_SCENE;
    }
    e->sceneReady = true;
  }
  return RD_OK;
}

void rd_engine_clear_surface(rd_engine* e) {
  if (!e || !e->swapChain.valid()) return;
  e->device->waitIdle();
  e->device->destroySwapChain(e->swapChain);
  e->swapChain = rd::SwapChainHandle();
}

void rd_engine_resize(rd_engine* e, uint32_t width, uint32_t height) {
  if (!e || !e->swapChain.valid() || width == 0 || height == 0) return;
  e->width = width;
  e->height = height;
  e->device->resizeSwapChain(e->swapChain, width, height);
}

void rd_engine_render_frame(rd_engine* e, float dt) {
  if (!e || !e->swapChain.valid() || !e->sceneReady) return;
  e->angle += dt * 1.0f;
  rd::TargetHandle target = e->device->acquireSwapChainTarget(e->swapChain);
  if (!target.valid()) return; // 表面重建中，跳过本帧
  e->scene.render(*e->device, target, e->width, e->height, e->angle);
  e->device->present(e->swapChain);
}

const char* rd_get_last_error(rd_engine* e) { return e ? e->lastError : ""; }
```

`core/CMakeLists.txt` 源列表追加 `api/rd_api.cpp`。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build -j8 2>&1 | grep error | head -3; ctest --test-dir build --output-on-failure -R Api
```
预期：4 个 Api 测试全过；全量 `ctest --test-dir build` 29/29 绿。

- [ ] **Step 5: Commit**

```bash
git add core/api/rd_api.h core/api/rd_api.cpp core/CMakeLists.txt tests/api/ tests/CMakeLists.txt
git commit -m "feat(api): rd_engine C API（create/surface/resize/render_frame）"
```

---

### Task 10: Android 平台库（RenderView + JNI + Gradle 接入）

**Files:**
- Create: `platform/android/CMakeLists.txt`
- Create: `platform/android/build.gradle`
- Create: `platform/android/src/main/AndroidManifest.xml`
- Create: `platform/android/src/main/java/com/rd/renderer/RenderView.kt`
- Create: `platform/android/jni/rd_jni.cpp`
- Modify: `CMakeLists.txt`（ANDROID 分支 add_subdirectory）

- [ ] **Step 1: 顶层 CMakeLists 接入 Android**

顶层 `CMakeLists.txt` 的子目录段改为：
```cmake
enable_testing()
add_subdirectory(core)
if(ANDROID)
  add_subdirectory(platform/android)
elseif(IOS)
  add_subdirectory(samples/ios)
else()
  add_subdirectory(shaders)
  add_subdirectory(tests)
  add_subdirectory(tools/render_test)
  add_subdirectory(tools/img_check)
endif()
```

- [ ] **Step 2: 写 platform/android/CMakeLists.txt 与 JNI**

`platform/android/CMakeLists.txt`:
```cmake
add_library(rd_jni SHARED jni/rd_jni.cpp)
target_link_libraries(rd_jni PRIVATE rd_core)
```

`platform/android/jni/rd_jni.cpp`:
```cpp
#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include "api/rd_api.h"

extern "C" {

JNIEXPORT jlong JNICALL Java_com_rd_renderer_RenderView_nativeCreate(JNIEnv*, jobject,
                                                                     jint backend) {
  return reinterpret_cast<jlong>(rd_engine_create(static_cast<rd_backend_t>(backend)));
}

JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeDestroy(JNIEnv*, jobject, jlong ptr) {
  rd_engine_destroy(reinterpret_cast<rd_engine*>(ptr));
}

JNIEXPORT jint JNICALL Java_com_rd_renderer_RenderView_nativeSetSurface(JNIEnv* env, jobject,
                                                                        jlong ptr, jobject surface,
                                                                        jint w, jint h) {
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (!window) return RD_ERROR_INVALID_ARG;
  jint result = rd_engine_set_surface(reinterpret_cast<rd_engine*>(ptr), window,
                                      static_cast<uint32_t>(w), static_cast<uint32_t>(h));
  ANativeWindow_release(window); // engine 内部已 acquire 自己的引用
  return result;
}

JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeClearSurface(JNIEnv*, jobject,
                                                                          jlong ptr) {
  rd_engine_clear_surface(reinterpret_cast<rd_engine*>(ptr));
}

JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeResize(JNIEnv*, jobject, jlong ptr,
                                                                    jint w, jint h) {
  rd_engine_resize(reinterpret_cast<rd_engine*>(ptr), static_cast<uint32_t>(w),
                   static_cast<uint32_t>(h));
}

JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeRenderFrame(JNIEnv*, jobject,
                                                                         jlong ptr, jfloat dt) {
  rd_engine_render_frame(reinterpret_cast<rd_engine*>(ptr), dt);
}

} // extern "C"
```

- [ ] **Step 3: 写 RenderView.kt 与模块文件**

`platform/android/src/main/java/com/rd/renderer/RenderView.kt`:
```kotlin
package com.rd.renderer

import android.content.Context
import android.os.Handler
import android.os.HandlerThread
import android.util.AttributeSet
import android.view.Choreographer
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * 3D 渲染视图。所有 engine 调用都在专用渲染线程（满足 rd_engine 单线程约定）。
 * 用法：setBackend() → 加入布局即可；surface 生命周期全自动。
 */
class RenderView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
) : SurfaceView(context, attrs), SurfaceHolder.Callback {

    enum class Backend(val value: Int) { VULKAN(0), GLES(2) }

    private var renderThread: HandlerThread? = null // 每次 surfaceCreated 新建（quit 后不可复用）
    private lateinit var renderHandler: Handler
    private var enginePtr: Long = 0
    private var backend = Backend.GLES
    private var running = false
    private var lastFrameNanos = 0L

    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!running) return
            val dt = if (lastFrameNanos == 0L) 1f / 60f
            else (frameTimeNanos - lastFrameNanos) / 1_000_000_000f
            lastFrameNanos = frameTimeNanos
            if (enginePtr != 0L) nativeRenderFrame(enginePtr, dt)
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    init {
        holder.addCallback(this)
    }

    fun setBackend(value: Backend) {
        check(enginePtr == 0L) { "setBackend 须在 surface 就绪前调用" }
        backend = value
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        val thread = HandlerThread("rd-render")
        thread.start()
        renderThread = thread
        renderHandler = Handler(thread.looper)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        val surface = holder.surface
        renderHandler.post {
            if (enginePtr == 0L) {
                enginePtr = nativeCreate(backend.value)
                if (enginePtr != 0L &&
                    nativeSetSurface(enginePtr, surface, width, height) == 0) {
                    running = true
                    lastFrameNanos = 0L
                    Choreographer.getInstance().postFrameCallback(frameCallback)
                }
            } else {
                nativeResize(enginePtr, width, height)
            }
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        renderHandler.post {
            running = false
            Choreographer.getInstance().removeFrameCallback(frameCallback)
            if (enginePtr != 0L) {
                nativeClearSurface(enginePtr)
                nativeDestroy(enginePtr)
                enginePtr = 0L
            }
            renderThread?.quitSafely()
            renderThread = null
        }
    }

    private external fun nativeCreate(backend: Int): Long
    private external fun nativeDestroy(ptr: Long)
    private external fun nativeSetSurface(ptr: Long, surface: Surface, width: Int, height: Int): Int
    private external fun nativeClearSurface(ptr: Long)
    private external fun nativeResize(ptr: Long, width: Int, height: Int)
    private external fun nativeRenderFrame(ptr: Long, dt: Float)

    companion object {
        init { System.loadLibrary("rd_jni") }
    }
}
```

`platform/android/build.gradle`:
```groovy
plugins {
    id 'com.android.library'
    id 'org.jetbrains.kotlin.android'
}

android {
    namespace 'com.rd.renderer'
    compileSdk 34
    defaultConfig { minSdk 26 }
}
```

`platform/android/src/main/AndroidManifest.xml`:
```xml
<?xml version="1.0" encoding="utf-8"?>
<manifest />
```

- [ ] **Step 4: Commit（编译验证合并到 Task 11 的 assembleDebug）**

```bash
git add CMakeLists.txt platform/android/
git commit -m "feat(platform/android): RenderView（渲染线程 + Choreographer 驱动）+ JNI 桥"
```

---

### Task 11: Android sample app 与 assembleDebug 构建

**Files:**
- Create: `samples/android/settings.gradle`
- Create: `samples/android/build.gradle`
- Create: `samples/android/gradle.properties`
- Create: `samples/android/app/build.gradle`
- Create: `samples/android/app/src/main/AndroidManifest.xml`
- Create: `samples/android/app/src/main/java/com/rd/sample/MainActivity.kt`

- [ ] **Step 1: 写 Gradle 工程文件**

`samples/android/settings.gradle`:
```groovy
pluginManagement {
    repositories { google(); mavenCentral(); gradlePluginPortal() }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories { google(); mavenCentral() }
}
rootProject.name = 'RdSamples'
include ':app'
include ':renderer'
project(':renderer').projectDir = new File(rootDir, '../../platform/android')
```

`samples/android/build.gradle`:
```groovy
plugins {
    id 'com.android.application' version '8.5.2' apply false
    id 'com.android.library' version '8.5.2' apply false
    id 'org.jetbrains.kotlin.android' version '2.0.20' apply false
}
```

`samples/android/gradle.properties`:
```properties
org.gradle.jvmargs=-Xmx4g
android.nonTransitiveRClass=true
```

`samples/android/app/build.gradle`:
```groovy
plugins {
    id 'com.android.application'
    id 'org.jetbrains.kotlin.android'
}

android {
    namespace 'com.rd.sample'
    compileSdk 34
    defaultConfig {
        applicationId 'com.rd.sample'
        minSdk 26
        targetSdk 34
        externalNativeBuild {
            cmake {
                targets 'rd_jni'
                arguments "-DRD_HOST_SHADER_OUT=${rootDir}/../../build/shaders_out".toString()
            }
        }
    }
    externalNativeBuild {
        cmake { path file('../../../CMakeLists.txt') }
    }
}

dependencies {
    implementation project(':renderer')
}
```

`samples/android/app/src/main/AndroidManifest.xml`:
```xml
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android">
    <application
        android:label="RdDemo"
        android:theme="@android:style/Theme.NoTitleBar.Fullscreen">
        <activity android:name=".MainActivity" android:exported="true">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
```

`samples/android/app/src/main/java/com/rd/sample/MainActivity.kt`:
```kotlin
package com.rd.sample

import android.app.Activity
import android.os.Bundle
import android.widget.FrameLayout
import com.rd.renderer.RenderView

class MainActivity : Activity() {
    private lateinit var renderView: RenderView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val backend = if (intent.getIntExtra("backend", 2) == 0)
            RenderView.Backend.VULKAN else RenderView.Backend.GLES
        renderView = RenderView(this)
        renderView.setBackend(backend)
        setContentView(
            renderView,
            FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT,
            ),
        )
    }
}
```

- [ ] **Step 2: 生成 wrapper 并构建 APK**

```bash
source /tmp/rd_env.sh
./scripts/check.sh   # 先确保 host 构建存在（shaders_out 是内嵌生成的输入）
cd samples/android
gradle wrapper --gradle-version 8.9
./gradlew :app:assembleDebug 2>&1 | tail -5
```
预期：`BUILD SUCCESSFUL`；APK 在 `samples/android/app/build/outputs/apk/debug/app-debug.apk`。
（首次构建会下载 AGP/Kotlin 依赖与编译 rd_core/rd_jni，耗时数分钟。）

- [ ] **Step 3: Commit**

```bash
cd /Users/meetyou/Desktop/3DRender
git add samples/android/
git commit -m "feat(samples/android): demo app（backend 由 intent extra 选择）"
```

---

### Task 12: Android 模拟器双后端截图验证 + CI

**Files:**
- Modify: `.github/workflows/ci.yml`（追加 android-build job）

- [ ] **Step 1: 启动模拟器并安装**

```bash
source /tmp/rd_env.sh
emulator -avd rdtest -no-window -gpu swiftshader_indirect -no-snapshot-save -no-audio &
adb wait-for-device
adb shell getprop sys.boot_completed   # 等到输出 1
adb install -r samples/android/app/build/outputs/apk/debug/app-debug.apk
```

- [ ] **Step 2: GLES 后端截图验证**

```bash
source /tmp/rd_env.sh
adb shell am start -n com.rd.sample/.MainActivity --ei backend 2
sleep 4
adb exec-out screencap -p > /tmp/android_gles.png
./build/tools/img_check/img_check /tmp/android_gles.png --min-coverage 0.03
```
预期：输出 `img_check: ... 覆盖率=...` + `PASS`（exit 0）。
注：模拟器含状态栏，故不用 `--bg` 角点检查；覆盖率阈值放宽到 3%。

- [ ] **Step 3: Vulkan 后端截图验证**

```bash
source /tmp/rd_env.sh
adb shell am force-stop com.rd.sample
adb shell am start -n com.rd.sample/.MainActivity --ei backend 0
sleep 4
adb exec-out screencap -p > /tmp/android_vulkan.png
./build/tools/img_check/img_check /tmp/android_vulkan.png --min-coverage 0.03
```
预期：`PASS`。
若 Vulkan 失败而 GLES 通过：查 `adb logcat -s rhi.vk:E`（模拟器 SwiftShader Vulkan 支持交换链，常见问题是格式/扩展，按日志修复）。

- [ ] **Step 4: 备份两张截图进 tests/golden 并核对**

```bash
cp /tmp/android_gles.png /tmp/android_vulkan.png tests/golden/
```
用 sips+python 16x16 采样（P0-1 用过的方法）目视确认两图都是顶点色立方体。

- [ ] **Step 5: CI 追加 Android 构建 job**

`.github/workflows/ci.yml` 在 `host-tests` job 之后追加：
```yaml
  android-build:
    runs-on: macos-14
    steps:
      - uses: actions/checkout@v4
      - name: Host shaders
        run: |
          cmake -S . -B build
          cmake --build build -j8 --target shader_cube.vert shader_cube.frag
      - name: Setup Android SDK
        uses: android-actions/setup-android@v3
        with:
          packages: platforms;android-34 build-tools;34.0.0 ndk;26.3.11579264
      - name: Build APK
        uses: gradle/actions/setup-gradle@v4
      - run: ./gradlew :app:assembleDebug
        working-directory: samples/android
```
（模拟器运行验证放本地；CI 只保证 APK 可构建，防回归。）

- [ ] **Step 6: Commit**

```bash
git add tests/golden/android_gles.png tests/golden/android_vulkan.png .github/workflows/ci.yml
git commit -m "test(android): 模拟器双后端截图验证 + CI 构建 job"
```

---

### Task 13: iOS 平台库 + sample app + 模拟器验证

**Files:**
- Create: `cmake/ios.toolchain.cmake`
- Create: `platform/ios/RenderView.swift`
- Create: `samples/ios/CMakeLists.txt`
- Create: `samples/ios/RdDemo/Info.plist`
- Create: `samples/ios/RdDemo/RdDemo-Bridging-Header.h`
- Create: `samples/ios/RdDemo/AppDelegate.swift`

- [ ] **Step 1: 写 iOS toolchain 与平台文件**

`cmake/ios.toolchain.cmake`:
```cmake
set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_OSX_DEPLOYMENT_TARGET "16.0")
if(NOT DEFINED RD_IOS_SDK)
  set(RD_IOS_SDK iphoneos)
endif()
set(CMAKE_OSX_SYSROOT ${RD_IOS_SDK})
set(CMAKE_OSX_ARCHITECTURES arm64)
```

`platform/ios/RenderView.swift`:
```swift
import QuartzCore
import UIKit

/// 3D 渲染视图（Metal 后端）。engine 调用全在主线程（与 MTKView 惯例一致）。
@objc public final class RenderView: UIView {
    override public class var layerClass: AnyClass { CAMetalLayer.self }

    private var engine: OpaquePointer?
    private var link: CADisplayLink?
    private var lastTimestamp: CFTimeInterval = 0

    override public init(frame: CGRect) {
        super.init(frame: frame)
        contentScaleFactor = UIScreen.main.nativeScale
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        contentScaleFactor = UIScreen.main.nativeScale
    }

    override public func didMoveToWindow() {
        super.didMoveToWindow()
        if window != nil {
            startEngine()
        } else {
            stopEngine()
        }
    }

    override public func layoutSubviews() {
        super.layoutSubviews()
        guard let engine else { return }
        let scale = contentScaleFactor
        let w = UInt32(max(1, bounds.width * scale))
        let h = UInt32(max(1, bounds.height * scale))
        (layer as? CAMetalLayer)?.drawableSize = CGSize(width: Int(w), height: Int(h))
        rd_engine_resize(engine, w, h)
    }

    private func startEngine() {
        guard engine == nil else { return }
        engine = rd_engine_create(RD_BACKEND_METAL)
        guard let engine else { return }
        let metalLayer = unsafeDowncast(layer, to: CAMetalLayer.self)
        let scale = contentScaleFactor
        let w = UInt32(max(1, bounds.width * scale))
        let h = UInt32(max(1, bounds.height * scale))
        let layerPtr = Unmanaged.passUnretained(metalLayer).toOpaque()
        guard rd_engine_set_surface(engine, layerPtr, w, h) == RD_OK else {
            rd_engine_destroy(engine)
            engine = nil
            return
        }
        let displayLink = CADisplayLink(target: self, selector: #selector(tick(_:)))
        displayLink.add(to: .main, forMode: .common)
        link = displayLink
    }

    @objc private func tick(_ displayLink: CADisplayLink) {
        guard let engine else { return }
        let dt = lastTimestamp == 0 ? Float(1.0 / 60.0) : Float(displayLink.timestamp - lastTimestamp)
        lastTimestamp = displayLink.timestamp
        rd_engine_render_frame(engine, dt)
    }

    private func stopEngine() {
        link?.invalidate()
        link = nil
        if let engine {
            rd_engine_clear_surface(engine)
            rd_engine_destroy(engine)
        }
        engine = nil
    }
}
```

- [ ] **Step 2: 写 sample app 文件**

`samples/ios/RdDemo/RdDemo-Bridging-Header.h`:
```objc
#pragma once
#include "api/rd_api.h"
```

`samples/ios/RdDemo/Info.plist`:
```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>$(EXECUTABLE_NAME)</string>
    <key>CFBundleIdentifier</key><string>$(PRODUCT_BUNDLE_IDENTIFIER)</string>
    <key>CFBundleName</key><string>$(PRODUCT_NAME)</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>UILaunchScreen</key><dict/>
</dict>
</plist>
```

`samples/ios/RdDemo/AppDelegate.swift`:
```swift
import UIKit

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    var window: UIWindow?

    func application(
        _ application: UIApplication,
        didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?
    ) -> Bool {
        let window = UIWindow(frame: UIScreen.main.bounds)
        window.rootViewController = UIViewController()
        window.rootViewController?.view = RenderView(frame: UIScreen.main.bounds)
        window.makeKeyAndVisible()
        self.window = window
        return true
    }
}
```

`samples/ios/CMakeLists.txt`:
```cmake
enable_language(Swift)

add_executable(RdDemo MACOSX_BUNDLE
  RdDemo/AppDelegate.swift
  ${CMAKE_SOURCE_DIR}/platform/ios/RenderView.swift
)
target_link_libraries(RdDemo PRIVATE rd_core "-framework UIKit" "-framework QuartzCore")
set_target_properties(RdDemo PROPERTIES
  MACOSX_BUNDLE_BUNDLE_NAME RdDemo
  MACOSX_BUNDLE_GUI_IDENTIFIER com.rd.demo
  MACOSX_BUNDLE_BUNDLE_VERSION 1
  MACOSX_BUNDLE_SHORT_VERSION_STRING 1.0
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/RdDemo/Info.plist
  XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}/RdDemo/RdDemo-Bridging-Header.h
  XCODE_ATTRIBUTE_SWIFT_VERSION 5.0
  XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
)
```
（Swift 调用 `RD_BACKEND_METAL` / `RD_OK` 等 C 枚举常量经 bridging header 导入。普通 C 枚举在 Swift 中的导入形式随版本有差异：若 `RD_BACKEND_METAL`/`== RD_OK` 编译报错，改用 `rd_backend_t(rawValue: 1)` 与 `result.rawValue == 0` 形式，构建错误会明确指出。）

- [ ] **Step 3: 配置 iOS 模拟器构建并编译**

```bash
./scripts/check.sh   # 确保 build/shaders_out_ios 已生成
cmake -S . -B build-ios -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake -DRD_IOS_SDK=iphonesimulator -DRD_HOST_SHADER_OUT=$PWD/build/shaders_out 2>&1 | tail -3
cmake --build build-ios --config Debug 2>&1 | tail -5
```
预期：`** BUILD SUCCEEDED **`；产物 `build-ios/samples/ios/Debug-iphonesimulator/RdDemo.app`。

- [ ] **Step 4: 模拟器运行 + 截图验证**

```bash
xcrun simctl boot "iPhone 17 Pro" 2>/dev/null || true
open -a Simulator
xcrun simctl install booted build-ios/samples/ios/Debug-iphonesimulator/RdDemo.app
xcrun simctl launch booted com.rd.demo
sleep 4
xcrun simctl io booted screenshot /tmp/ios_metal.png
./build/tools/img_check/img_check /tmp/ios_metal.png --min-coverage 0.03
```
预期：`PASS`。用 sips+python 采样目视确认为顶点色立方体后：
```bash
cp /tmp/ios_metal.png tests/golden/
```

- [ ] **Step 5: Commit**

```bash
git add cmake/ios.toolchain.cmake platform/ios/ samples/ios/ tests/golden/ios_metal.png
git commit -m "feat(platform/ios): RenderView(CAMetalLayer+CADisplayLink) + 模拟器截图验证"
```

---

### Task 14: 文档更新与 P0 整体验收

**Files:**
- Modify: `AGENTS.md`
- Modify: `README.md`

- [ ] **Step 1: 更新 AGENTS.md**

把「当前状态」段替换为：
```markdown
## 当前状态
- P0 完成：构建基建 + foundation + RHI 三后端（Vulkan/Metal/GLES）+ shader 离线管线
  + C API（rd_engine）+ Android/iOS RenderView 容器 + 双端模拟器截图验证
- P1 待做：glTF 加载 + PBR/IBL（resource/renderer/scene 层）
```
在「代码约定」段追加：
```markdown
- engine 线程约定：同一 rd_engine 的所有调用在同一线程（Android=RenderView 渲染线程，iOS=主线程）
- shader 内嵌：embedded_shaders.cpp 自动生成（host=build 期；Android/iOS=configure 期），勿手改
```

「构建与测试」段追加：
```markdown
## 移动端构建
- Android：`source /tmp/rd_env.sh`（ANDROID_HOME/JAVA_HOME），
  `cd samples/android && ./gradlew :app:assembleDebug`
- iOS 模拟器：`cmake -S . -B build-ios -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake -DRD_IOS_SDK=iphonesimulator && cmake --build build-ios --config Debug`
- 截图校验：`./build/tools/img_check/img_check <png> --min-coverage 0.03`
```

- [ ] **Step 2: 更新 README.md**

在「快速开始」后追加：
```markdown
## 移动端
- Android demo：`samples/android`（Gradle 构建，Vulkan/GLES 双后端）
- iOS demo：`samples/ios`（CMake/Xcode 生成，Metal）
- 业务接入：iOS `RenderView`（UIView）/ Android `RenderView`（SurfaceView），C API 见 `core/api/rd_api.h`
```

- [ ] **Step 3: P0 整体验收核对**

```bash
./scripts/check.sh   # 预期 29/29（25 + 4 Api 测试）
git log --oneline
```
逐项确认（对照 spec P0）：
- [ ] host 全绿（含 4 个 Api 测试）
- [ ] Android APK 构建成功；GLES 与 Vulkan 模拟器截图均 img_check PASS
- [ ] iOS 模拟器截图 img_check PASS
- [ ] tests/golden/ 含 cube_metal/cube_vulkan/android_gles/android_vulkan/ios_metal 五张参考图
- [ ] 每任务一个 commit

- [ ] **Step 4: Commit**

```bash
git add AGENTS.md README.md
git commit -m "docs: P0 完成状态与移动端构建说明"
```

---

## P0 验收标准（对照 spec）

| spec P0 条目 | 落点 | 状态 |
|---|---|---|
| CMake 构建 + CI | P0-1 T1/T13；本计划 T12 CI 追加 | ✅ |
| foundation | P0-1 T2-T5 | ✅ |
| RHI 抽象 | P0-1 T6-7 + 本计划 T2 SwapChain | ✅ |
| Metal 后端 | P0-1 T10 + 本计划 T5 swapchain | ✅ 主机+iOS 模拟器 |
| Vulkan 后端 | P0-1 T11 + 本计划 T8 Android swapchain | ✅ 主机+Android 模拟器 |
| GLES 后端 | 本计划 T6 | ✅ Android 模拟器 |
| shader 离线管线 | P0-1 T8 + 本计划 T4 内嵌 | ✅ |
| 双端 RenderView 容器 | 本计划 T10/T13 | ✅ |
| 双端 demo 旋转立方体 | T11/T13 sample app | ✅ |
| 三后端 golden image 一致 | P0-1 像素级（Metal≡Vulkan）；本计划 T12/T13 结构级（真机 GPU 差异容忍） | ✅ |

## 自查记录

- 类型一致性：`rd_engine_*` C API（T9）与 JNI（T10）/Swift（T13）调用签名一致；`SwapChainHandle` 系列方法在 T2 定义、T5/T6/T8 三后端实现签名一致；`CubeScene::init`（T3）与 T9 engine 调用一致；`embeddedCubeShader`（T4 声明）与 T9 使用一致。
- 已知简化（P1 处理）：Vulkan present 前 waitIdle 帧串行；GLES uniform block 硬编码 UBO↔slot0；离屏目标无深度附件；iOS 主线程渲染。
- 风险：模拟器 SwiftShader 与真机 GPU 差异 → P0 验收以结构校验为准，真机回归归 P2 性能基准；AGP/Kotlin 版本组合（8.5.2 + 2.0.20）为已验证兼容对。



