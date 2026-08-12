# 全量代码注释实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为仓库全部代码加完善中文注释（公开头文件 Doxygen 格式，实现文件普通 `//`），只增注释、不改任何代码逻辑。

**Architecture:** 按模块顺序逐文件加注释（foundation → rhi 接口 → 三后端 → api/scene → platform → tools/tests → shaders/构建脚本），每任务后增量编译验证，最后 `./scripts/check.sh` 全量验证，所有改动归入**单个 commit**（方案 C）。

**Tech Stack:** C++17 / Objective-C++ / Swift / Kotlin / GLSL / CMake / Gradle。

**设计文档:** `docs/superpowers/specs/2026-08-11-code-comments-design.md`

## 全局规则（每个任务都必须遵守）

1. **只加注释**：不修改、不重排、不格式化任何代码行；diff 中除注释外不得有代码变动。
2. **注释语言为中文**，术语与 AGENTS.md 一致（Handle、RHI、绑定约定、`NDC z∈[0,1]` 右手系、shader 入口名约定等）。
3. **公开头文件用 Doxygen**：`///` 或 `/** */`，含 `@brief`/`@param`/`@return`/`@note`，重点写所有权、线程约束、失败语义（返回空句柄/nullptr）、绑定约定。
4. **实现文件用普通 `//`**：文件顶部写模块职责说明（`// ===== ... =====` 或简洁块），关键算法/后端技巧/平台限制逐段注释。
5. **准确性优先**：注释必须与代码实际行为一致，宁可少写不可写错；不臆测未读代码的行为。
6. **排除** `core/api/embedded_shaders.cpp`（自动生成，勿手改）。
7. **特殊处理**：`tests/rhi/texture_test.cpp`、`tests/rhi/texture_quad_test.cpp` 含有用户未提交的代码改动，注释时**不得改动那些新增代码行本身**，仅在合适位置补注释；这些改动最终会随本工作的单 commit 一起提交（用户已明确指示）。
8. 每任务完成后运行 `cmake --build build -j8`（host 构建，验证注释未破坏编译）；Kotlin/Swift 文件不参与 host 构建，注释后仅需目检语法。

## 注释密度基准

- 每个文件：顶部文件级注释（模块/文件职责、关键约定）。
- 每个公开类型/函数：Doxygen 注释（头文件）或简要说明（实现文件中的静态/私有函数）。
- 非平凡代码块（>5 行的算法、平台 workaround、资源生命周期管理）：逐段解释「为什么」，不只是「做什么」。
- 简单自明的代码（getter、明显的一次性赋值）：不强行加注，避免噪音。

---

### Task 1: foundation 模块（9 文件）

**Files:**
- Modify: `core/foundation/handle.h`
- Modify: `core/foundation/log.h`
- Modify: `core/foundation/log.cpp`
- Modify: `core/foundation/math.h`
- Modify: `core/foundation/task_queue.h`
- Modify: `core/foundation/task_queue.cpp`
- Modify: `core/foundation/version.h`
- Modify: `core/foundation/version.cpp`

**各文件注释要点：**
- `handle.h`：Doxygen 说明类型安全句柄设计意图（Tag 使不同资源句柄不可互换）、0 为无效值的约定、`explicit` 构造原因、`std::hash` 特化的用途（可作 unordered 容器 key）。
- `log.h`：Doxygen 说明 `RD_LOGD/I/W/E` 宏的用法与 tag 约定（tag 用模块名如 `rhi.vk`）、各平台落地（Android logcat / iOS os_log / host stderr——以实际代码为准）。
- `log.cpp`：逐段注释平台分支实现。
- `math.h`：说明 glm 全局约定 `GLM_FORCE_DEPTH_ZERO_TO_ONE`（NDC z∈[0,1]，右手系）及该头提供的别名/工具（以实际内容为准）。
- `task_queue.h/.cpp`：Doxygen 说明单线程任务队列的用途与线程约束、各方法语义；实现中注释任务入队/执行流程。
- `version.h/.cpp`：说明版本号来源（编译期定义）与 API 语义。

- [ ] **Step 1: 逐文件阅读并按要点加注释**（规则 1-5 全程适用）
- [ ] **Step 2: 增量编译验证**

Run: `cmake --build build -j8`
Expected: 构建成功，无 error（注释不引入新告警）

---

### Task 2: rhi 接口层（3 文件）

**Files:**
- Modify: `core/rhi/rhi_types.h`
- Modify: `core/rhi/rhi_device.h`
- Modify: `core/rhi/rhi_factory.cpp`

**各文件注释要点：**
- `rhi_types.h`：逐枚举个 Doxygen（Backend/Format/ShaderStage/PrimitiveTopology/TextureType/IndexType 等，以实际内容为准），说明各枚举值含义与后端差异；各 Desc 结构体逐字段注释（单位、默认值、约束如 cube 必须方形/mip 上限）。
- `rhi_device.h`：现有注释已部分存在，升级为完整 Doxygen——CommandBuffer 录制模型（每帧 acquire、beginRenderPass/endRenderPass 配对）、Device 各 create/destroy 对的所有权与失败语义、swapchain 流程（create→acquire→present→destroy）、`swapChainColorFormat` 的用途（pipeline 须与之匹配，Metal layer 限 BGRA8 系）、线程约束（同一 Device 所有调用在同一线程）。
- `rhi_factory.cpp`：注释后端分发逻辑、编译期宏开关（RD_WITH_VULKAN 等）与运行期不可用时返回 nullptr 的语义。

- [ ] **Step 1: 逐文件阅读并加注释**
- [ ] **Step 2: 增量编译验证**

Run: `cmake --build build -j8`
Expected: 构建成功

---

### Task 3: Metal 后端（2 文件）

**Files:**
- Modify: `core/rhi/backends/metal/metal_device.h`
- Modify: `core/rhi/backends/metal/metal_device.mm`

**注释要点：**
- 文件顶部：模块职责（Metal 后端实现）+ 关键平台事实（Metal layer 颜色格式限 BGRA8 系、vertex binding N ↔ buffer(N+1) 约定、shader 入口 `main0` 的 spirv-cross 约定、资源堆/句柄表管理策略——以实际代码为准）。
- 逐方法注释：句柄分配/回收、createBuffer/Texture/Sampler 的参数校验（cube 方形、mip 上限前置校验）、staging/upload 路径、metallib 加载、pipeline 创建时颜色格式与 swapchain 对齐、readback 实现（blit 到 shared 内存）、swapchain（CAMetalLayer/nextDrawable/present）。
- Objective-C++ 与 C++ 边界处的所有权（__bridge 等，以实际代码为准）。

- [ ] **Step 1: 逐文件阅读并加注释**
- [ ] **Step 2: 增量编译验证**

Run: `cmake --build build -j8`
Expected: 构建成功（host 上 Metal 后端若被编译则验证；否则目检）

---

### Task 4: GLES 后端（2 文件）

**Files:**
- Modify: `core/rhi/backends/gles/gles_device.h`
- Modify: `core/rhi/backends/gles/gles_device.cpp`

**注释要点：**
- 文件顶部：GLES 后端职责、GL 上下文线程约束、binding 约定（uniform slot N ↔ GL binding N）。
- 逐方法注释：GL 对象创建/删除与句柄映射、VAO/VBO/UBO/纹理/采样器状态、着色器编译与入口 `main`、离屏 FBO 与 readback（glReadPixels）、swapchain 在 GLES 下的语义（默认帧缓冲，以实际代码为准）。

- [ ] **Step 1: 逐文件阅读并加注释**
- [ ] **Step 2: 增量编译验证**

Run: `cmake --build build -j8`
Expected: 构建成功（host 上 GLES 后端若被编译则验证；否则目检）

---

### Task 5: Vulkan 后端（2 文件）

**Files:**
- Modify: `core/rhi/backends/vulkan/vulkan_device.h`
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`（约 1225 行，全仓最大文件）

**注释要点：**
- 文件顶部：Vulkan 后端职责 + 关键架构事实（直连 MoltenVK ICD 不经过 Loader、不请求 `VK_KHR_portability_enumeration`、set0 binding N ↔ uniform slot N、staging buffer 上传纹理、combined sampler）。
- 逐段注释：实例/设备创建、内存分配策略、命令缓冲池、描述符集布局与分配、render pass/pipeline 创建、纹理上传（staging→copy→layout transition）、swapchain（acquire/present、表面丢失处理）、readback 路径、同步原语。
- 对 Vulkan 样板代码（冗长的 CreateInfo 填充）按块注释用途即可，不逐行解释字段。

- [ ] **Step 1: 阅读并加注释（文件大，分两段做：前半实例/资源，后半渲染/swapchain）**
- [ ] **Step 2: 增量编译验证**

Run: `cmake --build build -j8`
Expected: 构建成功

---

### Task 6: api + scene（5 文件）

**Files:**
- Modify: `core/api/rd_api.h`
- Modify: `core/api/rd_api.cpp`
- Modify: `core/api/embedded_shaders.h`（`.cpp` 自动生成，**不动**）
- Modify: `core/scene/cube_scene.h`
- Modify: `core/scene/cube_scene.cpp`

**注释要点：**
- `rd_api.h`：C API 逐函数 Doxygen（`@note` 标明线程约束：同一 rd_engine 所有调用在同一线程——Android=RenderView 渲染线程，iOS=主线程 MTKView 惯例）、句柄生命周期、nativeWindow 各平台类型。
- `rd_api.cpp`：注释 C→C++ 边界、engine 内部状态管理、逐函数实现要点。
- `embedded_shaders.h`：说明该接口由自动生成实现支撑（host=build 期，Android/iOS=configure 期生成 .cpp），入口名约定。
- `cube_scene.h/.cpp`：说明示例场景职责（旋转立方体）、MVP 计算与坐标系约定、逐段渲染流程。

- [ ] **Step 1: 逐文件阅读并加注释**
- [ ] **Step 2: 增量编译验证**

Run: `cmake --build build -j8`
Expected: 构建成功

---

### Task 7: platform 绑定（7 文件）

**Files:**
- Modify: `platform/android/jni/rd_jni.cpp`
- Modify: `platform/android/src/main/java/com/rd/renderer/RenderView.kt`
- Modify: `platform/ios/RenderView.swift`
- Modify: `samples/android/app/src/main/java/com/rd/sample/MainActivity.kt`
- Modify: `samples/ios/RdDemo/AppDelegate.swift`
- Modify: `samples/ios/RdDemo/dummy.cpp`
- Modify: `samples/ios/RdDemo/RdDemo-Bridging-Header.h`

**注释要点：**
- `rd_jni.cpp`：JNI 桥接职责、native 方法注册/句柄传递（long 存指针等，以实际代码为准）、线程模型（渲染线程调 rd_engine）。
- `RenderView.kt`：类职责（GLSurfaceView/SurfaceView 容器，以实际代码为准）、渲染线程生命周期、backend 选择（intent extra）。
- `RenderView.swift`：CAMetalLayer+CADisplayLink 驱动模型、MTKView 主线程惯例、与 rd_engine 的交互。
- `MainActivity.kt`/`AppDelegate.swift`：示例 app 职责、如何嵌入 RenderView。
- `dummy.cpp`/`Bridging-Header.h`：存在性说明（为何需要 dummy 编译单元/桥接头）。

- [ ] **Step 1: 逐文件阅读并加注释**（Kotlin/Swift 不参与 host 构建，目检语法）
- [ ] **Step 2: C++ 侧增量编译验证**

Run: `cmake --build build -j8`
Expected: 构建成功

---

### Task 8: tools（2 文件）

**Files:**
- Modify: `tools/render_test/main.cpp`
- Modify: `tools/img_check/main.cpp`

**注释要点：**
- `render_test/main.cpp`：工具用途（手动渲染：`--backend metal|vulkan --out cube.png`）、参数解析、渲染流程逐段注释。
- `img_check/main.cpp`：工具用途（截图校验：`--min-coverage`）、覆盖度算法逐段注释、退出码语义。

- [ ] **Step 1: 逐文件阅读并加注释**
- [ ] **Step 2: 增量编译验证**

Run: `cmake --build build -j8`
Expected: 构建成功

---

### Task 9: tests（16 文件）

**Files:**
- Modify: `tests/api/api_test.cpp`
- Modify: `tests/rhi/factory_test.cpp`
- Modify: `tests/rhi/texture_quad_test.cpp`（**含用户未提交改动**，只补注释不动代码）
- Modify: `tests/rhi/texture_test.cpp`（**含用户未提交改动**，只补注释不动代码）
- Modify: `tests/rhi/rhi_types_test.cpp`
- Modify: `tests/rhi/swapchain_test.cpp`
- Modify: `tests/rhi/cube_test.cpp`
- Modify: `tests/foundation/math_test.cpp`
- Modify: `tests/foundation/log_test.cpp`
- Modify: `tests/foundation/smoke_test.cpp`
- Modify: `tests/foundation/handle_test.cpp`
- Modify: `tests/foundation/task_queue_test.cpp`
- Modify: `tests/common/image.h`
- Modify: `tests/common/image.cpp`
- Modify: `tests/common/image_test.cpp`
- Modify: `tests/common/shader_code.h`
- Modify: `tests/common/shader_code.cpp`

**注释要点：**
- 每个测试文件顶部：该套件覆盖什么、后端条件编译（`#if defined(RD_WITH_VULKAN)` 等）的含义。
- 非平凡测试（golden image 对比、readback 校验、swapchain 流程）：逐 TEST 注释测试意图与判定标准；说明 `RD_UPDATE_GOLDENS=1` 更新 golden 的流程（cube/texquad 测试处）。
- `tests/common/`：image 读写/比较工具与 shader_code 加载工具（按后端选择 SPIR-V/metallib/GLSL 及入口名）的 Doxygen/说明注释。

- [ ] **Step 1: 逐文件阅读并加注释**
- [ ] **Step 2: 增量编译验证**

Run: `cmake --build build -j8`
Expected: 构建成功

---

### Task 10: shaders（5 文件）

**Files:**
- Modify: `shaders/cube.vert`
- Modify: `shaders/cube.frag`
- Modify: `shaders/texquad.vert`
- Modify: `shaders/texquad.frag`
- Modify: `shaders/CMakeLists.txt`

**注释要点：**
- 每个 GLSL 文件顶部：用途、输入/输出、uniform/texture binding 号与 RHI 侧约定的对应关系、坐标系（NDC z∈[0,1]，右手系，投影经 GLM_FORCE_DEPTH_ZERO_TO_ONE）。
- `shaders/CMakeLists.txt`：离线编译流程（glslang→SPIR-V，spirv-cross→MSL/GLSL ES，以实际脚本为准）、输出组织与 `RD_SHADER_DIR` 的关系。

- [ ] **Step 1: 逐文件阅读并加注释**
- [ ] **Step 2: 编译验证**（shader 重编译在 host 构建内）

Run: `cmake --build build -j8`
Expected: 构建成功（含 shader 编译步骤）

---

### Task 11: 构建脚本（11 文件）

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `cmake/Deps.cmake`
- Modify: `cmake/ShaderCompile.cmake`
- Modify: `cmake/GenEmbedded.cmake`
- Modify: `cmake/EmbedShaders.cmake`
- Modify: `cmake/ios.toolchain.cmake`
- Modify: `scripts/check.sh`
- Modify: `samples/android/build.gradle`
- Modify: `samples/android/settings.gradle`
- Modify: `samples/android/gradle.properties`
- Modify: `samples/android/app/build.gradle`

**注释要点：**
- 根 `CMakeLists.txt`：目标结构（core/platform/tools/tests）、选项与后端宏开关、各子目录职责。
- `Deps.cmake`：第三方依赖（glm、glslang/spirv-cross、MoltenVK 查找，以实际内容为准）来源与链接策略。
- `ShaderCompile.cmake`/`GenEmbedded.cmake`/`EmbedShaders.cmake`：shader 离线管线与 embedded_shaders.cpp 生成时机（host=build 期；Android/iOS=configure 期）、iOS 真机/模拟器 metallib 分离（RD_EMBED_IOS_METAL / RD_EMBED_IOS_SIMULATOR）。
- `ios.toolchain.cmake`：toolchain 变量语义（RD_IOS_SDK=iphonesimulator 等）。
- `check.sh`：脚本三步（配置+构建+测试）说明。
- Gradle 各文件：项目/模块配置块职责；`app/build.gradle` 重点注释 externalNativeBuild、`-Wl,-z,max-page-size=16384`（16KB 页对齐）的缘由、backend intent extra 约定（以实际内容为准）。

- [ ] **Step 1: 逐文件阅读并加注释**
- [ ] **Step 2: 重新配置+构建验证**（CMake 脚本改动须验证 configure 不破坏）

Run: `./scripts/check.sh`（此处先跑一次，作为 Task 12 前的预检）
Expected: 配置+构建+全部测试通过

---

### Task 12: 全量验证 + 单 commit

- [ ] **Step 1: 全量验证**

Run: `./scripts/check.sh`
Expected: 配置 + 构建 + 全部测试全绿（含 golden image 测试——注释不得改变任何渲染结果）

- [ ] **Step 2: diff 安全审查**

Run: `git diff --stat` 与 `git diff -U0 | grep -E '^[+-]' | grep -vE '^[+-]{3}' | grep -vE '^\+\s*(//|/\*|\*|#|--)' | grep -vE '^[+-]\s*$'`
Expected: 输出中除两类外无其他行：(a) 新增的注释行（`+` 开头，内容为 `//`/`/*`/`*`/`#`/`--`/XML 注释）；(b) `tests/rhi/texture_test.cpp` 与 `tests/rhi/texture_quad_test.cpp` 中用户原有的未提交代码改动。发现任何其他代码变动即回滚该行。
注意：Gradle/Kotlin/Swift 注释也是 `//`；CMake 注释是 `#`；如审查命令误报，逐行目检确认。

- [ ] **Step 3: 提交单 commit**（不含 `3DRender.code-workspace`—— untracked IDE 配置，保持未跟踪）

```bash
git add -u
git commit -m "docs(comments): 全量代码注释（Doxygen 公开 API + 实现逐段注释）+ rhi 纹理测试补充"
```

（commit message 注明包含用户原有的 rhi 纹理测试改动；`git add -u` 只暂存已跟踪文件的改动，自动排除 untracked 的 `3DRender.code-workspace`。）

- [ ] **Step 4: 提交后确认**

Run: `git status --short && git log --oneline -2`
Expected: 工作区仅剩 `3DRender.code-workspace` untracked；最新 commit 为本次注释 commit

---

## Self-Review 记录

- **Spec coverage**：spec 范围（core/platform/tools/tests/shaders/构建脚本）逐项对应 Task 1-11；排除项（embedded_shaders.cpp）在全局规则 6；单 commit 在 Task 12；check.sh 全绿在 Task 11/12。✓
- **Placeholder scan**：各任务给出具体注释要点而非「适当加注释」；注释本身在执行时撰写（计划内嵌全部注释文本会使计划与工作量重复，此处以「逐文件要点清单 + 密度基准」作为可检查标准）。✓
- **Type consistency**：无代码接口变更，无类型一致性问题。✓
- **风险**：两个含用户未提交改动的测试文件已在全局规则 7 与 Task 9/12 中明确处理方式（用户明确指示随单 commit 一起提交）。✓
