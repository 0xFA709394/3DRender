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
- P2-4 完成：拾取交互(scene::picking 三角形精确求交 + rd_engine_pick 同步结构体;
  蒙皮按绑定姿态——已知限制)
- P2-5 完成：性能基准套件(perf_test 4 场景×双后端,avg/p50/p95/p99/FPS,
  基线 JSON + p50 超 2× 软门槛,--update-baseline 更新;ctest 冒烟注册)
- P2 全部完成(阴影+多光源/后处理链/骨骼动画/拾取/性能基准)
- 场景示例集合完成:primitives 几何生成器(球/平面/盒)+ 5 程序场景
  (material_balls/cornell_box/light_playground/skinned_demo/instanced_field)
  + 知名场景(sponza/cesium_man,scripts/fetch_assets.sh 下载,assets/ 不入库)
  + render_test --scene + interactive --scene 直驱 + 移动 demo 画质/模型切换
- 场景库扩充:新增 emissive_bloom/normal_map_wall(程序化法线贴图)/shadow_gallery/
  ktx2_gallery(KTX2 vs PNG 对比)/alpha_blend(glTF alphaMode BLEND 混合管线+排序)/
  fox_anim(真骨骼动画)
- golden 三件套(F3D 学习落地):SSIM 主判据(compareSSIM,阈值 0.05)+
  RD_GOLDEN_TEST 声明式宏 + 输入注入回放(render_test --record/--play +
  tests/recordings/orbit_drag.log 手势回归);移动端录制同格式(demo 长按「切换」)
- 声明式选项系统(F3D 落地):core/api/options.json 唯一事实源;
  cmake/GenOptions.cmake 纯 CMake 生成 options_generated;`rd::Options` 强类型 +
  字符串反射 get/set/reset/domainJson;C API rd_options_count/name + set/get_option;
  engine render_frame 每帧映射进 Renderer(默认=现状零回归);
  composite 曝光经独立 compositeUbo_(逐 pass 独立 UBO)
- 命令总线+按需渲染(F3D 落地):core/api/command_bus;C API rd_engine_exec_command
  + command_output;内建命令=选项族(set/get/increase/decrease/cycle/toggle,
  域驱动 optionsRange/optionsEnumValues)+ 引擎族(load_model/play_animation/...);
  render_frame 干净时零 GPU 跳过(脏源:选项/命令/模型/输入/画质;
  Orbit 惯性或动画播放期间持续渲)
- IBL 预滤波缓存(F3D 落地):FNV-1a 内容哈希(env 源像素+size+mips);
  `<cache_dir>/ibl/<hash>.ibc`(原子写/坏文件拒绝);默认关,
  `rd_engine_set_cache_dir` 开启;缓存模式预滤波走离屏读回+updateTexture
  (三后端统一);BRDF LUT 为 CPU 纯函数不缓存;ctest ibl_cache_miss/hit/equal 双跑
- loader 补强:非索引/strip 分解(交替绕序)+ 法线缺失时逐面 flat 生成(Fox 可用)
- 下一步:P3(AR + 鸿蒙)或 P4(打磨:包体积/资产规范/性能调优)

## 构建与测试
```bash
brew install molten-vk cmake   # 一次性（注意公式名是 molten-vk）
./scripts/check.sh            # 配置 + 构建 + 全部测试
```
- 更新 golden image：`RD_UPDATE_GOLDENS=1 ctest --test-dir build -R Cube`，
  然后目视核对 tests/golden/*.png 再提交
- 手动渲染：`./build/tools/render_test/render_test --backend metal|vulkan --out cube.png`
- 性能基准：`./build/tools/perf_test/perf_test --backend all`(软门槛 p50>2× 判败);
  更新基线 `--update-baseline`(核对数值合理后提交 tests/perf/baseline.json);
  CI 噪声大用 `--no-gate` 只采集
- 场景示例：`./build/tools/render_test/render_test --scene <name> --out x.png`
  (5 程序场景 + sponza/cesium_man);交互 `--interactive --scene <name>`;
  知名资产 `./scripts/fetch_assets.sh` 下载(assets/ gitignore 不入库):
  CesiumMan/Fox/Duck/BoomBox/WaterBottle/Corset/Lantern/sponza;
  iOS demo bundle 自动收编已下载 glb(状态机 bundle 动态扫描轮换)
- golden 判据 = SSIM(`compareSSIM`,阈值默认 0.05);pixel diffRatio 仅辅助日志;
  golden 用例一律 `RD_GOLDEN_TEST` 宏;新增场景只写 renderFn(Image 返回)
- 输入回放:`./build/tools/render_test/render_test --interactive --play
  tests/recordings/orbit_drag.log`;录制 `--record <path>`(归一化坐标文本行);
  移动端录制:demo 长按「切换」起停,日志在 app 文档目录 rd_input.log
- 选项:core/api/options.json 唯一事实源(改 JSON 自动重生成);
  C API `rd_engine_set_option(engine, name, value)` / `get_option`;
  名表 `rd_options_count/name`;新增选项只需加 JSON 条目 + 在 applyOptions 接线
- 命令总线:`rd_engine_exec_command(engine, "set render.exposure 2.0")` /
  `rd_engine_command_output`(get 输出);内建 set/get/increase/decrease/cycle/toggle
  + load_model/play_animation/crossfade_animation/pause_animation/quality/reset_view
- 按需渲染:render_frame 干净(无脏/无动画/无惯性)时零 GPU 跳过;
  `rd_engine_request_render` 手动置脏;新增状态变更 API 须置脏
- 命令脚本:`rd_engine_exec_script(path)`(每行一命令,# 注释,错误即停);
  render_test `--interactive --script <path>`
- 选项持久化:`rd_engine_save_options/load_options`(扁平 JSON 名值对;
  原子写;未知名跳过向前兼容)
- 视锥剔除(P4 性能):endScene 排序后按包围球×world 测 6 平面;
  场景 pass 用相机 VP、阴影 pass 用光源 VP(屏外物体可向屏内投影);
  ItemUBO 槽位=两可见集并集 slotOf 映射;蒙皮项跳过(动态包围);
  选项 render.frustum_culling(默认开);perf sponza 基准场景(fetch_assets 下载,缺失跳过)
- 自动实例化(P4 性能):场景 pass 同 MeshRenderResource 相邻可见项(非蒙皮/非 blend,
  组≥2)合并 drawIndexedInstanced;shader=pbr_forward_instanced.vert/.frag
  (ItemUBO 声明为 items[64] 数组,gl_InstanceIndex 索引;宿主 bind 组首槽偏移+组大小);
  desc.instancedVs/Fs 空=不启用;阴影 pass 不分组(v2);golden 像素与逐项一致
- IBL 缓存:`rd_engine_set_cache_dir(path)` 开启(默认关);
  render_test `--cache-dir <path>`;缓存键=env 源像素+size+mips
- HDR 环境+天空盒:.hdr equirect(stb float)→ equirect_to_cube pass(RGBA16F)
  → SH9(equirect 立体角加权投影)+ 16F 预滤波;程序化模式保持 RGBA8 零回归;
  HDR 模式 v1 无磁盘缓存(readbackTarget 仅 RGBA8,v2 待 readback16F);
  天空盒=场景 pass 内首画(depthTest/Write 关,slot5 prefilterCube mip0,
  LDR Reinhard 与 pbr 一致);选项 env.skybox(默认关)/env.yaw_deg(烘进 equirect pass,
  值变触发环境重建;程序化模式忽略);
  `rd_engine_set_environment_hdri(path)`/`set_environment_procedural()`;
  golden: hdr_env_{metal,vulkan}.png(运行时生成渐变 hdr)
- pipeline 缓存(F3D 落地):`Device::setPipelineCachePath`(Vulkan VkPipelineCache
  blob/Metal MTLBinaryArchive[macOS 11+/iOS 14+ 门控;Apple Silicon 系统 shader 缓存
  命中时 archive 收集不到二进制,落盘跳过为良性]/GLES no-op);
  `rd_engine_set_cache_dir` 一旋钮同开 IBL+pipeline(<dir>/pipelines/<backend>.bin);
  析构落盘;ctest pipeline_cache_* 双跑
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
- iOS 部署目标：**ktx CMakeLists 会强设 `CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET=11.0`
  (CACHE 全局,污染所有目标;std::filesystem 需 13+)**——Deps.cmake 在拉取后覆盖回 16.0;
  toolchain 的 CMAKE_OSX_DEPLOYMENT_TARGET 须 CACHE FORCE(project() 平台初始化回填普通 set)
- LightUBO=slot2(352B:lightViewProj|shadowParams|lightCount|lights[4×64B]);
  GLES 块名 LightUBO→2、ShadowUBO→0;阴影纹理=slot 7(比较采样器 sampler2DShadow)
- 阴影:ShadowPass 在场景 pass 前(endScene 内);depth-only 目标
  (`OffscreenTargetDesc.depthFromTexture` + `PipelineDesc.depthOnly`);
  bias 走 shader(常量+slope);GLES 阴影 UV 的 v 翻转由 shadowParams.w 吸收;
  采样器 `SamplerDesc.compareEnable`(三后端硬件比较)
- 灯光约定:direction=指向光源(dot(N,L) 直接用);color 已乘 intensity;
  手动灯(C API)非空覆盖 glTF 灯,皆空则默认 1 方向光;glTF 灯方向=节点旋转×(0,0,-1) 取反
- 画质:`QualityPreset.shadowMapSize`(0=关);`rd_engine_set_shadow_enabled` 与画质档为与关系;
  阴影取景 `Renderer::setLightFraming(center, radius)`(包围球正交,光源方向取首盏方向光);
  **MSAA 采样数须经 `Device::snapSampleCount` 对齐**(MTLSimDriver 只支持 4x,拒绝 2x;
  Vulkan 按 framebufferColorSampleCounts 位掩码;目标与管线须用同一对齐值)
- 纹理存储:Metal D32 一律 Private(iOS 禁 Shared,CPU 不可写)→ D32 占位图用
  "清屏初始化"(depth-only 目标 clear),勿带初始数据创建;
  Environment 预滤波管线格式恒 RGBA8(与渲染目标格式无关)
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
- 拾取:`scene::picking` screenRay(屏幕 y 翻转 NDC)+ pickModel(world 逆变换入模型空间,
  Möller–Trumbore;包围球随调随算预筛;48B/80B 布局兼容);
  `rd_engine_pick` 无 surface 时用 512×512 默认投影
- 混合:glTF alphaMode=BLEND → MaterialData.alphaBlend;混合管线(BlendDesc srcAlpha/
  oneMinusSrcAlpha,depthWrite 关);endScene opaque 先、blend 按视距远→近;
  blend 项不参与蒙皮路径(按 opaque 处理)
- MASK cutout:glTF alphaMode=MASK → MaterialData.alphaCutoff(默认 0.5)→
  ItemUBO metallicRough.w;pbr.frag `if (w>0 && alpha<w) discard`;
  走 opaque 路径(depthWrite 开);**阴影 pass 支持裁剪**(shadow_depth_mask 管线:
  采样 baseColor alpha discard;mask 材质不参与阴影实例化分组)
- 阴影实例化:lightVis 同资源相邻组(非蒙皮/非 mask,≥2)→ shadow_depth_instanced.vert
  + drawIndexedInstanced(ItemUBO 组偏移绑定)
- 包体积(P4):`tools/glb_ktx2`(glb→ASTC 4x2 KTX2:cgltf/nlohmann 解析,逐图像
  stb 解码→maxDim 1024 降采样→libktx CompressAstcEx→全量重排 BIN+JSON;
  **无收益图像保留原样**;BoomBox 10.1→3.3MB,demo 包 49→22MB);
  iOS demo 优先 `<m>.ktx2.glb`;loader 内嵌 KTX2 bufferView 走 magic 嗅探(无需扩展声明)
- loader:非索引图元顺序生成索引;triangle_strip 分解为三角形列表(交替绕序);
  法线缺失时逐面 flat 生成(须在索引生成之后)

## 提交规范
- 小步提交，每任务一个 commit；格式 `<type>(<scope>): 描述`
  （feat/fix/build/test/chore/docs）
