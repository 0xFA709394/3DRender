# 阶段二c 设计:Orbit 手势 + 画质分级 + KTX2

日期:2026-08-16
状态:已确认(用户审阅通过)
前置:总设计 docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md;阶段二b(PBR/IBL)已完成

## 0. 目标与范围

阶段二c 一次收尾三个子项,让 demo 可交互、可落地移动端:

1. **Orbit 手势**:单指旋转 / 双指 pinch 缩放 / 双指平移 / 双击重置,带阻尼惯性;
   平台(iOS/Android)接线触摸事件;host 侧 render_test 加鼠标交互(GLFW 窗口)。
2. **画质分级 v1**:四旋钮全做——渲染分辨率缩放、MSAA 档位、IBL 质量(prefilter 尺寸)、
   纹理解码尺寸上限;高/中/低三档预设 + caps 启发式默认 + 业务手动覆盖(C API)。
3. **KTX2**:libktx 接入 resource 层;转码目标 ASTC + ETC2 + RGBA32 兜底(不引入 BC 格式);
   glTF `KHR_texture_basisu` 支持。

顺带补齐:C API 模型加载(`rd_engine_load_gltf`),双端 demo 换 PBR 模型(DamagedHelmet)。

非目标(留 P2/P4):阴影、多光源、后处理链(upscale pass 预留挂载点)、骨骼动画、
异步加载、BasisU 之外的 supercompression、BC 格式、HDR/tone mapping。

## 1. RHI 底座扩展(先行,契约测试门控)

### 1a. 压缩纹理格式

- `Format` 新增:`ASTC_4x4_UNORM`、`ETC2_RGBA8_UNORM`(只做这两个 block 规格)。
- caps 新增:`texture_compression_astc`、`texture_compression_etc2`(0=不支持)。
- 新工具函数 `formatBlockInfo(f) -> {blockW, blockH, bytesPerBlock}`;
  非压缩格式 block=1x1。上传/createTexture/updateTexture 对压缩格式按 block 计算
  数据量并校验尺寸按 block 对齐;`formatSize()` 对压缩格式不再被上传路径使用。
- `TextureDesc` 约定不变:data 逐 mip 紧凑排列;压缩格式每 mip 尺寸按 block 上取整。
- 后端映射:
  - Metal:`MTLPixelFormatASTC_4x4_LDR`;caps:iOS 全系 / Apple Silicon Mac 支持,
    Intel Mac 不支持(0);ETC2 恒 0。
  - Vulkan:`VK_FORMAT_ASTC_4x4_UNORM_BLOCK`(feature `textureCompressionASTC_LDR` 门控,
    MoltenVK on Apple Silicon 支持);`VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK`
    (MoltenVK 不支持 → 恒 0)。
  - GLES:ETC2 = `GL_COMPRESSED_RGBA8_ETC2_EAC`(ES3 core,恒支持);
    ASTC 查 `GL_KHR_texture_compression_astc_ldr` 扩展。
- 契约测试:2-mip 压缩纹理上传 → 采样渲染 → golden;按 caps 跳过不支持的后端。
  测试资产:预生成小 .ktx2(见 §2)或裸压缩数据。

### 1b. MSAA + resolve

- `PipelineDesc.sampleCount` 放开:>1 须 ≤ `caps().get(msaa)`,否则创建失败(空句柄+日志)。
- `OffscreenTargetDesc` 新增 `sampleCount`(默认 1):>1 时创建 MSAA 颜色附件 +
  单采样 resolve 纹理;渲染到该目标的 pass 结束时自动 resolve;depth 附件同步 MSAA
  (不可采样,store 丢弃)。
- OffscreenTarget 暴露 resolve 纹理查询(供 upscale pass 采样;非 MSAA 返回颜色附件自身)。
- 后端:
  - Vulkan:render pass 加 color resolve attachment;MSAA image storeOp→resolve;
    depth MSAA image storeOp dontCare。
  - Metal:MSAA 颜色纹理 + `MTLStoreActionMultisampleResolve`,resolveTexture 挂附件。
  - GLES:multisample renderbuffer(`glRenderbufferStorageMultisample`)绑 FBO,
    pass 结束 `glBlitFramebuffer` 到纹理 FBO(resolve 目标才是纹理)。
- caps `msaa` 上报后端实际最大值(画质层 clamp 到 4)。
- 契约测试:MSAA 三角形(斜边覆盖度 vs 单采样),caps 允许时跑。

## 2. KTX2 资源链

- Deps.cmake:FetchContent KTX-Software,裁剪 `KTX_FEATURE_TOOLS OFF`、
  `KTX_FEATURE_TESTS OFF`、`KTX_FEATURE_STATIC_LIBRARY ON`,静态链接 `ktx`;
  全平台(含 Android/iOS,iOS toolchain 兼容性 plan 时验证)。
- 新 `resource/ktx2_codec.{h,cpp}`:
  `decodeKtx2(data, size, target) -> {format, width, height, mipLevels, 逐 mip 紧凑数据}`;
  内部 `ktxTexture2_CreateFromMemory` → supercompressed 则 `ktxTexture2_TranscodeBasis`;
  target 由调用方按 device caps 选(astc > etc2 > rgba32 兜底)。
- `gltf_loader` 接 `KHR_texture_basisu`:按 KTX2 魔数(`«KTX 20»`)分流到 ktx2_codec;
  transcode 目标经加载链路参数传入(loader 不直接碰 device)。
- `image_codec` 加 `maxDim` 参数:超限等比降采样(stb_image_resize2,stb 已 FetchContent)。
- 测试资产:预生成 BasisU .ktx2(含 mip)+ 引用它的 glb(KHR_texture_basisu),
  提交 tests/assets(小二进制)。
- golden:KTX2 模型渲染 golden(metal+vulkan),有损压缩 → 感知差异阈值比对
  (不做像素级一致);`render_test --model` 手动验证。

## 3. renderer 上屏链 + 画质分级

### 3a. 上屏链(render scale + MSAA + upscale pass)

- `Renderer::endScene(cmd, target)` 内部两段:
  1. 场景 → 内部 SceneTarget(尺寸 = target 尺寸 × renderScale,sampleCount 按档位,带 depth);
  2. upscale pass:全屏三角形采样 SceneTarget 的 resolve 纹理 → 画到最终 target
     (swapchain 或离屏)。
- SceneTarget 参数(尺寸/scale/sampleCount)变化时重建;离屏 golden 路径同样走 upscale
  (路径统一,恒等拷贝开销忽略)。
- 新 `blit.vert/blit.frag`(全屏三角形,无顶点缓冲,tex slot0,线性采样),
  走既有 embedded_shaders 管线;renderer 持有 sceneTarget_/blitPipeline_/blitSampler_。
- 保持 LDR 链(HDR/tone mapping 是 P2);upscale pass 即 P2 后处理链挂载点。

### 3b. 画质分级

- `renderer/quality.h`:`QualityTier { High, Mid, Low }` +
  `QualityPreset { renderScale, msaa, iblPrefilterSize, maxTextureDim }`,预设初值
  (plan 阶段可调):
  | 档 | renderScale | MSAA | prefilter 尺寸 | 纹理上限 |
  |---|---|---|---|---|
  | High | 1.0 | 4 | 256 | 4096 |
  | Mid | 0.75 | 2 | 128 | 2048 |
  | Low | 0.5 | 1 | 64 | 1024 |
- `Renderer::setQuality(preset)`:renderScale/msaa 在 endScene 重建 SceneTarget 时生效;
  iblPrefilterSize 触发 Environment 预滤波链重建;maxTextureDim 经加载链传到解码层。
- 自动默认档启发式(caps):msaa≥4 且 max_texture_size≥8192 → High;msaa≥2 → Mid;
  否则 Low。保守起步,业务可覆盖。
- C API:`rd_engine_set_quality(RD_QUALITY_HIGH/MID/LOW/AUTO)`(AUTO=启发式默认,
  初始状态)+ `rd_engine_get_quality`。

## 4. Orbit + C API + 平台接线

### 4a. scene/orbit_controller.{h,cpp}

- 状态:target(注视点)、distance、yaw、pitch(±89° clamp)、惯性速度、阻尼系数。
- 输入:归一化指针事件 `onPointerDown/Move/Up(id, x, y)`、`onScroll(delta)`、
  `onDoubleTap(x, y)`;双指:两指针距离比 → pinch 缩放 distance,质心位移 → pan。
- `update(dt)`:惯性指数阻尼衰减;产出 eye/center 写入 Camera。
- 纯数学,单测喂合成事件:拖拽像素→yaw 变化量、pinch 2×→distance 减半、
  惯性收敛、pitch clamp。

### 4b. C API 新增

- `rd_engine_on_pointer(engine, RD_POINTER_DOWN/MOVE/UP, id, x, y)`
  (像素坐标,引擎内按 surface 尺寸归一化)。
- `rd_engine_on_scroll(engine, delta)`、`rd_engine_on_double_tap(engine, x, y)`。
- `rd_engine_load_gltf(engine, path) -> rd_result_t`:v1 同步加载(异步留 P2 任务队列);
  加载成功后替换演示场景(立方体)为 glTF 场景。

### 4c. 平台接线

- iOS `RenderView.swift`:UIPan/UIPinch/UITap(double)手势识别,主线程直调 C API
  (符合线程约定);demo 换 DamagedHelmet.glb(bundle 资产)。
- Android RenderView:GestureDetector + ScaleGestureDetector,事件投递到渲染线程
  (对齐现有投递机制);demo 同样换模型。
- 两端模拟器截图校验沿用 img_check。

### 4d. render_test 交互模式

- `render_test --interactive`:GLFW 窗口 + Vulkan(MoltenVK)swapchain;
  GLFW 鼠标回调 → C API pointer/scroll 事件(顺带验证 host swapchain 与 C API);
  Metal host 窗口(glfwGetCocoaWindow 挂 CAMetalLayer)为 stretch 项。
- headless 路径完全不变(CI/golden 不受影响)。
- Deps.cmake 加 GLFW(仅 host)。

## 5. 错误处理

- 压缩格式不被后端支持:createTexture 失败 → resource 层以 RGBA32 target 重转码兜底 + 告警。
- MSAA 目标创建失败(caps 不足已被画质层 clamp 规避;仍失败)→ fallback sampleCount=1 重建。
- KTX2 文件损坏/不支持特性:loader 返回空 + 日志。
- `rd_result_t` 新增 `RD_ERROR_ASSET`:资产加载/解析失败统一错误码
  (`rd_engine_load_gltf` 路径非法/解析失败时返回,细节经 rd_get_last_error)。

## 6. 测试与验证矩阵

| 层 | 内容 |
|---|---|
| 单测 | orbit 数学(合成事件)、quality 预设/clamp、formatBlockInfo、transcode 目标选择 |
| RHI 契约 | 压缩纹理上传采样(caps 门控)、MSAA 渲染 resolve |
| golden | DamagedHelmet KTX2 版(双后端,感知阈值)、Low 档(0.5×+无 MSAA)渲染 |
| 手动 | render_test --interactive 鼠标;iOS/Android 模拟器手势 + img_check 截图 |

## 7. 实施串行顺序

1. RHI:压缩格式 + MSAA/resolve(契约测试先行)
2. KTX2 资源链(libktx + loader + golden)
3. renderer 上屏链 + 画质分级
4. OrbitController + C API + 平台接线
5. render_test --interactive + 双端 demo 换 PBR 模型

每步独立可测、独立提交(提交规范:每任务一个 commit)。收尾更新 AGENTS.md 新约定
(压缩格式 caps、MSAA 目标语义、新 C API、画质档约定)。
