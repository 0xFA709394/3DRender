# 移动端 3D 渲染器框架设计文档

- **日期**: 2026-08-09
- **状态**: 已评审
- **目标平台**: iOS / Android（二期扩展鸿蒙 HarmonyOS NEXT）

## 1. 背景与目标

设计一套自研移动端 3D 渲染框架，供业务方以原生 View 嵌入方式接入，覆盖以下用例：

- **商品/物品 3D 展示**：电商详情页 360° 查看，单模型 + 环境光照 + 手势交互
- **通用 3D 场景渲染**：多物体、相机漫游、动画
- **数字人/形象渲染**：骨骼蒙皮动画、表情（后续）
- **AR 场景叠加**（二期）：ARCore/ARKit 结合，虚拟物品入真实环境

### 已确认的关键决策

| 决策点 | 结论 |
|---|---|
| 技术路线 | 自研 C++17 跨平台内核，双端共享 |
| RHI 后端 | Vulkan / Metal / OpenGL ES 3.0 三后端一步到位 |
| MVP 渲染特性 | PBR+IBL、骨骼动画、阴影+多光源、后处理，全部包含 |
| 业务接入 | 原生 View 嵌入（iOS UIView/SwiftUI，Android View/Compose） |
| AR 能力 | 放第二阶段（P3），首版专注渲染内核 |
| 架构组织 | 分层架构 + 场景图（渲染层内部用 RenderGraph 组织 pass） |

### 非目标（YAGNI）

- 不做游戏引擎级功能：无物理引擎、无粒子系统编辑器、无导航寻路
- 不做 ECS：场景图足够覆盖目标用例，API 易用性优先
- 不做全插件化内核：避免过度工程，插件化思想仅用于渲染层内部 pass 组织
- 不做 Web 端
- AR 不做自研 SLAM：直接基于 ARCore/ARKit

## 2. 整体架构

### 2.1 分层结构

```
┌────────────────────────────────────────────────────┐
│ 平台绑定层  iOS: RenderView(UIView/SwiftUI)          │
│            Android: RenderView(View/Compose)        │
│            (二期) 鸿蒙: ArkTS 组件                    │
├────────────────────────────────────────────────────┤
│ FFI 边界    统一 C API (extern "C"，三端复用同一套)    │
├────────────────────────────────────────────────────┤
│ scene      场景图 | 骨骼动画 | 相机与手势 | 拾取         │
│ renderer   前向渲染管线 | 材质系统 | IBL | 阴影 | 后处理     │
│ resource   glTF 加载 | KTX2 纹理 | 压缩几何 | 缓存      │
│ rhi        抽象接口 + Vulkan/Metal/GLES 三后端         │
│ foundation 数学 | 内存 | 线程/任务 | 日志 | 句柄         │
└────────────────────────────────────────────────────┘
```

依赖方向只允许向下；上层不知晓下层实现细节，只依赖接口。

### 2.2 模块职责

| 模块 | 职责 | 关键类型 | 不做什么 |
|---|---|---|---|
| `foundation` | 数学库、内存分配器、线程/任务、日志、类型安全句柄 | `Mat4` `Handle<T>` `TaskQueue` | 不知晓任何图形概念 |
| `rhi` | GPU 资源与命令的跨后端抽象 | `Device` `Buffer` `Texture` `Pipeline` `CommandBuffer` `SwapChain` | 不知晓场景/材质语义 |
| `resource` | 资产解码与缓存：glTF、KTX2、Draco/meshopt、异步加载 | `ModelAsset` `TextureCache` `AssetLoader` | 不做渲染决策 |
| `renderer` | 把场景转成 GPU 命令：材质、光照、阴影、后处理、RenderGraph | `Material` `RenderGraph` `LightManager` | 不管资产从哪来 |
| `scene` | 场景组织与动画：节点树、变换、相机、蒙皮、拾取 | `Node` `Camera` `Animator` | 不直接碰 GPU API |
| `api` | 稳定 C ABI 边界，供三端 FFI 复用 | `rd_engine_create()` 等 | 不含业务逻辑 |

### 2.3 第三方依赖

全部宽松开源协议、移动端验证过、源码静态编译进 SDK：

| 库 | 用途 |
|---|---|
| glm | 数学库 |
| cgltf | glTF 2.0 解析（单文件 C 库） |
| KTX-Software | KTX2/Basis 压缩纹理（ASTC/ETC2 运行时选格式） |
| Draco + meshoptimizer | 几何压缩解码 |
| glslang + SPIRV-Cross | shader 离线编译与跨语言转换 |
| volk | Vulkan 函数加载 |
| googletest | 测试（仅开发期） |

### 2.4 仓库结构

```
3DRender/
├── cmake/                # CMake 工具链与依赖获取
├── core/                 # C++ 内核 (CMake 构建)
│   ├── foundation/  rhi/  resource/  renderer/  scene/  api/
│   └── rhi/backends/{vulkan,metal,gles}/
├── platform/
│   ├── ios/              # Swift/ObjC 绑定 + RenderView + SwiftUI 封装 (SPM/CocoaPods)
│   ├── android/          # Kotlin 绑定 + RenderView + Compose 封装 (AAR/Maven)
│   └── harmony/          # 二期：ArkTS/NAPI 绑定
├── shaders/              # 统一 GLSL 源 + 离线编译脚本
├── tools/                # 资产检查/转换 CLI
├── samples/              # iOS / Android demo app
├── tests/                # 单元测试 + golden image + 性能基准
└── docs/
```

## 3. 核心机制

### 3.1 线程模型（三线程制）

```
主线程(UI)                渲染线程                 IO/解析线程池
─────────                ─────────                ─────────────
业务调用 API                独占渲染循环              网络/磁盘读取
  │                        │                        glTF 解析
  ▼                        ▼                        纹理解码(KTX2)
场景变更写入               消费状态快照               ──┐
【线程安全队列】  ──提交──▶  遍历场景→剔除→排序          │ 完成后投递
  │                        录制 RenderGraph      ◀──┘ GPU 上传任务
  │                        执行+Present          （GPU 上传只在渲染线程）
  ▼                        ▼
无锁等待，不阻塞 UI        固定节奏驱动 vsync
```

核心规则：

1. 所有公开 API 主线程可调用；内部通过**双缓冲场景状态**同步：主线程写副本 A，渲染线程读副本 B，帧边界交换。业务方不感知线程。
2. **GPU 资源创建/销毁只发生在渲染线程**（上下文单线程化，规避移动驱动最脆弱的多线程路径）。
3. 资产 IO 与 CPU 解码在后台线程池；解码完成后把"上传任务"投递给渲染线程。
4. Android `Surface` 生命周期回调（created/destroyed/sizeChanged）转发到渲染线程处理，避免与业务调用竞争。

### 3.2 关键数据流：加载并展示一个商品模型

```
view.loadModel("https://cdn.../shoe.glb")
  ① 主线程: 生成请求 → resource 层
  ② IO 线程: 下载/读盘 → cgltf 解析 → Draco/meshopt 解码 → KTX2 转码(ASTC/ETC2)
  ③ 渲染线程: 创建 Buffer/Texture/Pipeline(GPU 资源)
  ④ 主线程回调: onLoaded(modelHandle) → scene.add(model)
  ⑤ 每帧: 场景遍历 → 视锥剔除 → 渲染项排序(按材质/pass)
       → RenderGraph 执行:
         ┌─ ShadowPass (方向光/聚光 shadow map)
         ├─ MainPass   (PBR 前向渲染 + IBL, MSAA)
         ├─ PostPass   (Bloom → ACES tone mapping → FXAA)
         └─ Present
  ⑥ 手势: 触摸事件 → OrbitController → 相机变换(写入场景状态)
```

### 3.3 Shader 跨平台管线（离线编译，运行期零编译）

```
shaders/*.vert / *.frag            (统一 GLSL 450 编写)
        │
   构建期 CMake 调用工具链:
        ├─ glslang ────────────▶ SPIR-V      → Vulkan 后端直接用
        ├─ SPIRV-Cross ────────▶ MSL         → 编译进 .metallib (Metal)
        └─ SPIRV-Cross ────────▶ GLSL ES 3.0 → 运行时加载 (GLES)
        │
   产物按后端打包 + 反射信息(JSON): 材质系统据此自动生成 uniform 布局与绑定表
```

- 材质变体（如 `USE_SKINNING`、`NUM_LIGHTS`）通过 specialization constants（Vulkan/Metal function constants）与 GLES 宏定义，**构建期全展开**，避免运行时 shader 编译卡顿。

### 3.4 错误处理（分层策略）

| 层 | 策略 |
|---|---|
| FFI 边界 | **不用异常**。所有 C API 返回错误码 + `rd_get_last_error()` 详情；异步操作通过回调携带 `Result{errorCode, message}` |
| 内核 release | **优雅降级**：纹理解码失败→灰色占位纹理；shader 变体缺失→洋红错误材质；单 pass 失败→跳过该 pass 不中断帧 |
| 内核 debug | 断言 + 详细日志（模块/级别过滤），RHI 层可选开启 Vulkan validation / Metal validation |
| 设备级灾难 | GPU context lost（Android）/ 设备重置 → 渲染线程捕获 → 自动重建 SwapChain 与 GPU 资源 → 回调业务 `onRenderContextRestored()`；连续失败 N 次 → 回调 `onFatalError()` 让业务决定降级（如隐藏 3D 入口） |
| 崩溃隔离 | 渲染循环包在受控边界内，渲染线程异常不拖垮 App 主进程，统一上报到业务崩溃回调 |

## 4. 渲染特性落地要点

| 特性 | 实现要点 |
|---|---|
| 渲染管线 | 前向渲染为主（移动端 tile-based GPU 友好）：每对象光源裁剪 + 光照数据打包进 uniform/storage buffer。若后续光源数量需求增长，演进为 Forward+ 聚类光照，RenderGraph pass 结构不变 |
| PBR 材质 | 完整兼容 glTF 2.0 metallic-roughness：baseColor/normal/metallicRoughness/emissive/occlusion 贴图；MVP 支持 `KHR_texture_transform`、`KHR_materials_unlit` 扩展；clearcoat/sheen 等二期 |
| IBL 环境光照 | irradiance cubemap + 预滤波 specular cubemap（GGX）+ BRDF LUT；业务传入 HDR 环境图，内核首次加载时预滤波并缓存 |
| 阴影 + 多光源 | 方向光 CSM（2~4 级联，视距自适应）+ 聚光/点光单 shadow map；多光源上限 8 盏（1 主方向光 + N 点/聚光），超出按距离/强度裁剪；光照数据打包进 uniform/storage buffer |
| 骨骼动画 | GPU 蒙皮（joint matrix palette 存 texture/uniform）；clip 播放、交叉淡入过渡、双动画混合；单模型骨骼上限 128，超出自动拆 drawcall |
| 后处理链 | MSAA x4 → Bloom（阈值 + 多级降采样模糊）→ ACES tone mapping → FXAA（MSAA 不可用时兜底）；后处理链为可开关 pass 序列，低端机自动降级 |
| 画质分级 | 内置高/中/低三档预设：分辨率缩放、阴影尺寸、Bloom 开关、MSAA 档位；按设备档位自动选择 + 业务手动覆盖 |

## 5. 测试策略

| 层级 | 内容 | 运行位置 |
|---|---|---|
| 单元测试 | 数学库、场景图变换、动画混合、glTF 解析、资源缓存、RenderGraph 依赖排序 | macOS/Linux CI，googletest |
| RHI 契约测试 | 同一组离屏渲染用例（三角形/纹理/混合/深度）跑三个后端，验证行为一致 | CI + 真机 |
| Golden Image | 固定场景渲染输出 vs 参考图，感知差异阈值比对（容忍 GPU 间微小差异）；覆盖 PBR/阴影/动画/后处理 | 真机农场/本地 |
| 集成冒烟 | 双端 demo app：启动→加载模型→渲染 N 帧→截图→断言无崩溃无黑屏 | CI 真机 |
| 性能基准 | 帧时间 P50/P95、内存峰值、启动耗时、包体积，每次合入对比基线防回退 | CI |

## 6. 分阶段交付

| 阶段 | 交付物 | 验收标准 |
|---|---|---|
| **P0 地基** | CMake 构建 + CI；foundation；RHI 抽象 + Vulkan/Metal/GLES 三后端；shader 离线管线；双端 RenderView 容器 | 双端 demo 显示旋转立方体，三后端 golden image 一致 |
| **P1 静态展示** | resource 层（glTF/KTX2/Draco）；PBR + IBL；相机 + Orbit 手势；画质分级初版 | demo 加载 glb 商品模型，PBR 效果对齐参考渲染器，手势流畅 |
| **P2 MVP 完整** | 骨骼动画；阴影 + 多光源；后处理链；拾取交互；性能基准套件 | 数字人骨骼动画 demo；多光源阴影场景旗舰机 60fps；MVP 特性 golden image 全量通过 |
| **P3 AR + 鸿蒙** | AR 模块（ARCore/ARKit 相机帧接入、平面锚点、光照估计，渲染层复用）；鸿蒙 RHI 后端验证 + NAPI 绑定 + ArkTS 组件 | AR 放置 demo 双端跑通；鸿蒙设备渲染 demo 上屏 |
| **P4 打磨** | 性能调优、包体积裁剪、文档站、资产制作规范、更多 KHR 扩展 | 性能/体积达标线内，接入文档完备 |

## 7. 鸿蒙扩展点（设计预埋，二期零重构）

1. **RHI 后端**：HarmonyOS NEXT 支持 Vulkan 与 GLES —— 现有后端直接编译验证，无需新写后端。
2. **FFI**：统一 C API 可被 ArkTS NAPI 调用，`platform/harmony/` 只做薄绑定。
3. **窗口系统**：鸿蒙 `XComponent`（NativeWindow）等价于 Android `Surface` —— RHI SwapChain 抽象按"外部提供原生窗口句柄"设计，三端同构。
4. **构建**：新增 `OHOS_TOOLCHAIN` CMake toolchain 文件，core 层代码零改动。

## 8. 风险与对策

| 风险 | 对策 |
|---|---|
| 三后端一步到位，P0 周期拉长 | P0 内部按 GLES→Metal→Vulkan 顺序联调（GLES 最快出图），但对外交付标准不变：三后端 golden image 全过才算完成 |
| 移动驱动碎片化（尤其 Android GLES/Vulkan） | RHI 契约测试 + 真机矩阵；能力探测集中收敛在 rhi 层，上层不直接查扩展 |
| 包体积敏感（电商场景） | 全静态链接 + strip；KTX2 替代 PNG/JPEG；按功能模块化编译开关；P4 专项裁剪 |
| 骨骼动画/后处理在低端机性能 | 画质分级自动降级；性能基准 CI 防回退 |
