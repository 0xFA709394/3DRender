# P0-1 主机可验证内核：构建基建 + foundation + RHI 抽象 + Metal/Vulkan 后端 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 macOS 主机上建成 3D 渲染器内核骨架：CMake 构建、foundation 基础库、RHI 抽象层、shader 离线编译管线、Metal 与 Vulkan(MoltenVK) 两个后端，以"离屏渲染旋转立方体 + golden image 跨后端一致性"作为验收。

**Architecture:** 分层 C++17 内核（foundation → rhi），RHI 用抽象基类 `Device`/`CommandBuffer` + 类型安全句柄；后端实现离屏渲染（无窗口系统依赖），cube 顶点色渲染（凸体 + 背面剔除，无需深度缓冲）；shader 写一份 GLSL 450，构建期经 glslang/SPIRV-Cross 产出 SPIR-V/MSL(metallib)/GLSL ES + 反射 JSON。本计划是 P0 的上半部分；GLES 后端仅留桩，Android/iOS 容器在 P0-2 计划实现。

**Tech Stack:** C++17, CMake 3.24+, googletest, glm, glslang, SPIRV-Cross, Vulkan-Headers, MoltenVK (brew), Metal (macOS), stb

**前置环境（一次性）：**
```bash
brew install moltenvk cmake
```
预期：`ls /opt/homebrew/lib/libMoltenVK.dylib`（Intel Mac 为 `/usr/local/lib/libMoltenVK.dylib`）存在。

**执行约定：**
- 工作目录：仓库根 `/Users/meetyou/Desktop/3DRender`
- 所有任务的提交都在 `main` 分支
- 构建目录统一 `build/`；测试命令统一 `ctest --test-dir build --output-on-failure`
- 设计文档见 `docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md`

---

## 文件结构（本计划产出）

```
CMakeLists.txt                     # 顶层构建
cmake/Deps.cmake                   # 第三方依赖 (FetchContent)
cmake/ShaderCompile.cmake          # shader 离线编译函数
core/CMakeLists.txt
core/foundation/version.{h,cpp}    # 版本号（Task 1）
core/foundation/handle.h           # 类型安全句柄（Task 2）
core/foundation/log.{h,cpp}        # 日志（Task 3）
core/foundation/math.h             # 数学封装 glm（Task 4）
core/foundation/task_queue.{h,cpp} # 任务队列（Task 5）
core/rhi/rhi_types.h               # RHI 枚举/描述体（Task 6）
core/rhi/rhi_device.h              # Device/CommandBuffer 抽象（Task 7）
core/rhi/rhi_factory.cpp           # createDevice 工厂（Task 7）
core/rhi/backends/metal/metal_device.{h,mm}      # Metal 后端（Task 10）
core/rhi/backends/vulkan/vulkan_device.{h,cpp}   # Vulkan 后端（Task 11）
core/rhi/backends/gles/gles_device.{h,cpp}       # GLES 桩（Task 12）
shaders/CMakeLists.txt             # shader 编译目标（Task 8）
shaders/cube.vert, shaders/cube.frag
tests/CMakeLists.txt
tests/foundation/smoke_test.cpp    # Task 1
tests/foundation/handle_test.cpp   # Task 2
tests/foundation/log_test.cpp      # Task 3
tests/foundation/math_test.cpp     # Task 4
tests/foundation/task_queue_test.cpp # Task 5
tests/rhi/rhi_types_test.cpp       # Task 6
tests/rhi/factory_test.cpp         # Task 7
tests/common/image.{h,cpp}         # PNG 保存/比对（Task 9）
tests/common/image_test.cpp        # Task 9
tests/common/cube_renderer.{h,cpp} # 共享 cube 渲染器（Task 10）
tests/rhi/cube_test.cpp            # golden image 测试（Task 10/11）
tests/golden/cube_metal.png        # 生成后提交（Task 10）
tests/golden/cube_vulkan.png       # 生成后提交（Task 11）
tools/render_test/main.cpp         # 手动验证 CLI（Task 13）
tools/render_test/CMakeLists.txt
.github/workflows/ci.yml           # CI（Task 13）
scripts/check.sh                   # 本地一键检查（Task 13）
AGENTS.md, README.md               # 文档（Task 14）
```

---

### Task 1: 构建骨架与首个单元测试

**Files:**
- Create: `CMakeLists.txt`
- Create: `cmake/Deps.cmake`
- Create: `core/CMakeLists.txt`
- Create: `core/foundation/version.h`
- Create: `core/foundation/version.cpp`
- Create: `tests/CMakeLists.txt`
- Create: `tests/foundation/smoke_test.cpp`

- [ ] **Step 1: 写顶层 CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.24)
project(rdrenderer LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

include(cmake/Deps.cmake)

enable_testing()
add_subdirectory(core)
add_subdirectory(shaders)
add_subdirectory(tests)
add_subdirectory(tools/render_test)
```

- [ ] **Step 2: 写 cmake/Deps.cmake（googletest + glm）**

```cmake
include(FetchContent)

FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(glm
  URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz)
FetchContent_MakeAvailable(glm)
```

- [ ] **Step 3: 写 core/CMakeLists.txt 与 version 模块**

`core/CMakeLists.txt`:
```cmake
add_library(rd_core STATIC
  foundation/version.cpp
)
target_include_directories(rd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rd_core PUBLIC glm::glm)
target_compile_definitions(rd_core PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
```

`core/foundation/version.h`:
```cpp
#pragma once
namespace rd {
const char* version();
}
```

`core/foundation/version.cpp`:
```cpp
#include "foundation/version.h"
namespace rd {
const char* version() { return "0.1.0-p0"; }
}
```

- [ ] **Step 4: 写 shaders/ 与 tools/render_test/ 的占位 CMake**

`shaders/CMakeLists.txt`:
```cmake
# shader 编译目标在 Task 8 填充
```

`tools/render_test/CMakeLists.txt`:
```cmake
# render_test CLI 在 Task 13 填充
```

- [ ] **Step 5: 写 tests/CMakeLists.txt 与首个测试**

`tests/CMakeLists.txt`:
```cmake
include(GoogleTest)

add_executable(rd_tests
  foundation/smoke_test.cpp
)
target_link_libraries(rd_tests PRIVATE rd_core GTest::gtest_main)
gtest_discover_tests(rd_tests)
```

`tests/foundation/smoke_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "foundation/version.h"

TEST(Smoke, VersionIsSet) {
  ASSERT_NE(rd::version(), nullptr);
}
```

- [ ] **Step 6: 配置、构建、跑测试**

```bash
cmake -S . -B build 2>&1 | tail -5
cmake --build build -j8 2>&1 | tail -5
ctest --test-dir build --output-on-failure
```

预期：配置成功（FetchContent 下载 gtest/glm）；构建成功；`1/1 Test #1: Smoke.VersionIsSet ... Passed`。

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt cmake/ core/ shaders/ tests/ tools/
git commit -m "build: CMake 骨架 + googletest/glm 接入 + 冒烟测试"
```

---

### Task 2: foundation — 类型安全句柄 Handle<T>

**Files:**
- Create: `core/foundation/handle.h`
- Test: `tests/foundation/handle_test.cpp`

- [ ] **Step 1: 写失败测试**

`tests/foundation/handle_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "foundation/handle.h"

namespace {
struct FooTag;
using FooHandle = rd::Handle<FooTag>;
struct BarTag;
using BarHandle = rd::Handle<BarTag>;

TEST(Handle, DefaultIsInvalid) {
  FooHandle h;
  EXPECT_FALSE(h.valid());
}

TEST(Handle, ExplicitValueIsValid) {
  FooHandle h(42);
  EXPECT_TRUE(h.valid());
  EXPECT_EQ(h.value(), 42u);
}

TEST(Handle, EqualityByValue) {
  EXPECT_EQ(FooHandle(1), FooHandle(1));
  EXPECT_NE(FooHandle(1), FooHandle(2));
}

TEST(Handle, DistinctTypesNotComparable) {
  // 不同类型句柄无法互转/比较 —— 静态断言验证类型安全
  EXPECT_FALSE((std::is_convertible_v<FooHandle, BarHandle>));
}
} // namespace
```

`tests/CMakeLists.txt` 的 `add_executable` 列表加入 `foundation/handle_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

```bash
cmake --build build -j8 2>&1 | tail -5
```
预期：编译错误 `foundation/handle.h file not found`。

- [ ] **Step 3: 实现 handle.h**

`core/foundation/handle.h`:
```cpp
#pragma once
#include <cstdint>
#include <type_traits>

namespace rd {

// 类型安全句柄：不同 Tag 的句柄不可互换。0 为无效值。
template <typename Tag>
class Handle {
public:
  Handle() = default;
  explicit Handle(uint32_t v) : value_(v) {}

  bool valid() const { return value_ != 0; }
  uint32_t value() const { return value_; }

  bool operator==(const Handle& o) const { return value_ == o.value_; }
  bool operator!=(const Handle& o) const { return value_ != o.value_; }

private:
  uint32_t value_ = 0;
};

} // namespace rd

namespace std {
template <typename Tag>
struct hash<rd::Handle<Tag>> {
  size_t operator()(rd::Handle<Tag> h) const noexcept { return h.value(); }
};
} // namespace std
```

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure -R Handle
```
预期：`4 tests ... Passed`。

- [ ] **Step 5: Commit**

```bash
git add core/foundation/handle.h tests/foundation/handle_test.cpp tests/CMakeLists.txt
git commit -m "feat(foundation): 类型安全句柄 Handle<T>"
```

---

### Task 3: foundation — 日志系统

**Files:**
- Create: `core/foundation/log.h`
- Create: `core/foundation/log.cpp`
- Test: `tests/foundation/log_test.cpp`
- Modify: `core/CMakeLists.txt`（向 `rd_core` 源列表添加 `foundation/log.cpp`）

- [ ] **Step 1: 写失败测试**

`tests/foundation/log_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "foundation/log.h"
#include <string>
#include <vector>

namespace {
struct Capture {
  std::vector<std::string> lines;
  static Capture* self;
  static void sink(rd::LogLevel level, const char* tag, const char* msg) {
    self->lines.push_back(std::string(rd::logLevelName(level)) + "|" + tag + "|" + msg);
  }
};
Capture* Capture::self = nullptr;

TEST(Log, RoutesToSinkWithFormatting) {
  Capture c;
  Capture::self = &c;
  rd::setLogSink(&Capture::sink);
  rd::log(rd::LogLevel::Info, "rhi", "created %s #%d", "buffer", 7);
  ASSERT_EQ(c.lines.size(), 1u);
  EXPECT_EQ(c.lines[0], "info|rhi|created buffer #7");
  rd::setLogSink(nullptr);
}

TEST(Log, FiltersBelowMinLevel) {
  Capture c;
  Capture::self = &c;
  rd::setLogSink(&Capture::sink);
  rd::setLogMinLevel(rd::LogLevel::Warn);
  rd::log(rd::LogLevel::Info, "rhi", "hidden");
  rd::log(rd::LogLevel::Error, "rhi", "shown");
  EXPECT_EQ(c.lines.size(), 1u);
  EXPECT_EQ(c.lines[0], "error|rhi|shown");
  rd::setLogSink(nullptr);
  rd::setLogMinLevel(rd::LogLevel::Debug);
}
} // namespace
```

`tests/CMakeLists.txt` 源列表加入 `foundation/log_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

```bash
cmake --build build -j8 2>&1 | tail -3
```
预期：`foundation/log.h file not found`。

- [ ] **Step 3: 实现 log.h / log.cpp**

`core/foundation/log.h`:
```cpp
#pragma once

namespace rd {

enum class LogLevel { Debug = 0, Info, Warn, Error };

const char* logLevelName(LogLevel level);

using LogSink = void (*)(LogLevel level, const char* tag, const char* msg);

// 设置日志输出目标；nullptr 时输出到 stderr（默认）。
void setLogSink(LogSink sink);
void setLogMinLevel(LogLevel level);

void log(LogLevel level, const char* tag, const char* fmt, ...);

} // namespace rd

#define RD_LOGD(tag, ...) ::rd::log(::rd::LogLevel::Debug, tag, __VA_ARGS__)
#define RD_LOGI(tag, ...) ::rd::log(::rd::LogLevel::Info, tag, __VA_ARGS__)
#define RD_LOGW(tag, ...) ::rd::log(::rd::LogLevel::Warn, tag, __VA_ARGS__)
#define RD_LOGE(tag, ...) ::rd::log(::rd::LogLevel::Error, tag, __VA_ARGS__)
```

`core/foundation/log.cpp`:
```cpp
#include "foundation/log.h"
#include <cstdarg>
#include <cstdio>

namespace rd {

namespace {
LogSink g_sink = nullptr;
LogLevel g_minLevel = LogLevel::Debug;
} // namespace

const char* logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info:  return "info";
    case LogLevel::Warn:  return "warn";
    case LogLevel::Error: return "error";
  }
  return "unknown";
}

void setLogSink(LogSink sink) { g_sink = sink; }
void setLogMinLevel(LogLevel level) { g_minLevel = level; }

void log(LogLevel level, const char* tag, const char* fmt, ...) {
  if (level < g_minLevel) return;
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (g_sink) {
    g_sink(level, tag, buf);
  } else {
    fprintf(stderr, "[%s] %s: %s\n", logLevelName(level), tag, buf);
  }
}

} // namespace rd
```

`core/CMakeLists.txt` 源列表加入 `foundation/log.cpp`。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure -R Log
```
预期：`2 tests ... Passed`。

- [ ] **Step 5: Commit**

```bash
git add core/foundation/log.h core/foundation/log.cpp core/CMakeLists.txt tests/foundation/log_test.cpp tests/CMakeLists.txt
git commit -m "feat(foundation): 日志系统（级别过滤 + 可插拔 sink）"
```

---

### Task 4: foundation — 数学库封装（glm）

**Files:**
- Create: `core/foundation/math.h`
- Test: `tests/foundation/math_test.cpp`

- [ ] **Step 1: 写失败测试**

`tests/foundation/math_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "foundation/math.h"

using rd::math::Mat4;
using rd::math::Vec3;
using rd::math::Vec4;

TEST(Math, PerspectiveMapsNearToZero) {
  // GLM_FORCE_DEPTH_ZERO_TO_ONE 约定：近平面 NDC z=0
  Mat4 p = rd::math::perspective(rd::math::radians(60.0f), 1.0f, 0.1f, 100.0f);
  Vec4 near = p * Vec4(0.0f, 0.0f, -0.1f, 1.0f);
  EXPECT_NEAR(near.z / near.w, 0.0f, 1e-5f);
}

TEST(Math, LookAtMovesCameraBack) {
  Mat4 v = rd::math::lookAt(Vec3(0, 0, 4), Vec3(0, 0, 0), Vec3(0, 1, 0));
  Vec4 origin = v * Vec4(0, 0, 0, 1);
  EXPECT_NEAR(origin.z, -4.0f, 1e-5f); // 原点在相机前方 4 个单位（视图空间 -z）
}

TEST(Math, RotateNinetyDegreesAboutY) {
  Mat4 r = rd::math::rotate(Mat4(1.0f), rd::math::radians(90.0f), Vec3(0, 1, 0));
  Vec4 x = r * Vec4(1, 0, 0, 1);
  EXPECT_NEAR(x.x, 0.0f, 1e-5f);
  EXPECT_NEAR(x.z, -1.0f, 1e-5f);
}
```

`tests/CMakeLists.txt` 源列表加入 `foundation/math_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

```bash
cmake --build build -j8 2>&1 | tail -3
```
预期：`foundation/math.h file not found`。

- [ ] **Step 3: 实现 math.h**

`core/foundation/math.h`:
```cpp
#pragma once
// 统一约定：右手坐标系，NDC z ∈ [0,1]（GLM_FORCE_DEPTH_ZERO_TO_ONE 由构建系统定义，
// 匹配 Vulkan/Metal；GLES 深度范围为 [-1,1]，仅影响深度精度不影响遮挡关系）。
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace rd::math {

using glm::Mat3;
using glm::Mat4;
using glm::Vec2;
using glm::Vec3;
using glm::Vec4;

inline float radians(float degrees) { return glm::radians(degrees); }

inline Mat4 perspective(float fovYRad, float aspect, float zNear, float zFar) {
  return glm::perspective(fovYRad, aspect, zNear, zFar);
}

inline Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
  return glm::lookAt(eye, center, up);
}

inline Mat4 rotate(const Mat4& m, float angleRad, const Vec3& axis) {
  return glm::rotate(m, angleRad, axis);
}

} // namespace rd::math
```

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure -R Math
```
预期：`3 tests ... Passed`。

- [ ] **Step 5: Commit**

```bash
git add core/foundation/math.h tests/foundation/math_test.cpp tests/CMakeLists.txt
git commit -m "feat(foundation): 数学库封装（glm，NDC z [0,1] 约定）"
```

---

### Task 5: foundation — 任务队列 TaskQueue

**Files:**
- Create: `core/foundation/task_queue.h`
- Create: `core/foundation/task_queue.cpp`
- Test: `tests/foundation/task_queue_test.cpp`
- Modify: `core/CMakeLists.txt`（源列表添加 `foundation/task_queue.cpp`）

- [ ] **Step 1: 写失败测试**

`tests/foundation/task_queue_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "foundation/task_queue.h"
#include <thread>
#include <vector>

TEST(TaskQueue, ExecutesInFifoOrder) {
  rd::TaskQueue q;
  std::vector<int> order;
  for (int i = 0; i < 5; ++i) q.post([&order, i] { order.push_back(i); });
  while (q.tryPop()) {
  }
  ASSERT_EQ(order.size(), 5u);
  for (int i = 0; i < 5; ++i) EXPECT_EQ(order[i], i);
}

TEST(TaskQueue, WaitAndPopBlocksUntilWork) {
  rd::TaskQueue q;
  int ran = 0;
  std::thread consumer([&] { q.waitAndPop(); ran = 1; });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(ran, 0); // 无任务时应阻塞
  q.post([&] {});
  consumer.join();
  EXPECT_EQ(ran, 1);
}
```

`tests/CMakeLists.txt` 源列表加入 `foundation/task_queue_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

```bash
cmake --build build -j8 2>&1 | tail -3
```
预期：`foundation/task_queue.h file not found`。

- [ ] **Step 3: 实现 task_queue.h / task_queue.cpp**

`core/foundation/task_queue.h`:
```cpp
#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>

namespace rd {

// 多生产者单消费者任务队列。渲染线程模型的基础件：
// 主线程 post，渲染线程 tryPop/waitAndPop 消费。
class TaskQueue {
public:
  void post(std::function<void()> fn);
  bool tryPop();      // 有任务则执行一个并返回 true
  void waitAndPop();  // 阻塞直到有任务并执行

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<std::function<void()>> queue_;
};

} // namespace rd
```

`core/foundation/task_queue.cpp`:
```cpp
#include "foundation/task_queue.h"

namespace rd {

void TaskQueue::post(std::function<void()> fn) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(std::move(fn));
  }
  cv_.notify_one();
}

bool TaskQueue::tryPop() {
  std::function<void()> fn;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) return false;
    fn = std::move(queue_.front());
    queue_.pop();
  }
  fn();
  return true;
}

void TaskQueue::waitAndPop() {
  std::function<void()> fn;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return !queue_.empty(); });
    fn = std::move(queue_.front());
    queue_.pop();
  }
  fn();
}

} // namespace rd
```

`core/CMakeLists.txt` 源列表加入 `foundation/task_queue.cpp`。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure -R TaskQueue
```
预期：`2 tests ... Passed`。

- [ ] **Step 5: Commit**

```bash
git add core/foundation/task_queue.h core/foundation/task_queue.cpp core/CMakeLists.txt tests/foundation/task_queue_test.cpp tests/CMakeLists.txt
git commit -m "feat(foundation): MPSC 任务队列"
```

---

### Task 6: RHI 类型与描述体（rhi_types.h）

**Files:**
- Create: `core/rhi/rhi_types.h`
- Test: `tests/rhi/rhi_types_test.cpp`

- [ ] **Step 1: 写失败测试**

`tests/rhi/rhi_types_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "rhi/rhi_types.h"

TEST(RhiTypes, BufferUsageFlagsCombine) {
  rd::BufferUsage u = rd::BufferUsage::Vertex | rd::BufferUsage::Index;
  EXPECT_TRUE(rd::hasFlag(u, rd::BufferUsage::Vertex));
  EXPECT_TRUE(rd::hasFlag(u, rd::BufferUsage::Index));
  EXPECT_FALSE(rd::hasFlag(u, rd::BufferUsage::Uniform));
}

TEST(RhiTypes, FormatSizeBytes) {
  EXPECT_EQ(rd::formatSize(rd::Format::RGBA8_UNORM), 4u);
  EXPECT_EQ(rd::formatSize(rd::Format::R32G32B32_FLOAT), 12u);
  EXPECT_EQ(rd::formatSize(rd::Format::D32_FLOAT), 4u);
}

TEST(RhiTypes, PipelineDescDefaults) {
  rd::PipelineDesc d;
  EXPECT_EQ(d.topology, rd::PrimitiveTopology::TriangleList);
  EXPECT_EQ(d.cullMode, rd::CullMode::None);
  EXPECT_FALSE(d.depthTest);
  EXPECT_EQ(d.colorFormat, rd::Format::RGBA8_UNORM);
}

TEST(RhiTypes, ShaderModuleDescDefaultEntry) {
  rd::ShaderModuleDesc d;
  EXPECT_EQ(d.entryPoint, "main0"); // spirv-cross 默认入口名
}
```

`tests/CMakeLists.txt` 源列表加入 `rhi/rhi_types_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

```bash
cmake --build build -j8 2>&1 | tail -3
```
预期：`rhi/rhi_types.h file not found`。

- [ ] **Step 3: 实现 rhi_types.h**

`core/rhi/rhi_types.h`:
```cpp
#pragma once
#include "foundation/handle.h"
#include <cstdint>
#include <string>
#include <vector>

namespace rd {

enum class Backend { Vulkan, Metal, GLES };

// ---- 资源句柄 ----
struct BufferTag;
struct ShaderModuleTag;
struct PipelineTag;
struct TargetTag;
using BufferHandle = Handle<BufferTag>;
using ShaderModuleHandle = Handle<ShaderModuleTag>;
using PipelineHandle = Handle<PipelineTag>;
using TargetHandle = Handle<TargetTag>;

// ---- 枚举 ----
enum class Format {
  RGBA8_UNORM,
  BGRA8_UNORM,
  R32G32_FLOAT,
  R32G32B32_FLOAT,
  R32G32B32A32_FLOAT,
  D32_FLOAT,
};

enum class BufferUsage : uint32_t {
  Vertex = 1u << 0,
  Index = 1u << 1,
  Uniform = 1u << 2,
};

constexpr BufferUsage operator|(BufferUsage a, BufferUsage b) {
  return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr bool hasFlag(BufferUsage value, BufferUsage flag) {
  return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

enum class ShaderStage { Vertex, Fragment };
enum class PrimitiveTopology { TriangleList, TriangleStrip, LineList };
enum class CullMode { None, Front, Back };

// ---- 描述体 ----
struct ClearColor {
  float r = 0, g = 0, b = 0, a = 1;
};

struct BufferDesc {
  uint64_t size = 0;
  BufferUsage usage = BufferUsage::Vertex;
  const void* data = nullptr; // 非空则创建时上传
};

struct ShaderModuleDesc {
  ShaderStage stage = ShaderStage::Vertex;
  std::vector<uint8_t> code;              // SPIR-V / metallib / GLSL ES 文本
  std::string entryPoint = "main0";
};

struct VertexBinding {
  uint32_t binding = 0;
  uint32_t stride = 0;
};

struct VertexAttribute {
  uint32_t location = 0;
  Format format = Format::R32G32B32_FLOAT;
  uint32_t offset = 0;
  uint32_t binding = 0;
};

struct PipelineDesc {
  ShaderModuleHandle vertexShader;
  ShaderModuleHandle fragmentShader;
  std::vector<VertexBinding> vertexBindings;
  std::vector<VertexAttribute> attributes;
  PrimitiveTopology topology = PrimitiveTopology::TriangleList;
  CullMode cullMode = CullMode::None;
  bool depthTest = false;
  Format colorFormat = Format::RGBA8_UNORM;
};

struct OffscreenTargetDesc {
  uint32_t width = 0;
  uint32_t height = 0;
  Format colorFormat = Format::RGBA8_UNORM;
};

struct DeviceDesc {
  Backend backend = Backend::Vulkan;
  bool enableValidation = false;
};

// 绑定约定（三后端统一）：
//   uniform slot N  ↔  Metal buffer(N)  ↔  Vulkan set 0 binding N  ↔  GLES binding point N
//   vertex binding N ↔  Metal buffer(N+1)（0 留给 uniform）
uint32_t formatSize(Format f);

inline uint32_t formatSize(Format f) {
  switch (f) {
    case Format::RGBA8_UNORM:
    case Format::BGRA8_UNORM:
    case Format::D32_FLOAT:
      return 4;
    case Format::R32G32_FLOAT:
      return 8;
    case Format::R32G32B32_FLOAT:
      return 12;
    case Format::R32G32B32A32_FLOAT:
      return 16;
  }
  return 0;
}

} // namespace rd
```

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure -R RhiTypes
```
预期：`4 tests ... Passed`。

- [ ] **Step 5: Commit**

```bash
git add core/rhi/rhi_types.h tests/rhi/rhi_types_test.cpp tests/CMakeLists.txt
git commit -m "feat(rhi): 类型与描述体定义 + 绑定约定"
```

---

### Task 7: RHI Device 抽象与后端工厂

**Files:**
- Create: `core/rhi/rhi_device.h`
- Create: `core/rhi/rhi_factory.cpp`
- Test: `tests/rhi/factory_test.cpp`
- Modify: `core/CMakeLists.txt`（源列表添加 `rhi/rhi_factory.cpp`）

- [ ] **Step 1: 写失败测试**

`tests/rhi/factory_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"

// GLES 在 host 上永不可用（实现归 Android/P0-2），工厂应返回 nullptr 且不崩溃。
// Metal/Vulkan 的可用性由 cube 测试覆盖（见 Task 10/11）。
TEST(Factory, UnavailableBackendReturnsNull) {
  rd::DeviceDesc desc;
  desc.backend = rd::Backend::GLES;
  EXPECT_EQ(rd::createDevice(desc), nullptr);
}
```

`tests/CMakeLists.txt` 源列表加入 `rhi/factory_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

```bash
cmake --build build -j8 2>&1 | tail -3
```
预期：`rhi/rhi_device.h file not found`。

- [ ] **Step 3: 实现 rhi_device.h 与 rhi_factory.cpp**

`core/rhi/rhi_device.h`:
```cpp
#pragma once
#include "rhi/rhi_types.h"
#include <memory>

namespace rd {

// 每帧从 Device 获取，录制一帧的渲染命令。索引缓冲固定 uint16。
class CommandBuffer {
public:
  virtual ~CommandBuffer() = default;
  virtual void beginRenderPass(TargetHandle target, const ClearColor& clear) = 0;
  virtual void bindPipeline(PipelineHandle pipeline) = 0;
  virtual void bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) = 0;
  virtual void bindIndexBuffer(BufferHandle buffer, uint64_t offset) = 0;
  virtual void bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset,
                                 uint64_t size) = 0;
  virtual void draw(uint32_t vertexCount, uint32_t firstVertex) = 0;
  virtual void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset) = 0;
  virtual void endRenderPass() = 0;
};

// GPU 设备抽象。所有方法只在渲染线程调用（P0-1 测试内单线程使用）。
class Device {
public:
  virtual ~Device() = default;
  virtual Backend backend() const = 0;

  virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
  virtual void updateBuffer(BufferHandle buffer, const void* data, uint64_t size,
                            uint64_t offset) = 0;
  virtual void destroyBuffer(BufferHandle buffer) = 0;

  virtual ShaderModuleHandle createShaderModule(const ShaderModuleDesc& desc) = 0;
  virtual void destroyShaderModule(ShaderModuleHandle module) = 0;

  virtual PipelineHandle createPipeline(const PipelineDesc& desc) = 0;
  virtual void destroyPipeline(PipelineHandle pipeline) = 0;

  virtual TargetHandle createOffscreenTarget(const OffscreenTargetDesc& desc) = 0;
  virtual void destroyTarget(TargetHandle target) = 0;
  // 读出目标像素为紧凑排列的 RGBA8；outSize 需 >= width*height*4
  virtual bool readbackTarget(TargetHandle target, void* outRGBA8, uint64_t outSize) = 0;

  virtual CommandBuffer* acquireCommandBuffer() = 0; // 返回值由 Device 持有，勿 delete
  virtual void submit(CommandBuffer* cmd) = 0;
  virtual void waitIdle() = 0;
};

// 工厂：后端不可用时返回 nullptr（编译期未启用或运行期无设备）。
std::unique_ptr<Device> createDevice(const DeviceDesc& desc);

} // namespace rd
```

`core/rhi/rhi_factory.cpp`:
```cpp
#include "foundation/log.h"
#include "rhi/rhi_device.h"

namespace rd {

// 后端创建函数由各后端编译单元提供；未启用时走下方弱实现。
#if defined(RD_WITH_METAL)
std::unique_ptr<Device> createMetalDevice(const DeviceDesc& desc);
#else
static std::unique_ptr<Device> createMetalDevice(const DeviceDesc&) { return nullptr; }
#endif

#if defined(RD_WITH_VULKAN)
std::unique_ptr<Device> createVulkanDevice(const DeviceDesc& desc);
#else
static std::unique_ptr<Device> createVulkanDevice(const DeviceDesc&) { return nullptr; }
#endif

#if defined(__ANDROID__)
std::unique_ptr<Device> createGLESDevice(const DeviceDesc& desc);
#else
static std::unique_ptr<Device> createGLESDevice(const DeviceDesc&) { return nullptr; }
#endif

std::unique_ptr<Device> createDevice(const DeviceDesc& desc) {
  std::unique_ptr<Device> device;
  switch (desc.backend) {
    case Backend::Metal:
      device = createMetalDevice(desc);
      break;
    case Backend::Vulkan:
      device = createVulkanDevice(desc);
      break;
    case Backend::GLES:
      device = createGLESDevice(desc);
      break;
  }
  if (!device) {
    RD_LOGW("rhi", "backend %d unavailable", static_cast<int>(desc.backend));
  }
  return device;
}

} // namespace rd
```

`core/CMakeLists.txt` 源列表加入 `rhi/rhi_factory.cpp`。

- [ ] **Step 4: 跑测试确认通过**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure -R Factory
```
预期：`1 test ... Passed`（GLES 返回 null）。

- [ ] **Step 5: Commit**

```bash
git add core/rhi/rhi_device.h core/rhi/rhi_factory.cpp core/CMakeLists.txt tests/rhi/factory_test.cpp tests/CMakeLists.txt
git commit -m "feat(rhi): Device/CommandBuffer 抽象 + 后端工厂"
```

---

### Task 8: Shader 离线编译管线

**Files:**
- Create: `cmake/ShaderCompile.cmake`
- Create: `shaders/cube.vert`
- Create: `shaders/cube.frag`
- Modify: `shaders/CMakeLists.txt`
- Modify: `cmake/Deps.cmake`（追加 glslang / SPIRV-Cross）
- Modify: `tests/CMakeLists.txt`（追加 generated 头文件包含目录与 `RD_TEST_DATA_DIR` 定义）

- [ ] **Step 1: Deps.cmake 追加 shader 工具链**

`cmake/Deps.cmake` 末尾追加：
```cmake
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
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
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spirv-cross
  URL https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz)
FetchContent_MakeAvailable(spirv-cross)
```

- [ ] **Step 2: 写 cmake/ShaderCompile.cmake**

```cmake
# rd_compile_shader(<源文件相对 shaders/ 的路径>)
# 产出（构建目录 shaders_out/）：
#   <name>.spv        Vulkan 直接用
#   <name>.metal      MSL 源（参考/调试用）
#   <name>.metallib   Metal 后端加载（仅 APPLE）
#   <name>.gles       GLSL ES 3.0 源（GLES 后端运行期编译，P0-2 使用）
#   <name>.json       反射信息（材质系统用，P1 消费）
set(RD_SHADER_OUT ${CMAKE_BINARY_DIR}/shaders_out)
file(MAKE_DIRECTORY ${RD_SHADER_OUT})

function(rd_compile_shader SRC)
  get_filename_component(F ${SRC} NAME)
  set(IN ${CMAKE_CURRENT_SOURCE_DIR}/${SRC})
  set(SPV ${RD_SHADER_OUT}/${F}.spv)
  set(MSL ${RD_SHADER_OUT}/${F}.metal)
  set(AIR ${RD_SHADER_OUT}/${F}.air)
  set(METALLIB ${RD_SHADER_OUT}/${F}.metallib)
  set(GLES ${RD_SHADER_OUT}/${F}.gles)
  set(REFL ${RD_SHADER_OUT}/${F}.json)

  add_custom_command(
    OUTPUT ${SPV}
    COMMAND $<TARGET_FILE:glslang-standalone> -V ${IN} -o ${SPV}
    DEPENDS ${SRC} glslang-standalone
    COMMENT "glsl->spv ${F}")

  add_custom_command(
    OUTPUT ${MSL} ${GLES} ${REFL}
    COMMAND $<TARGET_FILE:spirv-cross> ${SPV} --msl --output ${MSL}
    COMMAND $<TARGET_FILE:spirv-cross> ${SPV} --version 300 --es --output ${GLES}
    COMMAND $<TARGET_FILE:spirv-cross> ${SPV} --reflect --output ${REFL}
    DEPENDS ${SPV} spirv-cross
    COMMENT "spv->msl/gles/reflect ${F}")

  if(APPLE)
    add_custom_command(
      OUTPUT ${METALLIB}
      COMMAND xcrun -sdk macosx metal -c ${MSL} -o ${AIR}
      COMMAND xcrun -sdk macosx metallib ${AIR} -o ${METALLIB}
      DEPENDS ${MSL}
      COMMENT "msl->metallib ${F}")
    add_custom_target(shader_${F} ALL DEPENDS ${METALLIB} ${GLES} ${REFL})
  else()
    add_custom_target(shader_${F} ALL DEPENDS ${MSL} ${GLES} ${REFL})
  endif()
endfunction()
```

- [ ] **Step 3: 写 cube shader 源**

`shaders/cube.vert`:
```glsl
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(binding = 0) uniform UBO { mat4 mvp; };
layout(location = 0) out vec3 vColor;
void main() {
  gl_Position = mvp * vec4(aPos, 1.0);
  vColor = aColor;
}
```

`shaders/cube.frag`:
```glsl
#version 450
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;
void main() {
  outColor = vec4(vColor, 1.0);
}
```

- [ ] **Step 4: 改写 shaders/CMakeLists.txt**

```cmake
include(${CMAKE_SOURCE_DIR}/cmake/ShaderCompile.cmake)
rd_compile_shader(cube.vert)
rd_compile_shader(cube.frag)

# 生成路径头，供测试定位 shader 产物
file(WRITE ${CMAKE_BINARY_DIR}/generated/rd_shader_dir.h
  "#pragma once\n#define RD_SHADER_DIR \"${RD_SHADER_OUT}\"\n")
```

- [ ] **Step 5: tests/CMakeLists.txt 追加包含目录与测试数据目录定义**

在 `target_link_libraries(rd_tests ...)` 之后追加：
```cmake
target_include_directories(rd_tests PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(rd_tests PRIVATE RD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
```

- [ ] **Step 6: 构建并验证产物**

```bash
cmake -S . -B build 2>&1 | tail -3
cmake --build build -j8 2>&1 | tail -8
ls build/shaders_out/
```
预期存在：`cube.vert.spv cube.vert.metal cube.vert.metallib cube.vert.gles cube.vert.json`（frag 同组）。
人工检查反射 JSON 非空且含 `mvp`：`grep -o mvp build/shaders_out/cube.vert.json`。

- [ ] **Step 7: Commit**

```bash
git add cmake/ShaderCompile.cmake cmake/Deps.cmake shaders/ tests/CMakeLists.txt
git commit -m "build(shaders): GLSL→SPIR-V/MSL/GLSL ES 离线编译管线 + 反射 JSON"
```

---

### Task 9: 图像工具与 stb 集成（golden 基建）

**Files:**
- Create: `tests/common/image.h`
- Create: `tests/common/image.cpp`
- Test: `tests/common/image_test.cpp`
- Modify: `cmake/Deps.cmake`（追加 stb）
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Deps.cmake 追加 stb**

`cmake/Deps.cmake` 末尾追加：
```cmake
FetchContent_Declare(stb
  URL https://github.com/nothings/stb/archive/refs/heads/master.tar.gz)
FetchContent_MakeAvailable(stb)
```
（stb 无版本 tag，锁定 master 快照；如需可复现可在首次构建后记下 commit 并替换为具体 hash。）

- [ ] **Step 2: 写失败测试**

`tests/common/image_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include "common/image.h"
#include <vector>

namespace {
std::vector<uint8_t> solidImage(uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
  std::vector<uint8_t> img(w * h * 4);
  for (uint32_t i = 0; i < w * h; ++i) {
    img[i * 4 + 0] = r;
    img[i * 4 + 1] = g;
    img[i * 4 + 2] = b;
    img[i * 4 + 3] = 255;
  }
  return img;
}
} // namespace

TEST(Image, RoundTripSaveLoad) {
  auto img = solidImage(16, 16, 200, 10, 30);
  std::string path = std::string(RD_TEST_DATA_DIR) + "/../build/image_roundtrip.png";
  ASSERT_TRUE(rd::test::savePNG(path, 16, 16, img.data()));
  auto loaded = rd::test::loadPNG(path);
  ASSERT_EQ(loaded.width, 16u);
  ASSERT_EQ(loaded.height, 16u);
  ASSERT_EQ(loaded.pixels.size(), img.size());
  EXPECT_EQ(loaded.pixels[0], 200);
  EXPECT_EQ(loaded.pixels[1], 10);
  EXPECT_EQ(loaded.pixels[2], 30);
}

TEST(Image, CompareWithinTolerancePasses) {
  auto a = solidImage(8, 8, 100, 100, 100);
  auto b = solidImage(8, 8, 102, 100, 100); // 通道差 2
  auto r = rd::test::compareRGBA8(a.data(), b.data(), 8, 8, /*channelTol=*/3, /*ratioTol=*/0.0);
  EXPECT_TRUE(r.pass);
  EXPECT_EQ(r.maxChannelDiff, 2);
}

TEST(Image, CompareBeyondToleranceFails) {
  auto a = solidImage(8, 8, 100, 100, 100);
  auto b = solidImage(8, 8, 110, 100, 100);
  auto r = rd::test::compareRGBA8(a.data(), b.data(), 8, 8, 3, 0.0);
  EXPECT_FALSE(r.pass);
}

TEST(Image, CompareRatioTolerance) {
  auto a = solidImage(4, 4, 0, 0, 0);
  auto b = a;
  b[0] = 255; // 1/16 像素超差
  EXPECT_FALSE(rd::test::compareRGBA8(a.data(), b.data(), 4, 4, 3, 0.0).pass);
  EXPECT_TRUE(rd::test::compareRGBA8(a.data(), b.data(), 4, 4, 3, 0.10).pass);
}
```

- [ ] **Step 3: 实现 image.h / image.cpp**

`tests/common/image.h`:
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rd::test {

struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels; // RGBA8 紧凑排列
};

bool savePNG(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba);
Image loadPNG(const std::string& path);

struct CompareResult {
  bool pass = false;
  double diffRatio = 0;   // 超差像素占比
  int maxChannelDiff = 0; // 最大单通道差值
};

CompareResult compareRGBA8(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h,
                           int channelTol, double ratioTol);

} // namespace rd::test
```

`tests/common/image.cpp`:
```cpp
#include "common/image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace rd::test {

bool savePNG(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba) {
  return stbi_write_png(path.c_str(), static_cast<int>(width), static_cast<int>(height), 4,
                        rgba, static_cast<int>(width * 4)) != 0;
}

Image loadPNG(const std::string& path) {
  Image img;
  int w = 0, h = 0, channels = 0;
  uint8_t* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
  if (!data) return img;
  img.width = static_cast<uint32_t>(w);
  img.height = static_cast<uint32_t>(h);
  img.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
  stbi_image_free(data);
  return img;
}

CompareResult compareRGBA8(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h,
                           int channelTol, double ratioTol) {
  CompareResult r;
  uint64_t diffPixels = 0;
  const uint64_t total = static_cast<uint64_t>(w) * h;
  for (uint64_t i = 0; i < total; ++i) {
    int maxDiff = 0;
    for (int c = 0; c < 4; ++c) {
      int d = std::abs(static_cast<int>(a[i * 4 + c]) - static_cast<int>(b[i * 4 + c]));
      maxDiff = std::max(maxDiff, d);
    }
    r.maxChannelDiff = std::max(r.maxChannelDiff, maxDiff);
    if (maxDiff > channelTol) ++diffPixels;
  }
  r.diffRatio = total ? static_cast<double>(diffPixels) / static_cast<double>(total) : 0.0;
  r.pass = r.diffRatio <= ratioTol;
  return r;
}

} // namespace rd::test
```

- [ ] **Step 4: tests/CMakeLists.txt 接入**

`add_executable(rd_tests ...)` 源列表追加：
```cmake
  common/image.cpp
  common/image_test.cpp
```
`target_include_directories(rd_tests PRIVATE ...)` 一行改为：
```cmake
target_include_directories(rd_tests PRIVATE
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${stb_SOURCE_DIR})
```
（`tests/rhi/factory_test.cpp` 等使用 `"rhi/..."` 风格包含由 rd_core 的 PUBLIC include 提供；`common/...` 由 `${CMAKE_CURRENT_SOURCE_DIR}` 提供。）

- [ ] **Step 5: 跑测试确认通过**

```bash
cmake -S . -B build 2>&1 | tail -2
cmake --build build -j8 && ctest --test-dir build --output-on-failure -R Image
```
预期：`4 tests ... Passed`。

- [ ] **Step 6: Commit**

```bash
git add cmake/Deps.cmake tests/common/ tests/CMakeLists.txt
git commit -m "test(common): PNG 保存/加载/容差比对工具（stb）"
```

---

### Task 10: Metal 后端 + 共享 cube 渲染器 + golden image 测试

**Files:**
- Create: `core/rhi/backends/metal/metal_device.h`
- Create: `core/rhi/backends/metal/metal_device.mm`
- Create: `tests/common/cube_renderer.h`
- Create: `tests/common/cube_renderer.cpp`
- Test: `tests/rhi/cube_test.cpp`
- Create: `tests/golden/cube_metal.png`（生成后提交）
- Modify: `core/CMakeLists.txt`（APPLE 条件源与 framework 链接）
- Modify: `tests/CMakeLists.txt`（源列表追加 `common/cube_renderer.cpp`、`rhi/cube_test.cpp`）

- [ ] **Step 1: 写共享 cube 渲染器**

`tests/common/cube_renderer.h`:
```cpp
#pragma once
#include "rhi/rhi_device.h"
#include <string>

namespace rd::test {

// 后端无关的顶点色立方体：8 顶点 / 36 索引，凸体 + 背面剔除保证正确遮挡（P0 无需深度缓冲）。
class CubeRenderer {
public:
  bool init(Device& device, const std::string& shaderDir);
  void render(Device& device, TargetHandle target, uint32_t width, uint32_t height, float angleRad);
  void shutdown(Device& device);

private:
  BufferHandle vbo_, ibo_, ubo_;
  ShaderModuleHandle vs_, fs_;
  PipelineHandle pipeline_;
};

} // namespace rd::test
```

`tests/common/cube_renderer.cpp`:
```cpp
#include "common/cube_renderer.h"
#include "foundation/math.h"
#include <fstream>
#include <glm/glm.hpp>

namespace rd::test {
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

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  auto size = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> data(static_cast<size_t>(size));
  f.read(reinterpret_cast<char*>(data.data()), size);
  return data;
}

std::string shaderFile(Backend backend, const char* stage, const std::string& dir) {
  std::string ext = (backend == Backend::Metal) ? ".metallib" : ".spv";
  return dir + "/cube." + stage + ext;
}

} // namespace

bool CubeRenderer::init(Device& device, const std::string& shaderDir) {
  vbo_ = device.createBuffer({sizeof(kVertices), BufferUsage::Vertex, kVertices});
  ibo_ = device.createBuffer({sizeof(kIndices), BufferUsage::Index, kIndices});
  ubo_ = device.createBuffer({64, BufferUsage::Uniform, nullptr});

  // 入口名约定：Metal(metallib, spirv-cross 生成) 为 "main0"；SPIR-V/GLSL ES 保留 "main"
  const char* entry = (device.backend() == Backend::Metal) ? "main0" : "main";

  ShaderModuleDesc vsd;
  vsd.stage = ShaderStage::Vertex;
  vsd.code = readFile(shaderFile(device.backend(), "vert", shaderDir));
  vsd.entryPoint = entry;
  vs_ = device.createShaderModule(vsd);

  ShaderModuleDesc fsd;
  fsd.stage = ShaderStage::Fragment;
  fsd.code = readFile(shaderFile(device.backend(), "frag", shaderDir));
  fsd.entryPoint = entry;
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

void CubeRenderer::render(Device& device, TargetHandle target, uint32_t width, uint32_t height,
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

void CubeRenderer::shutdown(Device& device) {
  device.destroyPipeline(pipeline_);
  device.destroyShaderModule(vs_);
  device.destroyShaderModule(fs_);
  device.destroyBuffer(vbo_);
  device.destroyBuffer(ibo_);
  device.destroyBuffer(ubo_);
}

} // namespace rd::test
```

- [ ] **Step 2: 写失败测试 cube_test.cpp**

`tests/rhi/cube_test.cpp`:
```cpp
#include <gtest/gtest.h>
#include <cstdlib>
#include <cmath>
#include "common/cube_renderer.h"
#include "common/image.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 512, kH = 512;
constexpr float kAngle = 0.78539816f; // 45°

std::string goldenPath(rd::Backend b) {
  std::string name = (b == rd::Backend::Metal) ? "cube_metal.png" : "cube_vulkan.png";
  return std::string(RD_TEST_DATA_DIR) + "/golden/" + name;
}

rd::test::Image renderCube(rd::Backend b) {
  rd::DeviceDesc desc;
  desc.backend = b;
  auto device = rd::createDevice(desc);
  if (!device) return {};
  auto target = device->createOffscreenTarget({kW, kH});
  rd::test::CubeRenderer cube;
  if (!target.valid() || !cube.init(*device, RD_SHADER_DIR)) return {};
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

void expectMatchesGolden(rd::Backend b, const rd::test::Image& img) {
  ASSERT_FALSE(img.pixels.empty());
  std::string path = goldenPath(b);
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(rd::test::savePNG(path, img.width, img.height, img.pixels.data()));
    GTEST_SKIP() << "golden updated: " << path;
  }
  auto golden = rd::test::loadPNG(path);
  ASSERT_EQ(golden.width, img.width) << "golden 缺失？用 RD_UPDATE_GOLDENS=1 生成";
  ASSERT_EQ(golden.height, img.height);
  auto r = rd::test::compareRGBA8(img.pixels.data(), golden.pixels.data(), img.width,
                                  img.height, /*channelTol=*/3, /*ratioTol=*/0.01);
  EXPECT_TRUE(r.pass) << "diffRatio=" << r.diffRatio << " maxChannelDiff=" << r.maxChannelDiff;
}
} // namespace

TEST(Cube, MetalGoldenMatches) {
#if defined(__APPLE__)
  auto img = renderCube(rd::Backend::Metal);
  ASSERT_FALSE(img.pixels.empty()) << "metal 渲染失败";
  expectMatchesGolden(rd::Backend::Metal, img);
#else
  GTEST_SKIP() << "metal 仅 Apple 平台";
#endif
}

TEST(Cube, VulkanGoldenMatches) {
#if defined(RD_WITH_VULKAN)
  auto img = renderCube(rd::Backend::Vulkan);
  ASSERT_FALSE(img.pixels.empty()) << "vulkan 渲染失败";
  expectMatchesGolden(rd::Backend::Vulkan, img);
#else
  GTEST_SKIP() << "vulkan 未编译";
#endif
}

TEST(Cube, CrossBackendConsistent) {
#if defined(__APPLE__) && defined(RD_WITH_VULKAN)
  auto a = renderCube(rd::Backend::Metal);
  auto b = renderCube(rd::Backend::Vulkan);
  ASSERT_FALSE(a.pixels.empty());
  ASSERT_FALSE(b.pixels.empty());
  auto r = rd::test::compareRGBA8(a.pixels.data(), b.pixels.data(), kW, kH, 3, 0.02);
  EXPECT_TRUE(r.pass) << "diffRatio=" << r.diffRatio << " maxChannelDiff=" << r.maxChannelDiff;
#else
  GTEST_SKIP() << "需要双后端";
#endif
}

TEST(Cube, CoversScreenArea) {
#if defined(__APPLE__)
  auto img = renderCube(rd::Backend::Metal);
  ASSERT_FALSE(img.pixels.empty());
  uint64_t covered = 0;
  for (size_t i = 0; i < img.pixels.size(); i += 4) {
    if (std::abs(int(img.pixels[i]) - 26) > 8 || std::abs(int(img.pixels[i + 1]) - 26) > 8 ||
        std::abs(int(img.pixels[i + 2]) - 31) > 8)
      ++covered;
  }
  EXPECT_GT(double(covered) / (kW * kH), 0.05);
#else
  GTEST_SKIP();
#endif
}
```

`tests/CMakeLists.txt` 源列表追加 `common/cube_renderer.cpp`、`rhi/cube_test.cpp`。

- [ ] **Step 3: 跑测试确认失败（红）**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure -R 'Cube.MetalGoldenMatches'
```
预期：`MetalGoldenMatches` FAIL（metal 渲染失败，工厂返回 nullptr）。`VulkanGoldenMatches`/`CrossBackendConsistent` SKIP。

- [ ] **Step 4: 实现 Metal 后端**

`core/rhi/backends/metal/metal_device.h`:
```cpp
#pragma once
#include "rhi/rhi_device.h"

namespace rd {
std::unique_ptr<Device> createMetalDevice(const DeviceDesc& desc);
}
```

`core/rhi/backends/metal/metal_device.mm`:
```objc
#include "metal_device.h"
#include "foundation/log.h"
#import <Metal/Metal.h>
#import <dispatch/dispatch.h>
#include <cstring>
#include <unordered_map>

namespace rd {
namespace {

MTLPixelFormat toMTLPixelFormat(Format f) {
  switch (f) {
    case Format::RGBA8_UNORM: return MTLPixelFormatRGBA8Unorm;
    case Format::BGRA8_UNORM: return MTLPixelFormatBGRA8Unorm;
    case Format::D32_FLOAT:   return MTLPixelFormatDepth32Float;
    default:                  return MTLPixelFormatInvalid;
  }
}

MTLVertexFormat toMTLVertexFormat(Format f) {
  switch (f) {
    case Format::R32G32_FLOAT:        return MTLVertexFormatFloat2;
    case Format::R32G32B32_FLOAT:     return MTLVertexFormatFloat3;
    case Format::R32G32B32A32_FLOAT:  return MTLVertexFormatFloat4;
    case Format::RGBA8_UNORM:         return MTLVertexFormatUChar4Normalized;
    default:                          return MTLVertexFormatInvalid;
  }
}

struct BufferRec { id<MTLBuffer> buffer; };
struct ShaderRec { id<MTLLibrary> library; std::string entry; };
struct PipelineRec { id<MTLRenderPipelineState> state; MTLPrimitiveType topology; MTLCullMode cull; };
struct TargetRec { id<MTLTexture> color; uint32_t width; uint32_t height; };

class MetalDevice;

class MetalCommandBuffer final : public CommandBuffer {
public:
  explicit MetalCommandBuffer(MetalDevice* device) : device_(device) {}
  void beginRenderPass(TargetHandle target, const ClearColor& clear) override;
  void bindPipeline(PipelineHandle pipeline) override;
  void bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) override;
  void bindIndexBuffer(BufferHandle buffer, uint64_t offset) override;
  void bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t size) override;
  void draw(uint32_t vertexCount, uint32_t firstVertex) override;
  void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset) override;
  void endRenderPass() override;

  MetalDevice* device_;
  id<MTLCommandBuffer> cmd_ = nil;
  id<MTLRenderCommandEncoder> encoder_ = nil;
  PipelineRec pipeline_{};
  BufferHandle indexBuffer_;
  uint64_t indexOffset_ = 0;
};

class MetalDevice final : public Device {
public:
  ~MetalDevice() override { waitIdle(); }

  bool init(const DeviceDesc&) {
    device_ = MTLCreateSystemDefaultDevice();
    if (!device_) {
      RD_LOGE("rhi.metal", "MTLCreateSystemDefaultDevice 失败");
      return false;
    }
    queue_ = [device_ newCommandQueue];
    return queue_ != nil;
  }

  Backend backend() const override { return Backend::Metal; }

  BufferHandle createBuffer(const BufferDesc& desc) override {
    id<MTLBuffer> b = [device_ newBufferWithLength:desc.size
                                           options:MTLResourceStorageModeShared];
    if (!b) return {};
    if (desc.data) memcpy(b.contents, desc.data, desc.size);
    BufferHandle h(nextId_++);
    buffers_.emplace(h, BufferRec{b});
    return h;
  }

  void updateBuffer(BufferHandle buffer, const void* data, uint64_t size, uint64_t offset) override {
    auto it = buffers_.find(buffer);
    if (it == buffers_.end()) return;
    memcpy(static_cast<uint8_t*>(it->second.buffer.contents) + offset, data, size);
  }

  void destroyBuffer(BufferHandle buffer) override { buffers_.erase(buffer); }

  ShaderModuleHandle createShaderModule(const ShaderModuleDesc& desc) override {
    void* copy = malloc(desc.code.size());
    memcpy(copy, desc.code.data(), desc.code.size());
    dispatch_data_t d = dispatch_data_create(copy, desc.code.size(),
        dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), DISPATCH_DATA_DESTRUCTOR_FREE);
    NSError* err = nil;
    id<MTLLibrary> lib = [device_ newLibraryWithData:d error:&err];
    if (!lib) {
      RD_LOGE("rhi.metal", "metallib 加载失败: %s",
              err ? err.localizedDescription.UTF8String : "unknown");
      return {};
    }
    ShaderModuleHandle h(nextId_++);
    shaders_.emplace(h, ShaderRec{lib, desc.entryPoint});
    return h;
  }

  void destroyShaderModule(ShaderModuleHandle module) override { shaders_.erase(module); }

  PipelineHandle createPipeline(const PipelineDesc& desc) override {
    if (desc.depthTest) {
      RD_LOGE("rhi.metal", "P0-1 离屏目标不支持 depthTest（P1 引入深度附件）");
      return {};
    }
    auto vsIt = shaders_.find(desc.vertexShader);
    auto fsIt = shaders_.find(desc.fragmentShader);
    if (vsIt == shaders_.end() || fsIt == shaders_.end()) return {};

    MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = [vsIt->second.library
        newFunctionWithName:@(vsIt->second.entry.c_str())];
    pd.fragmentFunction = [fsIt->second.library
        newFunctionWithName:@(fsIt->second.entry.c_str())];
    pd.colorAttachments[0].pixelFormat = toMTLPixelFormat(desc.colorFormat);
    if (!pd.vertexFunction || !pd.fragmentFunction) return {};

    MTLVertexDescriptor* vd = [[MTLVertexDescriptor alloc] init];
    for (const auto& a : desc.attributes) {
      vd.attributes[a.location].format = toMTLVertexFormat(a.format);
      vd.attributes[a.location].offset = a.offset;
      vd.attributes[a.location].bufferIndex = a.binding + 1; // 约定：0 留给 uniform
    }
    for (const auto& b : desc.vertexBindings) {
      vd.layouts[b.binding + 1].stride = b.stride;
      vd.layouts[b.binding + 1].stepFunction = MTLVertexStepFunctionPerVertex;
    }
    pd.vertexDescriptor = vd;

    NSError* err = nil;
    id<MTLRenderPipelineState> state =
        [device_ newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!state) {
      RD_LOGE("rhi.metal", "pipeline 创建失败: %s",
              err ? err.localizedDescription.UTF8String : "unknown");
      return {};
    }
    PipelineRec rec;
    rec.state = state;
    rec.topology = desc.topology == PrimitiveTopology::TriangleList  ? MTLPrimitiveTypeTriangle
                   : desc.topology == PrimitiveTopology::TriangleStrip ? MTLPrimitiveTypeTriangleStrip
                                                                       : MTLPrimitiveTypeLine;
    rec.cull = desc.cullMode == CullMode::Back  ? MTLCullModeBack
               : desc.cullMode == CullMode::Front ? MTLCullModeFront
                                                  : MTLCullModeNone;
    PipelineHandle h(nextId_++);
    pipelines_.emplace(h, rec);
    return h;
  }

  void destroyPipeline(PipelineHandle pipeline) override { pipelines_.erase(pipeline); }

  TargetHandle createOffscreenTarget(const OffscreenTargetDesc& desc) override {
    MTLTextureDescriptor* td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:toMTLPixelFormat(desc.colorFormat)
                                                           width:desc.width
                                                          height:desc.height
                                                       mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared; // macOS 统一内存，便于 readback
    id<MTLTexture> tex = [device_ newTextureWithDescriptor:td];
    if (!tex) return {};
    TargetHandle h(nextId_++);
    targets_.emplace(h, TargetRec{tex, desc.width, desc.height});
    return h;
  }

  void destroyTarget(TargetHandle target) override { targets_.erase(target); }

  bool readbackTarget(TargetHandle target, void* outRGBA8, uint64_t outSize) override {
    auto it = targets_.find(target);
    if (it == targets_.end()) return false;
    const TargetRec& t = it->second;
    if (outSize < uint64_t(t.width) * t.height * 4) return false;
    waitIdle();
    [t.color getBytes:outRGBA8
          bytesPerRow:t.width * 4
           fromRegion:MTLRegionMake2D(0, 0, t.width, t.height)
          mipmapLevel:0];
    return true;
  }

  CommandBuffer* acquireCommandBuffer() override {
    cmdBuf_.cmd_ = [queue_ commandBuffer];
    return &cmdBuf_;
  }

  void submit(CommandBuffer*) override {
    [cmdBuf_.cmd_ commit];
    lastCmd_ = cmdBuf_.cmd_;
  }

  void waitIdle() override {
    if (lastCmd_) {
      [lastCmd_ waitUntilCompleted];
      lastCmd_ = nil;
    }
  }

  // ---- CommandBuffer 访问的内部状态 ----
  id<MTLBuffer> buffer(BufferHandle h) const {
    auto it = buffers_.find(h);
    return it == buffers_.end() ? nil : it->second.buffer;
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
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLCommandBuffer> lastCmd_ = nil;
  MetalCommandBuffer cmdBuf_{this};
  uint32_t nextId_ = 1;
  std::unordered_map<BufferHandle, BufferRec> buffers_;
  std::unordered_map<ShaderModuleHandle, ShaderRec> shaders_;
  std::unordered_map<PipelineHandle, PipelineRec> pipelines_;
  std::unordered_map<TargetHandle, TargetRec> targets_;
};

void MetalCommandBuffer::beginRenderPass(TargetHandle target, const ClearColor& clear) {
  TargetRec t;
  if (!device_->target(target, t)) return;
  MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
  rp.colorAttachments[0].texture = t.color;
  rp.colorAttachments[0].loadAction = MTLLoadActionClear;
  rp.colorAttachments[0].clearColor = MTLClearColorMake(clear.r, clear.g, clear.b, clear.a);
  rp.colorAttachments[0].storeAction = MTLStoreActionStore;
  encoder_ = [cmd_ renderCommandEncoderWithDescriptor:rp];
  MTLViewport vp{0, 0, double(t.width), double(t.height), 0, 1};
  [encoder_ setViewport:vp];
}

void MetalCommandBuffer::bindPipeline(PipelineHandle pipeline) {
  if (!device_->pipeline(pipeline, pipeline_)) return;
  [encoder_ setRenderPipelineState:pipeline_.state];
  [encoder_ setCullMode:pipeline_.cull];
}

void MetalCommandBuffer::bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) {
  [encoder_ setVertexBuffer:device_->buffer(buffer) offset:offset atIndex:binding + 1];
}

void MetalCommandBuffer::bindIndexBuffer(BufferHandle buffer, uint64_t offset) {
  indexBuffer_ = buffer;
  indexOffset_ = offset;
}

void MetalCommandBuffer::bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset,
                                           uint64_t) {
  id<MTLBuffer> b = device_->buffer(buffer);
  [encoder_ setVertexBuffer:b offset:offset atIndex:slot];
  [encoder_ setFragmentBuffer:b offset:offset atIndex:slot];
}

void MetalCommandBuffer::draw(uint32_t vertexCount, uint32_t firstVertex) {
  [encoder_ drawPrimitives:pipeline_.topology vertexStart:firstVertex vertexCount:vertexCount];
}

void MetalCommandBuffer::drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t) {
  [encoder_ drawIndexedPrimitives:pipeline_.topology
                       indexCount:indexCount
                        indexType:MTLIndexTypeUInt16
                      indexBuffer:device_->buffer(indexBuffer_)
                indexBufferOffset:indexOffset_ + firstIndex * 2];
}

void MetalCommandBuffer::endRenderPass() { [encoder_ endEncoding]; }

} // namespace

std::unique_ptr<Device> createMetalDevice(const DeviceDesc& desc) {
  auto device = std::make_unique<MetalDevice>();
  if (!device->init(desc)) return nullptr;
  return device;
}

} // namespace rd
```

`core/CMakeLists.txt` 末尾追加：
```cmake
if(APPLE)
  target_sources(rd_core PRIVATE rhi/backends/metal/metal_device.mm)
  set_source_files_properties(rhi/backends/metal/metal_device.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_compile_definitions(rd_core PRIVATE RD_WITH_METAL=1)
  target_link_libraries(rd_core PRIVATE "-framework Metal" "-framework Foundation")
endif()
```

- [ ] **Step 5: 构建并生成 golden，人工核验图像**

```bash
cmake -S . -B build 2>&1 | tail -2
cmake --build build -j8 2>&1 | tail -5
mkdir -p tests/golden
RD_UPDATE_GOLDENS=1 ctest --test-dir build --output-on-failure -R 'Cube.MetalGoldenMatches'
```
预期：测试 SKIP 并提示 golden 已写入 `tests/golden/cube_metal.png`。
用 Read 工具打开该 PNG 目视确认：**顶点色立方体、暗色背景、无花屏**。

- [ ] **Step 6: 跑全部 cube 测试确认通过（绿）**

```bash
ctest --test-dir build --output-on-failure -R Cube
```
预期：`MetalGoldenMatches`、`CoversScreenArea` PASS；`VulkanGoldenMatches`、`CrossBackendConsistent` SKIP。

- [ ] **Step 7: Commit**

```bash
git add core/rhi/backends/metal/ core/CMakeLists.txt tests/common/cube_renderer.* tests/rhi/cube_test.cpp tests/CMakeLists.txt tests/golden/cube_metal.png
git commit -m "feat(rhi/metal): Metal 后端（离屏渲染）+ cube golden image 测试"
```

---

### Task 11: Vulkan 后端（MoltenVK）+ 跨后端一致性

**Files:**
- Create: `core/rhi/backends/vulkan/vulkan_device.h`
- Create: `core/rhi/backends/vulkan/vulkan_device.cpp`
- Create: `tests/golden/cube_vulkan.png`（生成后提交）
- Modify: `cmake/Deps.cmake`（追加 Vulkan-Headers）
- Modify: `core/CMakeLists.txt`（Vulkan 条件编译）
- Modify: `tests/CMakeLists.txt`（`RD_WITH_VULKAN` 定义 + rpath）

- [ ] **Step 1: Deps.cmake 追加 Vulkan-Headers**

`cmake/Deps.cmake` 末尾追加：
```cmake
FetchContent_Declare(VulkanHeaders
  URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz)
FetchContent_MakeAvailable(VulkanHeaders)
```

- [ ] **Step 2: core/CMakeLists.txt 追加 Vulkan 条件段**

`core/CMakeLists.txt` 末尾追加：
```cmake
find_library(RD_MOLTENVK_LIB MoltenVK
  PATHS /opt/homebrew/lib /usr/local/lib "$ENV{VULKAN_SDK}/lib")
if(RD_MOLTENVK_LIB)
  message(STATUS "MoltenVK found: ${RD_MOLTENVK_LIB}")
  target_sources(rd_core PRIVATE rhi/backends/vulkan/vulkan_device.cpp)
  target_compile_definitions(rd_core PUBLIC RD_WITH_VULKAN=1)
  target_link_libraries(rd_core PRIVATE Vulkan::Headers ${RD_MOLTENVK_LIB})
endif()
```
（`RD_WITH_VULKAN` 用 PUBLIC：rd_tests 的 `#if defined(RD_WITH_VULKAN)` 需要。）

`tests/CMakeLists.txt` 末尾追加：
```cmake
set_target_properties(rd_tests PROPERTIES
  BUILD_RPATH "/opt/homebrew/lib;/usr/local/lib;$ENV{VULKAN_SDK}/lib")
```

- [ ] **Step 3: 跑测试确认失败（红）**

```bash
cmake -S . -B build 2>&1 | grep -i moltenvk
cmake --build build -j8 2>&1 | tail -5
```
预期：找到 MoltenVK；链接失败 `undefined symbol: rd::createVulkanDevice(...)`（工厂已引用但实现未写）。这就是红。

- [ ] **Step 4: 实现 Vulkan 后端**

`core/rhi/backends/vulkan/vulkan_device.h`:
```cpp
#pragma once
#include "rhi/rhi_device.h"

namespace rd {
std::unique_ptr<Device> createVulkanDevice(const DeviceDesc& desc);
}
```

`core/rhi/backends/vulkan/vulkan_device.cpp`:
```cpp
#include "vulkan_device.h"
#include "foundation/log.h"
#include <vulkan/vulkan.h>
#include <cstring>
#include <unordered_map>
#include <vector>

#define VK_CHECK(x)                                                    \
  do {                                                                 \
    VkResult res_ = (x);                                               \
    if (res_ != VK_SUCCESS) {                                          \
      RD_LOGE("rhi.vk", "%s 失败 (%d) @%d", #x, int(res_), __LINE__);  \
      return false;                                                    \
    }                                                                  \
  } while (0)

namespace rd {
namespace {

VkFormat toVkFormat(Format f) {
  switch (f) {
    case Format::RGBA8_UNORM:        return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::BGRA8_UNORM:        return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::R32G32_FLOAT:       return VK_FORMAT_R32G32_SFLOAT;
    case Format::R32G32B32_FLOAT:    return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::D32_FLOAT:          return VK_FORMAT_D32_SFLOAT;
  }
  return VK_FORMAT_UNDEFINED;
}

struct BufferRec { VkBuffer buffer; VkDeviceMemory memory; };
struct ShaderRec { VkShaderModule module; ShaderStage stage; std::string entry; };
struct PipelineRec { VkPipeline pipeline; VkPipelineLayout layout; VkPrimitiveTopology topology; VkCullModeFlags cull; };
struct TargetRec {
  VkImage color; VkDeviceMemory colorMem; VkImageView view; VkFramebuffer fb;
  VkImage staging; VkDeviceMemory stagingMem;
  uint32_t width, height;
  VkDeviceSize stagingRowPitch;
};

constexpr uint32_t kMaxUniformSlots = 4;

class VulkanDevice;

class VulkanCommandBuffer final : public CommandBuffer {
public:
  explicit VulkanCommandBuffer(VulkanDevice* device) : device_(device) {}
  void beginRenderPass(TargetHandle target, const ClearColor& clear) override;
  void bindPipeline(PipelineHandle pipeline) override;
  void bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) override;
  void bindIndexBuffer(BufferHandle buffer, uint64_t offset) override;
  void bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset, uint64_t size) override;
  void draw(uint32_t vertexCount, uint32_t firstVertex) override;
  void drawIndexed(uint32_t indexCount, uint32_t firstIndex, int32_t vertexOffset) override;
  void endRenderPass() override;

  VulkanDevice* device_;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  TargetHandle currentTarget_;
  VkPipelineLayout currentLayout_ = VK_NULL_HANDLE;
  VkPrimitiveTopology topology_ = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
};

class VulkanDevice final : public Device {
public:
  ~VulkanDevice() override;
  bool init(const DeviceDesc& desc);
  Backend backend() const override { return Backend::Vulkan; }

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
  CommandBuffer* acquireCommandBuffer() override;
  void submit(CommandBuffer* cmd) override;
  void waitIdle() override;

  // ---- CommandBuffer 访问 ----
  VkBuffer buffer(BufferHandle h) const {
    auto it = buffers_.find(h);
    return it == buffers_.end() ? VK_NULL_HANDLE : it->second.buffer;
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
  VkRenderPass renderPass() const { return renderPass_; }
  VkDescriptorSet descriptorSet() const { return descSet_; }
  VkDevice device() const { return device_; }
  void writeUniformDescriptor(uint32_t slot, VkBuffer buffer, uint64_t offset, uint64_t size) {
    VkDescriptorBufferInfo info{buffer, offset, size};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descSet_;
    write.dstBinding = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

private:
  uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;
  bool createImage(uint32_t w, uint32_t h, VkFormat format, VkImageTiling tiling,
                   VkImageUsageFlags usage, VkMemoryPropertyFlags memProps, VkImage& image,
                   VkDeviceMemory& memory);

  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool cmdPool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool descPool_ = VK_NULL_HANDLE;
  VkDescriptorSet descSet_ = VK_NULL_HANDLE;
  VulkanCommandBuffer cmdBuf_{this};
  uint32_t nextId_ = 1;
  std::unordered_map<BufferHandle, BufferRec> buffers_;
  std::unordered_map<ShaderModuleHandle, ShaderRec> shaders_;
  std::unordered_map<PipelineHandle, PipelineRec> pipelines_;
  std::unordered_map<TargetHandle, TargetRec> targets_;
};

// ---------------- Device 初始化 ----------------

bool VulkanDevice::init(const DeviceDesc& desc) {
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "rdrenderer";
  app.apiVersion = VK_API_VERSION_1_1;

  std::vector<const char*> extensions;
  VkInstanceCreateFlags flags = 0;
  // 注意：macOS 上我们直接链接 libMoltenVK.dylib（ICD），不经过 Vulkan Loader，
  // 因此不需要也不能请求 VK_KHR_portability_enumeration（那是 loader 层扩展）。
  // 若日后改为经 Loader 加载 MoltenVK，需恢复该扩展与 ENUMERATE_PORTABILITY_BIT flag。

  std::vector<const char*> layers;
  if (desc.enableValidation) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> available(count);
    vkEnumerateInstanceLayerProperties(&count, available.data());
    for (const auto& l : available) {
      if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        break;
      }
    }
  }

  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.flags = flags;
  ici.pApplicationInfo = &app;
  ici.enabledExtensionCount = uint32_t(extensions.size());
  ici.ppEnabledExtensionNames = extensions.data();
  ici.enabledLayerCount = uint32_t(layers.size());
  ici.ppEnabledLayerNames = layers.data();
  VK_CHECK(vkCreateInstance(&ici, nullptr, &instance_));

  uint32_t physCount = 0;
  vkEnumeratePhysicalDevices(instance_, &physCount, nullptr);
  if (physCount == 0) {
    RD_LOGE("rhi.vk", "无物理设备");
    return false;
  }
  std::vector<VkPhysicalDevice> physList(physCount);
  vkEnumeratePhysicalDevices(instance_, &physCount, physList.data());

  for (auto phys : physList) {
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &familyCount, families.data());
    for (uint32_t i = 0; i < familyCount; ++i) {
      if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        phys_ = phys;
        queueFamily_ = i;
        break;
      }
    }
    if (phys_) break;
  }
  if (!phys_) {
    RD_LOGE("rhi.vk", "无图形队列");
    return false;
  }

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queueFamily_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;

  std::vector<const char*> deviceExtensions;
#if defined(__APPLE__)
  uint32_t extCount = 0;
  vkEnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, nullptr);
  std::vector<VkExtensionProperties> exts(extCount);
  vkEnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, exts.data());
  for (const auto& e : exts) {
    if (strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) {
      deviceExtensions.push_back("VK_KHR_portability_subset");
      break;
    }
  }
#endif

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = uint32_t(deviceExtensions.size());
  dci.ppEnabledExtensionNames = deviceExtensions.data();
  VK_CHECK(vkCreateDevice(phys_, &dci, nullptr, &device_));
  vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

  VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = queueFamily_;
  VK_CHECK(vkCreateCommandPool(device_, &cpci, nullptr, &cmdPool_));

  VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cbai.commandPool = cmdPool_;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VK_CHECK(vkAllocateCommandBuffers(device_, &cbai, &cmd_));

  // 离屏 render pass：单颜色附件，结束后转为 TRANSFER_SRC 供 readback 拷贝
  VkAttachmentDescription color{};
  color.format = VK_FORMAT_R8G8B8A8_UNORM;
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

  // uniform 描述符：set 0，binding 0..3
  VkDescriptorSetLayoutBinding bindings[kMaxUniformSlots]{};
  for (uint32_t i = 0; i < kMaxUniformSlots; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dslci.bindingCount = kMaxUniformSlots;
  dslci.pBindings = bindings;
  VK_CHECK(vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &setLayout_));

  VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kMaxUniformSlots};
  VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &poolSize;
  VK_CHECK(vkCreateDescriptorPool(device_, &dpci, nullptr, &descPool_));

  VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dsai.descriptorPool = descPool_;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &setLayout_;
  VK_CHECK(vkAllocateDescriptorSets(device_, &dsai, &descSet_));
  return true;
}

VulkanDevice::~VulkanDevice() {
  if (!device_) return;
  vkDeviceWaitIdle(device_);
  vkDestroyDescriptorPool(device_, descPool_, nullptr);
  vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr);
  vkDestroyRenderPass(device_, renderPass_, nullptr);
  vkDestroyCommandPool(device_, cmdPool_, nullptr);
  vkDestroyDevice(device_, nullptr);
  vkDestroyInstance(instance_, nullptr);
}

uint32_t VulkanDevice::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(phys_, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
  }
  return UINT32_MAX;
}

bool VulkanDevice::createImage(uint32_t w, uint32_t h, VkFormat format, VkImageTiling tiling,
                               VkImageUsageFlags usage, VkMemoryPropertyFlags memProps,
                               VkImage& image, VkDeviceMemory& memory) {
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = format;
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = tiling;
  ici.usage = usage;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VK_CHECK(vkCreateImage(device_, &ici, nullptr, &image));

  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device_, image, &req);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, memProps);
  if (mai.memoryTypeIndex == UINT32_MAX) return false;
  VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &memory));
  VK_CHECK(vkBindImageMemory(device_, image, memory, 0));
  return true;
}

// ---------------- 资源 ----------------

BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc) {
  VkBufferUsageFlags usage = 0;
  if (hasFlag(desc.usage, BufferUsage::Vertex)) usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (hasFlag(desc.usage, BufferUsage::Index)) usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (hasFlag(desc.usage, BufferUsage::Uniform)) usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = desc.size;
  bci.usage = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer;
  if (vkCreateBuffer(device_, &bci, nullptr, &buffer) != VK_SUCCESS) return {};

  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, buffer, &req);
  // P0 简化：全部 HOST_VISIBLE|COHERENT（P1 再引入 device-local + staging）
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex =
      findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory memory;
  if (mai.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device_, &mai, nullptr, &memory) != VK_SUCCESS ||
      vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
    return {};
  }
  BufferHandle h(nextId_++);
  buffers_.emplace(h, BufferRec{buffer, memory});
  if (desc.data) updateBuffer(h, desc.data, desc.size, 0);
  return h;
}

void VulkanDevice::updateBuffer(BufferHandle buffer, const void* data, uint64_t size,
                                uint64_t offset) {
  auto it = buffers_.find(buffer);
  if (it == buffers_.end()) return;
  void* mapped = nullptr;
  vkMapMemory(device_, it->second.memory, offset, size, 0, &mapped);
  memcpy(mapped, data, size);
  vkUnmapMemory(device_, it->second.memory);
}

void VulkanDevice::destroyBuffer(BufferHandle buffer) {
  auto it = buffers_.find(buffer);
  if (it == buffers_.end()) return;
  vkDestroyBuffer(device_, it->second.buffer, nullptr);
  vkFreeMemory(device_, it->second.memory, nullptr);
  buffers_.erase(it);
}

ShaderModuleHandle VulkanDevice::createShaderModule(const ShaderModuleDesc& desc) {
  VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  smci.codeSize = desc.code.size();
  smci.pCode = reinterpret_cast<const uint32_t*>(desc.code.data());
  VkShaderModule module;
  if (vkCreateShaderModule(device_, &smci, nullptr, &module) != VK_SUCCESS) return {};
  ShaderModuleHandle h(nextId_++);
  shaders_.emplace(h, ShaderRec{module, desc.stage, desc.entryPoint});
  return h;
}

void VulkanDevice::destroyShaderModule(ShaderModuleHandle module) {
  auto it = shaders_.find(module);
  if (it == shaders_.end()) return;
  vkDestroyShaderModule(device_, it->second.module, nullptr);
  shaders_.erase(it);
}

PipelineHandle VulkanDevice::createPipeline(const PipelineDesc& desc) {
  if (desc.depthTest) {
    RD_LOGE("rhi.vk", "P0-1 离屏目标不支持 depthTest（P1 引入深度附件）");
    return {};
  }
  auto vsIt = shaders_.find(desc.vertexShader);
  auto fsIt = shaders_.find(desc.fragmentShader);
  if (vsIt == shaders_.end() || fsIt == shaders_.end()) return {};

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vsIt->second.module;
  stages[0].pName = vsIt->second.entry.c_str();
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fsIt->second.module;
  stages[1].pName = fsIt->second.entry.c_str();

  std::vector<VkVertexInputBindingDescription> bindings;
  for (const auto& b : desc.vertexBindings) {
    bindings.push_back({b.binding, b.stride, VK_VERTEX_INPUT_RATE_VERTEX});
  }
  std::vector<VkVertexInputAttributeDescription> attribs;
  for (const auto& a : desc.attributes) {
    attribs.push_back({a.location, a.binding, toVkFormat(a.format), a.offset});
  }
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = uint32_t(bindings.size());
  vi.pVertexBindingDescriptions = bindings.data();
  vi.vertexAttributeDescriptionCount = uint32_t(attribs.size());
  vi.pVertexAttributeDescriptions = attribs.data();

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  if (desc.topology == PrimitiveTopology::TriangleStrip) topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  if (desc.topology == PrimitiveTopology::LineList) topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  ia.topology = topology;

  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  VkCullModeFlags cull = VK_CULL_MODE_NONE;
  if (desc.cullMode == CullMode::Back) cull = VK_CULL_MODE_BACK_BIT;
  if (desc.cullMode == CullMode::Front) cull = VK_CULL_MODE_FRONT_BIT;
  rs.cullMode = cull;
  // 视口用负高度做 y 翻转（对齐 Metal/GLES 的 y-up NDC），翻转后绕序变 CW
  rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rs.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = VK_FALSE;
  ds.depthWriteEnable = VK_FALSE;

  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &blendAttachment;

  VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dynamicStates;

  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &setLayout_;
  VkPipelineLayout layout;
  if (vkCreatePipelineLayout(device_, &plci, nullptr, &layout) != VK_SUCCESS) return {};

  VkGraphicsPipelineCreateInfo gpci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpci.stageCount = 2;
  gpci.pStages = stages;
  gpci.pVertexInputState = &vi;
  gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vp;
  gpci.pRasterizationState = &rs;
  gpci.pMultisampleState = &ms;
  gpci.pDepthStencilState = &ds;
  gpci.pColorBlendState = &cb;
  gpci.pDynamicState = &dyn;
  gpci.layout = layout;
  gpci.renderPass = renderPass_;
  VkPipeline pipeline;
  if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpci, nullptr, &pipeline) !=
      VK_SUCCESS) {
    vkDestroyPipelineLayout(device_, layout, nullptr);
    return {};
  }
  PipelineHandle h(nextId_++);
  pipelines_.emplace(h, PipelineRec{pipeline, layout, topology, cull});
  return h;
}

void VulkanDevice::destroyPipeline(PipelineHandle pipeline) {
  auto it = pipelines_.find(pipeline);
  if (it == pipelines_.end()) return;
  vkDestroyPipeline(device_, it->second.pipeline, nullptr);
  vkDestroyPipelineLayout(device_, it->second.layout, nullptr);
  pipelines_.erase(it);
}

TargetHandle VulkanDevice::createOffscreenTarget(const OffscreenTargetDesc& desc) {
  TargetRec rec{};
  rec.width = desc.width;
  rec.height = desc.height;
  if (!createImage(desc.width, desc.height, toVkFormat(desc.colorFormat),
                   VK_IMAGE_TILING_OPTIMAL,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, rec.color, rec.colorMem)) {
    return {};
  }

  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = rec.color;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = toVkFormat(desc.colorFormat);
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device_, &vci, nullptr, &rec.view) != VK_SUCCESS) return {};

  VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbci.renderPass = renderPass_;
  fbci.attachmentCount = 1;
  fbci.pAttachments = &rec.view;
  fbci.width = desc.width;
  fbci.height = desc.height;
  fbci.layers = 1;
  if (vkCreateFramebuffer(device_, &fbci, nullptr, &rec.fb) != VK_SUCCESS) return {};

  if (!createImage(desc.width, desc.height, toVkFormat(desc.colorFormat), VK_IMAGE_TILING_LINEAR,
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                   rec.staging, rec.stagingMem)) {
    return {};
  }
  VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
  VkSubresourceLayout layout;
  vkGetImageSubresourceLayout(device_, rec.staging, &sub, &layout);
  rec.stagingRowPitch = layout.rowPitch;

  TargetHandle h(nextId_++);
  targets_.emplace(h, rec);
  return h;
}

void VulkanDevice::destroyTarget(TargetHandle target) {
  auto it = targets_.find(target);
  if (it == targets_.end()) return;
  const TargetRec& t = it->second;
  vkDestroyFramebuffer(device_, t.fb, nullptr);
  vkDestroyImageView(device_, t.view, nullptr);
  vkDestroyImage(device_, t.color, nullptr);
  vkFreeMemory(device_, t.colorMem, nullptr);
  vkDestroyImage(device_, t.staging, nullptr);
  vkFreeMemory(device_, t.stagingMem, nullptr);
  targets_.erase(it);
}

bool VulkanDevice::readbackTarget(TargetHandle target, void* outRGBA8, uint64_t outSize) {
  auto it = targets_.find(target);
  if (it == targets_.end()) return false;
  const TargetRec& t = it->second;
  const uint64_t rowBytes = uint64_t(t.width) * 4;
  if (outSize < rowBytes * t.height) return false;
  waitIdle();
  void* mapped = nullptr;
  if (vkMapMemory(device_, t.stagingMem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return false;
  const uint8_t* src = static_cast<const uint8_t*>(mapped);
  uint8_t* dst = static_cast<uint8_t*>(outRGBA8);
  for (uint32_t y = 0; y < t.height; ++y) {
    memcpy(dst + rowBytes * y, src + t.stagingRowPitch * y, rowBytes);
  }
  vkUnmapMemory(device_, t.stagingMem);
  return true;
}

CommandBuffer* VulkanDevice::acquireCommandBuffer() {
  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd_, &bi);
  cmdBuf_.cmd_ = cmd_;
  return &cmdBuf_;
}

void VulkanDevice::submit(CommandBuffer*) {
  vkEndCommandBuffer(cmd_);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
}

void VulkanDevice::waitIdle() { vkQueueWaitIdle(queue_); }

// ---------------- CommandBuffer 录制 ----------------

void VulkanCommandBuffer::beginRenderPass(TargetHandle target, const ClearColor& clear) {
  TargetRec t;
  if (!device_->target(target, t)) return;
  currentTarget_ = target;

  VkClearValue clearValue{};
  clearValue.color = {{clear.r, clear.g, clear.b, clear.a}};
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = device_->renderPass();
  rp.framebuffer = t.fb;
  rp.renderArea = {{0, 0}, {t.width, t.height}};
  rp.clearValueCount = 1;
  rp.pClearValues = &clearValue;
  vkCmdBeginRenderPass(cmd_, &rp, VK_SUBPASS_CONTENTS_INLINE);

  // 负高度视口：y 翻转对齐 Metal/GLES
  VkViewport viewport{0, float(t.height), float(t.width), -float(t.height), 0, 1};
  vkCmdSetViewport(cmd_, 0, 1, &viewport);
  VkRect2D scissor{{0, 0}, {t.width, t.height}};
  vkCmdSetScissor(cmd_, 0, 1, &scissor);
}

void VulkanCommandBuffer::bindPipeline(PipelineHandle pipeline) {
  PipelineRec rec;
  if (!device_->pipeline(pipeline, rec)) return;
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, rec.pipeline);
  currentLayout_ = rec.layout;
  topology_ = rec.topology;
  VkDescriptorSet set = device_->descriptorSet();
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, rec.layout, 0, 1,
                          &set, 0, nullptr);
}

void VulkanCommandBuffer::bindVertexBuffer(uint32_t binding, BufferHandle buffer, uint64_t offset) {
  VkBuffer b = device_->buffer(buffer);
  vkCmdBindVertexBuffers(cmd_, binding, 1, &b, &offset);
}

void VulkanCommandBuffer::bindIndexBuffer(BufferHandle buffer, uint64_t offset) {
  vkCmdBindIndexBuffer(cmd_, device_->buffer(buffer), offset, VK_INDEX_TYPE_UINT16);
}

void VulkanCommandBuffer::bindUniformBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset,
                                            uint64_t size) {
  device_->writeUniformDescriptor(slot, device_->buffer(buffer), offset, size);
}

void VulkanCommandBuffer::draw(uint32_t vertexCount, uint32_t firstVertex) {
  vkCmdDraw(cmd_, vertexCount, 1, firstVertex, 0);
}

void VulkanCommandBuffer::drawIndexed(uint32_t indexCount, uint32_t firstIndex,
                                      int32_t vertexOffset) {
  vkCmdDrawIndexed(cmd_, indexCount, 1, firstIndex, vertexOffset, 0);
}

void VulkanCommandBuffer::endRenderPass() {
  vkCmdEndRenderPass(cmd_);

  TargetRec t;
  if (!device_->target(currentTarget_, t)) return;
  // staging: UNDEFINED -> TRANSFER_DST，拷贝后 -> GENERAL（供 host 读）
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.image = t.staging;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &barrier);

  VkImageCopy copy{};
  copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.extent = {t.width, t.height, 1};
  vkCmdCopyImage(cmd_, t.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.staging,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &barrier);
}

} // namespace

std::unique_ptr<Device> createVulkanDevice(const DeviceDesc& desc) {
  auto device = std::make_unique<VulkanDevice>();
  if (!device->init(desc)) return nullptr;
  return device;
}

} // namespace rd
```

- [ ] **Step 5: 构建、生成 golden、人工核验**

```bash
cmake --build build -j8 2>&1 | tail -5
RD_UPDATE_GOLDENS=1 ctest --test-dir build --output-on-failure -R 'Cube.VulkanGoldenMatches'
```
预期：SKIP 并提示写入 `tests/golden/cube_vulkan.png`。用 Read 工具目视核验：与 Metal 图几乎一致。

- [ ] **Step 6: 全量测试（含跨后端一致性）**

```bash
ctest --test-dir build --output-on-failure
```
预期：全部 PASS（无 FAIL；GLES 相关 SKIP 属预期）。重点确认 `Cube.CrossBackendConsistent` PASS。

- [ ] **Step 7: Commit**

```bash
git add cmake/Deps.cmake core/CMakeLists.txt core/rhi/backends/vulkan/ tests/CMakeLists.txt tests/golden/cube_vulkan.png
git commit -m "feat(rhi/vulkan): Vulkan 后端（MoltenVK 离屏渲染）+ 跨后端 golden 一致性"
```

---

### Task 12: GLES 后端桩（实现留 P0-2）

**Files:**
- Create: `core/rhi/backends/gles/gles_device.h`
- Create: `core/rhi/backends/gles/gles_device.cpp`
- Modify: `core/CMakeLists.txt`

**背景**：GLES 后端只能在 Android 上有效验证（macOS 无 GLES 驱动），P0-1 只落文件位置与接口契约，避免写无法测试的实现代码。host 上工厂走 `rhi_factory.cpp` 的内置 fallback 返回 nullptr。

- [ ] **Step 1: 写桩文件**

`core/rhi/backends/gles/gles_device.h`:
```cpp
#pragma once
#include "rhi/rhi_device.h"

namespace rd {
std::unique_ptr<Device> createGLESDevice(const DeviceDesc& desc);
}
```

`core/rhi/backends/gles/gles_device.cpp`:
```cpp
#include "gles_device.h"

#if defined(__ANDROID__)
#include "foundation/log.h"

// P0-2 实现要点备忘：
// - EGL 上下文 + GLES3；离屏用 pbuffer surface，上屏用 ANativeWindow
// - spirv-cross 输出的 GLSL ES 3.0 uniform block 不带 binding 布局：
//   需 glGetUniformBlockIndex + glUniformBlockBinding(program, index, slot) 手动绑定
// - NDC z ∈ [-1,1]（与 0..1 约定只影响深度精度，P1 引入深度时注意）
namespace rd {
std::unique_ptr<Device> createGLESDevice(const DeviceDesc&) {
  RD_LOGW("rhi.gles", "GLES 后端将在 P0-2 实现");
  return nullptr;
}
} // namespace rd
#endif
```

`core/CMakeLists.txt` 末尾追加：
```cmake
if(ANDROID)
  target_sources(rd_core PRIVATE rhi/backends/gles/gles_device.cpp)
endif()
```

- [ ] **Step 2: 验证 host 构建与测试不受影响**

```bash
cmake --build build -j8 && ctest --test-dir build --output-on-failure
```
预期：全绿（`Factory.UnavailableBackendReturnsNull` 中 GLES 分支仍返回 nullptr，PASS）。

- [ ] **Step 3: Commit**

```bash
git add core/rhi/backends/gles/ core/CMakeLists.txt
git commit -m "chore(rhi/gles): 后端桩与 P0-2 实现备忘"
```

---

### Task 13: render_test CLI + CI + 本地检查脚本

**Files:**
- Create: `tools/render_test/main.cpp`
- Modify: `tools/render_test/CMakeLists.txt`
- Create: `.github/workflows/ci.yml`
- Create: `scripts/check.sh`

- [ ] **Step 1: 写 render_test CLI**

`tools/render_test/main.cpp`:
```cpp
// 手动验证工具：离屏渲染 cube 并保存 PNG。
// 用法: render_test --backend metal|vulkan [--angle 45] [--out cube.png]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "common/cube_renderer.h"
#include "common/image.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

int main(int argc, char** argv) {
  rd::Backend backend = rd::Backend::Metal;
  float angleDeg = 45.0f;
  std::string out = "cube.png";
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
      backend = !strcmp(argv[++i], "vulkan") ? rd::Backend::Vulkan : rd::Backend::Metal;
    } else if (!strcmp(argv[i], "--angle") && i + 1 < argc) {
      angleDeg = float(atof(argv[++i]));
    } else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
      out = argv[++i];
    }
  }

  rd::DeviceDesc desc;
  desc.backend = backend;
  auto device = rd::createDevice(desc);
  if (!device) {
    fprintf(stderr, "后端不可用\n");
    return 1;
  }
  constexpr uint32_t kW = 512, kH = 512;
  auto target = device->createOffscreenTarget({kW, kH});
  rd::test::CubeRenderer cube;
  if (!target.valid() || !cube.init(*device, RD_SHADER_DIR)) {
    fprintf(stderr, "初始化失败\n");
    return 1;
  }
  cube.render(*device, target, kW, kH, angleDeg * 0.0174532925f);
  std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4);
  if (!device->readbackTarget(target, pixels.data(), pixels.size())) {
    fprintf(stderr, "readback 失败\n");
    return 1;
  }
  cube.shutdown(*device);
  device->destroyTarget(target);
  if (!rd::test::savePNG(out, kW, kH, pixels.data())) {
    fprintf(stderr, "保存失败\n");
    return 1;
  }
  printf("已保存 %s (%ux%u)\n", out.c_str(), kW, kH);
  return 0;
}
```

`tools/render_test/CMakeLists.txt`:
```cmake
add_executable(render_test
  main.cpp
  ${CMAKE_SOURCE_DIR}/tests/common/cube_renderer.cpp
  ${CMAKE_SOURCE_DIR}/tests/common/image.cpp
)
target_link_libraries(render_test PRIVATE rd_core)
target_include_directories(render_test PRIVATE
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_BINARY_DIR}/generated
  ${stb_SOURCE_DIR})
set_target_properties(render_test PROPERTIES
  BUILD_RPATH "/opt/homebrew/lib;/usr/local/lib;$ENV{VULKAN_SDK}/lib")
```

- [ ] **Step 2: 构建并双后端验证**

```bash
cmake --build build -j8
./build/tools/render_test/render_test --backend metal --out /tmp/cube_metal.png
./build/tools/render_test/render_test --backend vulkan --out /tmp/cube_vulkan.png
```
预期：两条命令都输出 `已保存 ... (512x512)`。用 Read 工具目视两张图均为顶点色立方体。

- [ ] **Step 3: 写 CI 与本地检查脚本**

`.github/workflows/ci.yml`:
```yaml
name: ci
on: [push, pull_request]
jobs:
  host-tests:
    runs-on: macos-14
    steps:
      - uses: actions/checkout@v4
      - name: Install MoltenVK
        run: brew install moltenvk
      - name: Configure
        run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
      - name: Build
        run: cmake --build build -j8
      - name: Test
        run: ctest --test-dir build --output-on-failure
```

`scripts/check.sh`:
```bash
#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build
cmake --build build -j8
ctest --test-dir build --output-on-failure
echo "hint: RD_UPDATE_GOLDENS=1 ctest ... 可重新生成 golden image"
```

- [ ] **Step 4: 跑 check.sh 全量验证**

```bash
chmod +x scripts/check.sh
./scripts/check.sh
```
预期：构建成功，测试全绿。

- [ ] **Step 5: Commit**

```bash
git add tools/ .github/ scripts/
git commit -m "build: render_test 手动验证 CLI + GitHub Actions CI + check.sh"
```

---

### Task 14: 文档与 P0-1 验收

**Files:**
- Create: `AGENTS.md`
- Create: `README.md`

- [ ] **Step 1: 写 AGENTS.md**

```markdown
# AGENTS.md

## 项目概述
移动端 3D 渲染器框架：C++17 跨平台内核 + RHI 三后端（Vulkan/Metal/GLES），
平台绑定（iOS/Android，二期鸿蒙）。设计文档：
docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md

## 当前状态
- P0-1 完成：构建基建 + foundation + RHI 抽象 + Metal/Vulkan 后端（主机离屏验证）
- P0-2 待做：GLES 后端实现 + Android/iOS RenderView 容器 + 真机验收
- P1 待做：glTF 加载 + PBR/IBL

## 构建与测试
```bash
brew install moltenvk cmake   # 一次性
./scripts/check.sh            # 配置 + 构建 + 全部测试
```
- 更新 golden image：`RD_UPDATE_GOLDENS=1 ctest --test-dir build -R Cube`，
  然后目视核对 tests/golden/*.png 再提交
- 手动渲染：`./build/tools/render_test/render_test --backend metal|vulkan --out cube.png`

## 代码约定
- C++17；命名空间 `rd`；测试工具在 `rd::test`
- 数学：glm，全局 `GLM_FORCE_DEPTH_ZERO_TO_ONE`（NDC z ∈ [0,1]，右手系）
- 句柄：`Handle<Tag>`，0 无效；GPU 资源只经 `rhi::Device` 创建/销毁
- 绑定约定：uniform slot N ↔ Metal buffer(N) ↔ Vulkan set0 binding N ↔ GLES binding N；
  vertex binding N ↔ Metal buffer(N+1)
- 日志：`RD_LOGD/I/W/E(tag, fmt, ...)`，tag 用模块名（如 `rhi.vk`）
- 错误处理：内核不用异常；工厂/创建函数失败返回空句柄/nullptr + 日志
- 线程（P0-2 起强制）：公开 API 主线程调用；GPU 调用只在渲染线程

## 提交规范
- 小步提交，每任务一个 commit；格式 `<type>(<scope>): 描述`
  （feat/fix/build/test/chore/docs）
```

- [ ] **Step 2: 写 README.md**

```markdown
# 3DRender

移动端 3D 渲染框架（iOS / Android，规划鸿蒙）。C++17 跨平台内核，
RHI 后端：Metal / Vulkan / OpenGL ES 3.0。

## 快速开始
```bash
brew install moltenvk cmake
./scripts/check.sh          # 构建 + 测试
./build/tools/render_test/render_test --backend metal --out cube.png
```

## 文档
- 设计：docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md
- 实施计划：docs/superpowers/plans/
```

- [ ] **Step 3: P0-1 验收核对（逐项确认）**

```bash
./scripts/check.sh
git log --oneline
```
逐项确认：
- [ ] check.sh 全绿（含 `Cube.CrossBackendConsistent` PASS）
- [ ] `ls build/shaders_out/` 有 spv / metallib / gles / json 产物
- [ ] render_test 双后端各出一张正确 PNG
- [ ] git log 每任务一个 commit，共 14 个

- [ ] **Step 4: Commit**

```bash
git add AGENTS.md README.md
git commit -m "docs: AGENTS.md + README"
```

---

## P0-1 验收标准（对照 spec P0 中属于本计划的部分）

| spec 条目 | 本计划落点 | 验收方式 |
|---|---|---|
| CMake 构建 + CI | Task 1/8/13 | `scripts/check.sh` 全绿；ci.yml 就绪 |
| foundation | Task 2-5 | 单测全过 |
| RHI 抽象 | Task 6-7 | 单测全过；接口被两后端实现 |
| Vulkan/Metal 后端 | Task 10/11 | golden image 各自通过 + 跨后端一致 |
| GLES 后端 | Task 12（桩） | Android 实现归 P0-2 |
| shader 离线管线 | Task 8 | 三语言产物 + 反射 JSON 生成 |
| 双端 RenderView 容器 | — | **P0-2 范围** |

## 自查记录

- 类型一致性：`createDevice`/`createMetalDevice`/`createVulkanDevice`/`createGLESDevice` 签名与工厂一致；`CommandBuffer` 方法名跨任务一致；`CubeRenderer::init/render/shutdown` 在 Task 10/13 用法一致；`formatSize`、`hasFlag`、`BufferUsage::operator|` 在 Task 6 定义、后续任务直接使用。
- 已知简化（有意为之，P1/P0-2 处理）：离屏目标无深度附件（cube 靠凸体+背面剔除）；Vulkan buffer 全部 HOST_VISIBLE；uniform slot 上限 4；Metal shader 入口固定 `main0`。
- 风险：CI macos-14 runner 的 Metal 可用性（GitHub ARM runner 一般可用；若 `MetalGoldenMatches` 在 CI 失败而本地通过，给该测试加 CI 环境跳过并记录 issue 跟进）。



