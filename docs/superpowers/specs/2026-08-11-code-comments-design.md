# 全量代码注释设计

日期：2026-08-11
状态：已批准（用户选定方案 C）

## 目标

为仓库**全部代码**加上完善的中文注释。只增注释，**不改任何代码逻辑与行为**
（除注释外不动任何代码行，不重排格式）。

## 范围

- core/：foundation、rhi（接口层 + Vulkan/Metal/GLES 三后端实现）、api、scene
- platform/：Android JNI（C++）与 Kotlin、iOS Swift
- tools/：render_test、img_check
- tests/：foundation、rhi、api、common
- shaders/：GLSL 源码
- 构建脚本：CMakeLists.txt（含 cmake/ 工具链与模块）、Gradle 脚本

**排除**：`core/api/embedded_shaders.cpp`（自动生成，AGENTS.md 明确勿手改；
其头文件 `embedded_shaders.h` 可注释）。`build/`、`build-ios/` 产物目录不涉及。

## 注释规范

| 文件类型 | 风格 |
|---|---|
| 公开头文件（foundation/rhi/api/scene 的 `.h`） | Doxygen 格式（`///` 或 `/** */`，含 `@brief` `@param` `@return` `@note`）。重点写清：所有权归属、线程约束、绑定约定、句柄生命周期、失败语义（返回空句柄/nullptr） |
| 实现文件（`.cpp`/`.mm`/`.swift`/`.kt`） | 文件顶部模块职责说明 + 普通 `//` 逐段注释：关键算法、后端技巧（MoltenVK ICD 直连、BGRA8 swapchain 对齐、staging 上传、combined sampler 等）、平台限制 |
| shaders（GLSL） | 顶部用途说明 + binding 约定、NDC/坐标系约定（`GLM_FORCE_DEPTH_ZERO_TO_ONE`，z∈[0,1]，右手系）逐处解释 |
| CMake/Gradle | 段落级说明各配置块用途（16KB 页对齐、metallib 真机/模拟器分离编译、embedded_shaders 生成时机、MoltenVK 链接等） |

术语与 AGENTS.md 保持一致：Handle、RHI、绑定约定（uniform slot N ↔ Metal
buffer(N) ↔ Vulkan set0 binding N ↔ GLES binding N；vertex binding N ↔ Metal
buffer(N+1)）、shader 入口名约定等。

## 执行方案（方案 C：一次性全量，单 commit）

工作顺序（便于保持上下文连贯，但所有改动归入**单个 commit**）：

1. foundation（handle/log/math/task_queue/version）
2. rhi 接口层（rhi_types.h / rhi_device.h / rhi_factory.cpp）
3. 三后端实现（metal → gles → vulkan）
4. api + scene（rd_api、embedded_shaders.h、cube_scene）
5. platform（Android JNI/Kotlin、iOS Swift）
6. tools + tests
7. shaders + 构建脚本
8. `./scripts/check.sh` 全量验证（配置 + 构建 + 全部测试必须全绿）
9. 单 commit：`docs(comments): 全量代码注释（Doxygen 公开 API + 实现逐段注释）`

## 验收标准

- `check.sh` 全绿（注释不得破坏编译与任何测试）
- 逐文件目检：注释与代码行为一致——注释必须准确，宁可少写不可写错
- diff 中除注释外无代码行变动

## 风险与约束

- 改动文件数多（约 40+），单 commit diff 大；以「除注释外零代码变动」
  和 check.sh 全绿作为安全网
- Doxygen 注释不得引入编译告警（`-Wdocumentation` 未启用，无风险；
  但须保持注释语法合法）
