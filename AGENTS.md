# AGENTS.md

## 项目概述
移动端 3D 渲染器框架：C++17 跨平台内核 + RHI 三后端（Vulkan/Metal/GLES），
平台绑定（iOS/Android，二期鸿蒙）。设计文档：
docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md

## 当前状态
- P0 完成：构建基建 + foundation + RHI 三后端（Vulkan/Metal/GLES）+ shader 离线管线
  + C API（rd_engine）+ Android/iOS RenderView 容器 + 双端模拟器截图验证
- P1 待做：glTF 加载 + PBR/IBL（resource/renderer/scene 层）

## 构建与测试
```bash
brew install molten-vk cmake   # 一次性（注意公式名是 molten-vk）
./scripts/check.sh            # 配置 + 构建 + 全部测试
```
- 更新 golden image：`RD_UPDATE_GOLDENS=1 ctest --test-dir build -R Cube`，
  然后目视核对 tests/golden/*.png 再提交
- 手动渲染：`./build/tools/render_test/render_test --backend metal|vulkan --out cube.png`

## 移动端构建
- 环境：`source /tmp/rd_env.sh`（JAVA_HOME/ANDROID_HOME/PATH）；JDK 须 17~22（openjdk@21）
- Android：`cd samples/android && ./gradlew :app:assembleDebug`
  （native 库须 16KB 页对齐：rd_jni 已配 `-Wl,-z,max-page-size=16384`）
- iOS 模拟器：`cmake -S . -B build-ios -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake -DRD_IOS_SDK=iphonesimulator && cmake --build build-ios --config Debug`
- 截图校验：`./build/tools/img_check/img_check <png> --min-coverage 0.03`

## 代码约定
- C++17；命名空间 `rd`；测试工具在 `rd::test`
- 数学：glm，全局 `GLM_FORCE_DEPTH_ZERO_TO_ONE`（NDC z ∈ [0,1]，右手系）
- 句柄：`Handle<Tag>`，0 无效；GPU 资源只经 `rhi::Device` 创建/销毁
- 绑定约定：uniform slot N ↔ Metal buffer(N) ↔ Vulkan set0 binding N ↔ GLES binding N；
  vertex binding N ↔ Metal buffer(N+1)
- shader 入口名：Metal(metallib) 为 `main0`（spirv-cross 约定）；SPIR-V/GLSL ES 为 `main`
- Vulkan 直连 MoltenVK ICD（不经过 Loader），不要请求 VK_KHR_portability_enumeration
- 日志：`RD_LOGD/I/W/E(tag, fmt, ...)`，tag 用模块名（如 `rhi.vk`）
- 错误处理：内核不用异常；工厂/创建函数失败返回空句柄/nullptr + 日志
- 线程：同一 rd_engine 的所有调用在同一线程（Android=RenderView 渲染线程，iOS=主线程 MTKView 惯例）
- shader 内嵌：embedded_shaders.cpp 自动生成（host=build 期；Android/iOS=configure 期），勿手改；
  iOS 真机/模拟器 metallib 分别编译（RD_EMBED_IOS_METAL / RD_EMBED_IOS_SIMULATOR）
- Metal swapchain 颜色格式为 BGRA8（layer 限制）；pipeline 格式须经 swapChainColorFormat 对齐

## 提交规范
- 小步提交，每任务一个 commit；格式 `<type>(<scope>): 描述`
  （feat/fix/build/test/chore/docs）
