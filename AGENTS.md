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
- 阶段二 c 完成：Orbit 手势(旋转/pinch/平移/双击重置+惯性阻尼)+ 画质分级
  (三档预设+caps 启发式+C API)+ KTX2(libktx,ASTC/ETC2/RGBA32 兜底)+ 上屏链
  (SceneTarget→blit upscale)+ engine 迁移 Renderer 链 + render_test --interactive;
  RHI 扩展:ASTC/ETC2 压缩格式 + MSAA/resolve 三后端 + Vulkan 描述符按绑定状态缓存
- P2-1 完成：单方向光阴影(depth-only 目标 + 硬件比较采样 + PCF 3x3,包围球自动取景)
  + KHR_lights_punctual 多光源(dir/point/spot×4,glTF 解析/C API 双通道)
  + 画质档 shadowMapSize(2048/1024/0)
- P2-2 完成：HDR 后处理链(High/Mid:RGBA16F SceneTarget + Bloom 3 级 tent 模糊
  + ACES composite;Low:FXAA 兜底;R16F 格式 + hdr_render_target caps;
  场景管线按目标格式/采样数匹配重建)
- P2-3 完成：骨骼动画(节点层级/skins/animations 解析 + GPU 蒙皮
  pbr_forward_skinned + JointUBO slot3 调色板 + Animator 播放/交叉淡入
  + C API play/crossfade/pause,load_gltf 自动播放 clip0)
- 下一步：P2 余下(拾取/性能基准)

## 构建与测试
```bash
brew install molten-vk cmake   # 一次性（注意公式名是 molten-vk）
./scripts/check.sh            # 配置 + 构建 + 全部测试
```
- 更新 golden image：`RD_UPDATE_GOLDENS=1 ctest --test-dir build -R Cube`，
  然后目视核对 tests/golden/*.png 再提交
- 手动渲染：`./build/tools/render_test/render_test --backend metal|vulkan --out cube.png`
- 交互调试：`./build/tools/render_test/render_test --interactive --model <glb>`
  （GLFW 窗口,Metal;拖拽旋转/滚轮缩放/双击重置;`RD_INTERACTIVE_FRAMES=N` 冒烟退出）

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
- 压缩纹理：`Format::ASTC_4x4_UNORM/ETC2_RGBA8_UNORM`；caps `texture_compression_astc/etc2`
  门控；上传/更新按 `formatMipBytes(f,w,h)`（block 上取整）计算，非压缩路径不变
- 离屏目标可采样：`Device::targetColorTexture(target)`（MSAA 目标返回 resolve 纹理，
  texture-backed 返回源纹理，swapchain 返回无效）；`targetSize` 查询尺寸；
  `OffscreenTargetDesc.sampleCount>1` 创建 MSAA+resolve（texture-backed 不支持）
- 上屏链：`Renderer::endScene` 两段（场景→内部 SceneTarget[尺寸×renderScale,MSAA 按画质档,
  带 depth] → blit upscale pass→最终目标）；blit 纹理槽 0、UBO 块名 BlitUBO→slot0；
  GLES 渲染到纹理的 v 方向由 BlitUBO.params.x 翻转吸收（Metal/Vulkan 传 0）
- 画质：`rd_engine_set/get_quality(AUTO/HIGH/MID/LOW)`；AUTO=caps 启发式
  (msaa≥4 且 max_texture_size≥8192→High;msaa≥2→Mid;否则 Low)；
  预设表 renderer/quality.h(renderScale/msaa/IBL 尺寸/纹理上限)
- 输入 C API：`rd_engine_on_pointer/on_scroll/on_pinch/on_double_tap`（像素坐标,左上 origin）；
  `rd_engine_load_gltf` 同步加载并 Orbit 自动取景（v1 同步,异步归 P2）
- KTX2：glTF `KHR_texture_basisu` + 外链 URI（相对 gltf 目录）;转码目标
  astc>etc2>rgba32 由 caps 推导（pickTranscodeTarget）;测试资产运行时生成
  （tests/common/ktx2_gen,勿提交二进制）;Vulkan 描述符按绑定状态缓存
  （bind 只记状态、draw 时绑定,支持逐 draw 异构绑定）
- 依赖弱网旁路：`$ENV{RD_DEPS_MIRROR}/ktx|glfw` 指向本地源码副本可跳过 FetchContent 下载
- LightUBO=slot2(352B:lightViewProj|shadowParams|lightCount|lights[4×64B]);
  GLES 块名 LightUBO→2、ShadowUBO→0;阴影纹理=slot 7(比较采样器 sampler2DShadow)
- 阴影:ShadowPass 在场景 pass 前(endScene 内);depth-only 目标
  (`OffscreenTargetDesc.depthFromTexture` + `PipelineDesc.depthOnly`);
  bias 走 shader(常量+slope);GLES 阴影 UV 的 v 翻转由 shadowParams.w 吸收;
  采样器 `SamplerDesc.compareEnable`(三后端硬件比较)
- 灯光约定:direction=指向光源(dot(N,L) 直接用);color 已乘 intensity;
  手动灯(C API)非空覆盖 glTF 灯,皆空则默认 1 方向光;glTF 灯方向=节点旋转×(0,0,-1) 取反
- 画质:`QualityPreset.shadowMapSize`(0=关);`rd_engine_set_shadow_enabled` 与画质档为与关系;
  阴影取景 `Renderer::setLightFraming(center, radius)`(包围球正交,光源方向取首盏方向光)
- 后处理:post 开=HDR(LightUBO `lightCount.y`=hdrMode,pbr 线性输出到 R16F SceneTarget)走
  extract(半分)→l1/l2/l3 tent 模糊→composite(w=1.0/0.6/0.4+ACES+gamma)直出;
  post 关=LDR(Reinhard 在 pbr);FXAA 与 MSAA 互斥(仅 msaa==1 的 Low 档);
  post pass 均复用 blit.vert 全屏三角形;composite 槽位 0=scene,1..3=bloom l1..l3;
  参数 UBO x=vFlip,yz=texel(逐 pass 独立小 UBO,勿跨 pass 复用——录制期覆写问题)
- 格式:`Format::R16G16B16A16_FLOAT`(8B/px);caps `hdr_render_target`(GLES 查 EXT);
  **场景管线(pbr/unlit)按 SceneTarget 格式/采样数匹配重建**(ensureScenePipelines,
  管线经 RenderContext 在 record 时注入,勿在构造渲染项时固化)
- 蒙皮:顶点布局 80B(48B + joints4f@48|weights4f@64,location 4/5);
  JointUBO=slot3(64KB 共享,8 项×8192B 步进,超 128 骨截断告警);
  jointMatrices[j] = nodeGlobals[joints[j]] × IBM[j];
  蒙皮阴影用 shadow_depth_skinned;法线蒙皮用 mat3(skin) 近似;
  **场景管线含 skinned 变体,首帧 ensureScenePipelines 统一重建(pipeSamples_ 初始 0)**
- Animator:clip 线性插值(rotation slerp),STEP 退化保持;节点父先子后序依赖
  (反序模型已知限制);C API play/crossfade/pause;load_gltf 自动播放 clip0

## 提交规范
- 小步提交，每任务一个 commit；格式 `<type>(<scope>): 描述`
  （feat/fix/build/test/chore/docs）
