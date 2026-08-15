# AGENTS.md

## 项目概述
移动端 3D 渲染器框架：C++17 跨平台内核 + RHI 三后端（Vulkan/Metal/GLES），
平台绑定（iOS/Android，二期鸿蒙）。设计文档：
docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md

## 当前状态
- P0 完成：构建基建 + foundation + RHI 三后端（Vulkan/Metal/GLES）+ shader 离线管线
  + C API（rd_engine）+ Android/iOS RenderView 容器 + 双端模拟器截图验证
- 阶段一完成：RHI 底座强化（能力表/内存 flag/N 帧退休/管线缓存/instancing/
  cube 渲染目标/GLES 纹理+延迟回放）
- 阶段二 a 完成：renderer/scene/resource 三层骨架 + 离屏深度附件 + glTF unlit 渲染
- 阶段二 b 完成：PBR/IBL(glTF MR 全模型 + emissive/occlusion/KHR_texture_transform;
  混合路径 IBL:GPU specular 预滤波 + CPU SH9/BRDF LUT;1 方向光;
  DamagedHelmet golden 双后端像素级一致)
- 下一步：阶段二 c(Orbit 手势 + 画质分级 + KTX2)/ P2(阴影+多光源+后处理+骨骼动画)

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
  vertex binding N ↔ Metal buffer(N+4)（uniform 0..3 占 Metal buffer 0..3，顶点从 4 起）
- shader 入口名：Metal(metallib) 为 `main0`（spirv-cross 约定）；SPIR-V/GLSL ES 为 `main`
- Vulkan 直连 MoltenVK ICD（不经过 Loader），不要请求 VK_KHR_portability_enumeration
- 日志：`RD_LOGD/I/W/E(tag, fmt, ...)`，tag 用模块名（如 `rhi.vk`）
- 错误处理：内核不用异常；工厂/创建函数失败返回空句柄/nullptr + 日志
- 线程：同一 rd_engine 的所有调用在同一线程（Android=RenderView 渲染线程，iOS=主线程 MTKView 惯例）
- 帧括号：渲染循环每帧 `beginFrame()`/`endFrame()`（present 之后）；destroy 的底层资源
  延迟到帧完成后回收（退休队列），句柄 destroy 后立即失效；每帧至多一次 submit
- 能力查询：只经 `device.caps()`（rhi_capability.h），不直接查后端扩展
- BufferDesc 五元组 `{size, usage, hostWrite, hostRead, data}`：非 hostWrite 缓冲为
  device-local，`updateBuffer` 会被拒绝（动态数据须 `hostWrite=true`）
- 纹理可作为渲染目标：`TextureUsage::RenderTargetAttachment` +
  `OffscreenTargetDesc.colorFromTexture(face/mip)`；GLES sampler uniform 命名 `texN ↔ slot N`
- 顶点布局约定（glTF 模型）：pos(3f)@0 | normal(3f)@12 | tangent(4f)@24 | uv(2f)@40，
  交错 stride 48，location 0/1/2/3
- UBO 约定：slot0=FrameUBO(256B:viewProj|cameraPos|lightDir|lightColor|sh[9])，
  slot1=ItemUBO(256B 步进:mvp|world|normalMatrix|factors|uvTransform)；
  GLES uniform block 名表：UBO/FrameUBO→0，ItemUBO→1
- 纹理槽位：0=baseColor，1=MR，2=normal，3=emissive，4=occlusion，5=prefilterCube，
  6=brdfLut(nearest 采样)
- cubemap 方向约定：GL/Khronos(u 右向、v 顶向下)，环境生成/预滤波/采样三处必须一致
- 深度：离屏目标 `OffscreenTargetDesc.depth=true` + pipeline `depthTest/depthWrite`；
  depth 管线须配 depth 目标；CompareOp 默认 Less（Reverse-Z 预留）
- renderer 层 per-item UBO 步进 256B（三后端对齐最小公倍）；渲染循环见
  `tests/renderer/renderer_test.cpp` 的 beginFrame/beginScene/collect/endScene/submit/endFrame 顺序
- shader 内嵌：embedded_shaders.cpp 自动生成（host=build 期；Android/iOS=configure 期），勿手改；
  iOS 真机/模拟器 metallib 分别编译（RD_EMBED_IOS_METAL / RD_EMBED_IOS_SIMULATOR）
- Metal swapchain 颜色格式为 BGRA8（layer 限制）；pipeline 格式须经 swapChainColorFormat 对齐

## 提交规范
- 小步提交，每任务一个 commit；格式 `<type>(<scope>): 描述`
  （feat/fix/build/test/chore/docs）
